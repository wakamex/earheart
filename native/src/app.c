#include "app.h"
#include "listen.h"
#include <SDL3/SDL.h>
#include "catalog.h"
#include "engines.h"
#include "history.h"
#include "loop.h"
#include "paths.h"
#include "recorder.h"
#include "settings.h"

static struct {
    eh_view view;
    eh_observer *observer;
    eh_deliver_fn *deliver;
} A;

const eh_view *eh_app_view(void) { return &A.view; }

static void tell(const char *kind, const char *a, const char *b, double number, const gs_jval *detail) {
    if (A.observer) A.observer(kind, a, b, number, detail);
}

/* ---------------- what the pipeline tells the overlay ---------------- */

static void set_status(const char *status, const gs_jval *detail) {
    SDL_strlcpy(A.view.status, status, sizeof A.view.status);
    gs_jval_free(A.view.detail);
    A.view.detail = detail ? gs_jval_clone(detail) : NULL;
    // A status is a phase change: the bar starts over, and the transcript goes with the recording.
    A.view.phase[0] = 0;
    A.view.fraction = 0;
    if (SDL_strcmp(status, "recording") && SDL_strcmp(status, "paused")) {
        SDL_free(A.view.raw);
        SDL_free(A.view.cleaned);
        A.view.raw = A.view.cleaned = NULL;
    }
    tell("status", status, NULL, 0, detail);
}

static void on_progress(const char *phase, float fraction) {
    SDL_strlcpy(A.view.phase, phase, sizeof A.view.phase);
    A.view.fraction = fraction;
    tell("progress", phase, NULL, fraction, NULL);
}

static void on_partial(const char *kind, const char *text) {
    char **slot = !SDL_strcmp(kind, "cleaned") ? &A.view.cleaned : &A.view.raw;
    SDL_free(*slot);
    *slot = SDL_strdup(text);
    tell("partial", kind, text, 0, NULL);
}

static void on_overlay_show(bool visible) {
    A.view.visible = visible;
    tell("overlay", visible ? "show" : "hide", NULL, 0, NULL);
}

static void on_notify(const char *title, const char *body, bool opens_settings) { tell("notify", title, body, opens_settings, NULL); }
static void on_history_changed(void) { tell("history", NULL, NULL, 0, NULL); }
static void on_state_changed(eh_state state) {
    eh_listen_state(state);
    tell("state", state == EH_IDLE ? "idle" : state == EH_RECORDING ? "recording" : "processing", NULL, 0, NULL);
}

/* ---------------- the recorder, as the pipeline drives it ---------------- */

static void record_start(unsigned sid, const char *device_id, double max_seconds, bool chunks, bool display, const gs_jval *live_preview) {
    A.view.sid = sid;
    A.view.notice[0] = 0;
    SDL_free(A.view.raw);
    SDL_free(A.view.cleaned);
    A.view.raw = A.view.cleaned = NULL;
    set_status("starting", NULL);
    eh_recorder_start(sid, device_id, max_seconds, chunks, display, live_preview);
}

// The overlay's close key: discard the take while recording, cancel while processing, and only
// dismiss once the text is on its way or the dictation has settled.
static void record_discard(void) {
    if (eh_recorder_active()) { eh_recorder_cancel(); return; }
    const char *s = A.view.status;
    if (!SDL_strcmp(s, "delivering") || !SDL_strcmp(s, "done") || !SDL_strcmp(s, "empty") || !SDL_strcmp(s, "error")) eh_pipeline_overlay_dismiss();
    else eh_pipeline_cancel();
}

static bool deliver(const char *text, const gs_jval *output, eh_signal *signal, eh_delivery *result, char *error, size_t error_size) {
    if (A.deliver) return A.deliver(text, output, signal, result, error, error_size);
    SDL_strlcpy(error, "This build cannot deliver text yet", error_size);
    return false;
}

static void on_live(unsigned sid, const char *notice) {
    SDL_strlcpy(A.view.notice, notice, sizeof A.view.notice);
    set_status("recording", NULL);
}

static void on_paused(unsigned sid, bool paused) { set_status(paused ? "paused" : "recording", NULL); }

// Always-on listening follows its setting, on the microphone the settings name.
static void apply_always_on(void) {
    char error[256];
    if (!eh_setting_bool("alwaysOn")) { eh_listen_stop(); return; }
    if (!eh_listen_start(eh_setting_str("audio.deviceId"), eh_setting_num("audio.maxRecordingSeconds"), error, sizeof error)) on_notify("Earheart: always-on listening could not start", error, true);
}

static void settings_changed(void *user, const gs_jval *previous, const gs_jval *current) {
    eh_models_set_custom(gs_jval_get(current, "customModels"));
    eh_pipeline_settings_changed();
    apply_always_on();
}

void eh_app_init(const char *data_dir, eh_observer *observer, eh_deliver_fn *deliver_fn) {
    A.observer = observer;
    A.deliver = deliver_fn;
    eh_loop_init();
    eh_lock();
    char path[1200], theirs[1200], own_models[1200], their_models[1200];
    SDL_snprintf(path, sizeof path, "%s" EH_SEP "settings.json", data_dir);
    SDL_snprintf(theirs, sizeof theirs, "%s" EH_SEP "settings.json", eh_electron_dir());
    eh_settings_init(path, theirs);
    eh_settings_on_changed(settings_changed, NULL);
    SDL_snprintf(path, sizeof path, "%s" EH_SEP "history.json", data_dir);
    eh_history_init(path);
    // Models: $EARHEART_MODELS when set, else this build's own folder, where downloads go, and
    // then the Electron build's, whose models are used where they are.
    SDL_snprintf(own_models, sizeof own_models, "%s" EH_SEP "models", data_dir);
    SDL_snprintf(their_models, sizeof their_models, "%s" EH_SEP "models", eh_electron_dir());
    const char *env = SDL_getenv("EARHEART_MODELS");
    const char *dirs[3];
    int n = 0;
    if (env && *env) dirs[n++] = env;
    dirs[n++] = own_models;
    dirs[n++] = their_models;
    eh_models_set_dirs(dirs, n);
    eh_models_set_custom(gs_jval_get(eh_settings(), "customModels"));
    eh_engines_init();
    static const eh_recorder_deps recorder = { eh_pipeline_captured, eh_pipeline_partial, eh_pipeline_record_cancelled, eh_pipeline_record_error, on_live, on_paused };
    eh_recorder_init(&recorder);
    static const eh_pipeline_deps pipeline = {
        set_status, on_progress, on_partial, on_overlay_show, on_notify, on_history_changed, on_state_changed,
        record_start, eh_recorder_stop, eh_recorder_cancel, eh_recorder_pause_toggle, record_discard, deliver,
    };
    SDL_snprintf(path, sizeof path, "%s" EH_SEP "stt-rtf.json", data_dir);
    eh_pipeline_init(&pipeline, path);
    apply_always_on();
    eh_unlock();
}
