// The settings window: every section on one scrolling page, an index on the left that follows the
// scroll and jumps to a section, and Save in the footer. The fields edit a draft; nothing changes
// until Save, except what is its own action (downloading or removing a model, clearing history).
// Ported from renderer/settings.js, settings.html and the handlers of main/ipc.js behind them.
// Everything here runs on the main thread, with the loop's lock held for the length of a frame.
#include "files.h"
#include "settings_window.h"
#include <math.h>
#include "app.h"
#include "catalog.h"
#include "chunked.h"
#include "commit.h"
#include "correction_window.h"
#include "defaults.h"
#include "deliver.h"
#include "gs_desk.h"
#include "fonts.h"
#include "gs_draw.h"
#include "engines.h"
#include "hf.h"
#include "history.h"
#include "hotkeys.h"
#include "loop.h"
#include "model_jobs.h"
#include "paths.h"
#include "remote.h"
#include "settings.h"
#include "ui.h"
#include "wav.h"
#include "wizard_window.h"

enum { SEC_GENERAL, SEC_LIVE, SEC_STT, SEC_CLEANUP, SEC_HISTORY, SEC_ADVANCED, SECTIONS };
static const char *const section_names[SECTIONS] = { "General", "Live transcript", "Speech-to-text", "Cleanup", "History", "Advanced" };
static const char *const section_ids[SECTIONS] = { "general", "live", "stt", "cleanup", "history", "advanced" };
static const unsigned section_colours[SECTIONS] = { 0x79a7e2, 0x9cae65, 0x48b7c2, 0xb49ce1, 0xd389ba, 0xc49f4d };

typedef enum { ST_NONE, ST_PLAIN, ST_OK, ST_ERR } tone;
typedef struct { char text[320]; tone tone; } status;

// One engine's half of the draft: speech or cleanup.
typedef struct {
    int engine;                      // 0 on this computer, 1 OpenAI-compatible
    int model;                       // index among the kind's models
    char url[256], key[256], remote_model[128];
    status test, fetch, manage;
    bool testing, fetching;
    char **remote_models;            // what the service listed
    int remote_count, remote_pick;
    // Adding a model from Hugging Face: the address, the versions found there, the one chosen.
    char hf_url[300];
    status hf;
    bool hf_busy;
    char **hf_labels, **hf_shown;
    int hf_count, hf_pick;
} engine_draft;

static struct {
    eh_ui *ui;
    gs_jval *base;                     // the settings the draft was filled from; fields not shown here pass through
    eh_form_baseline baseline;       // the shared fields as this window last saw them saved
    int section;                     // the index entry lit
    bool spy_held;                   // a jump is gliding: the index keeps the entry chosen
    float section_y[SECTIONS], content_h, card_h[24], view_top, view_bottom;
    int card;
    bool wizard_banner, open_wizard;
    char *correct;                   // a transcript whose Correct… was pressed: the editor opens after the frame
    // General.
    char hotkeys[4][64];
    status hotkey_status[4];
    char **mics;
    int mic_count, mic;              // 0: system default
    char saved_mic[256];
    bool mic_missing;
    int output_mode;
    status accessibility;
    // Live, speech, cleanup.
    bool live_preview;
    engine_draft stt, cleanup;
    char language[32];
    bool cleanup_enabled;
    int style_mode, style;           // 0 preset, 1 custom
    char temperature[16], top_p[16], top_k[16], min_p[16];
    char dictionary[8192], prompt[16384];
    // History and advanced.
    bool history_enabled;
    int history_page;
    Uint32 expanded;                 // one bit per row of the page
    status history_status;
    int copied_row, copied_kind;
    Uint64 copied_at;
    char max_seconds[16], idle_unload[16];
    status logs;
    // Save.
    status save;
    bool saving, close_when_saved;
    Uint64 save_clear_at;
    int ask;                         // what the open question is about
    const eh_model *ask_model;
} W;

enum { ASK_NONE, ASK_REMOVE_MODEL, ASK_CLEAR_HISTORY, ASK_REMOVE_CUSTOM };
static const char *const hotkey_paths[4] = { "hotkey", "pauseHotkey", "discardHotkey", "correctHotkey" };

static void say(status *s, tone t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(s->text, sizeof s->text, fmt, ap);
    va_end(ap);
    s->tone = t;
}

/* ---------------- the draft ---------------- */

static int models_of(eh_model_kind kind, const eh_model **out, int max) { return eh_models_list(kind, out, max); }

static int model_index(eh_model_kind kind, const char *id) {
    const eh_model *list[32];
    int n = models_of(kind, list, 32), fallback = 0;
    for (int i = 0; i < n; i++) {
        if (!SDL_strcmp(list[i]->id, id)) return i;
        if (list[i]->is_default) fallback = i;
    }
    return fallback;
}

static const eh_model *model_at(eh_model_kind kind, int index) {
    const eh_model *list[32];
    int n = models_of(kind, list, 32);
    return index >= 0 && index < n ? list[index] : NULL;
}

static int output_mode_of(const gs_jval *cfg) {
    const char *mode = eh_displayed_output_mode(cfg);
    return !SDL_strcmp(mode, "clipboard") ? 2 : !SDL_strcmp(mode, "paste-copy") ? 1 : 0;
}

// A number as its shortest text that reads back as the same number.
static void number_text(char *out, size_t size, const gs_jval *v) {
    if (!v || gs_jval_kind(v) != GS_JSON_NUMBER) { out[0] = 0; return; }
    if (gs_jval_number(v, 0) == floor(gs_jval_number(v, 0)) && fabs(gs_jval_number(v, 0)) < 1e15) { SDL_snprintf(out, size, "%.0f", gs_jval_number(v, 0)); return; }
    for (int digits = 1; digits <= 17; digits++) {
        SDL_snprintf(out, size, "%.*g", digits, gs_jval_number(v, 0));
        if (SDL_strtod(out, NULL) == gs_jval_number(v, 0)) return;
    }
}

static void load_microphones(void) {
    for (int i = 0; i < W.mic_count; i++) SDL_free(W.mics[i]);
    SDL_free(W.mics);
    int count = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioRecordingDevices(&count);
    W.mics = SDL_calloc((size_t)count + 2, sizeof *W.mics);
    W.mic_count = 0;
    W.mics[W.mic_count++] = SDL_strdup("System default");
    W.mic = 0;
    W.mic_missing = false;
    for (int i = 0; ids && i < count; i++) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        if (!name) continue;
        if (!SDL_strcmp(name, W.saved_mic)) W.mic = W.mic_count;
        W.mics[W.mic_count++] = SDL_strdup(name);
    }
    SDL_free(ids);
    // A saved microphone that is not connected stays chosen, and stays saved.
    if (W.saved_mic[0] && !W.mic) {
        W.mic = W.mic_count;
        W.mics[W.mic_count++] = SDL_strdup("Configured microphone (not connected)");
        W.mic_missing = true;
    }
}

static void populate(void) {
    const gs_jval *c = W.base;
    for (int i = 0; i < 4; i++) SDL_strlcpy(W.hotkeys[i], gs_jval_string(gs_jval_get(c, hotkey_paths[i]), ""), sizeof W.hotkeys[i]);
    W.output_mode = output_mode_of(c);
    SDL_strlcpy(W.saved_mic, gs_jval_string(gs_jval_path(c, "audio.deviceId"), ""), sizeof W.saved_mic);
    load_microphones();
    W.live_preview = gs_jval_boolean(gs_jval_path(c, "stt.livePreview.enabled"), true);
    W.stt.engine = SDL_strcmp(gs_jval_string(gs_jval_path(c, "stt.engine"), "builtin"), "builtin") != 0;
    W.stt.model = model_index(EH_MODEL_STT, gs_jval_string(gs_jval_path(c, "stt.builtin.model"), ""));
    SDL_strlcpy(W.stt.url, gs_jval_string(gs_jval_path(c, "stt.baseUrl"), ""), sizeof W.stt.url);
    SDL_strlcpy(W.stt.key, gs_jval_string(gs_jval_path(c, "stt.apiKey"), ""), sizeof W.stt.key);
    SDL_strlcpy(W.stt.remote_model, gs_jval_string(gs_jval_path(c, "stt.model"), ""), sizeof W.stt.remote_model);
    SDL_strlcpy(W.language, gs_jval_string(gs_jval_path(c, "stt.language"), ""), sizeof W.language);
    W.cleanup_enabled = gs_jval_boolean(gs_jval_path(c, "cleanup.enabled"), true);
    W.cleanup.engine = SDL_strcmp(gs_jval_string(gs_jval_path(c, "cleanup.engine"), "builtin"), "builtin") != 0;
    W.cleanup.model = model_index(EH_MODEL_CLEANUP, gs_jval_string(gs_jval_path(c, "cleanup.builtin.model"), ""));
    SDL_strlcpy(W.cleanup.url, gs_jval_string(gs_jval_path(c, "cleanup.baseUrl"), ""), sizeof W.cleanup.url);
    SDL_strlcpy(W.cleanup.key, gs_jval_string(gs_jval_path(c, "cleanup.apiKey"), ""), sizeof W.cleanup.key);
    SDL_strlcpy(W.cleanup.remote_model, gs_jval_string(gs_jval_path(c, "cleanup.model"), ""), sizeof W.cleanup.remote_model);
    const char *style = gs_jval_string(gs_jval_path(c, "cleanup.style"), EH_DEFAULT_STYLE);
    W.style_mode = !SDL_strcmp(style, "custom");
    W.style = 0;
    for (size_t i = 0; i < SDL_arraysize(eh_styles); i++)
        if (!SDL_strcmp(eh_styles[i].id, W.style_mode ? EH_DEFAULT_STYLE : style)) W.style = (int)i;
    number_text(W.temperature, sizeof W.temperature, gs_jval_path(c, "cleanup.custom.temperature"));
    number_text(W.top_p, sizeof W.top_p, gs_jval_path(c, "cleanup.custom.topP"));
    number_text(W.top_k, sizeof W.top_k, gs_jval_path(c, "cleanup.custom.topK"));
    number_text(W.min_p, sizeof W.min_p, gs_jval_path(c, "cleanup.custom.minP"));
    W.dictionary[0] = 0;
    const gs_jval *words = gs_jval_path(c, "cleanup.dictionary");
    for (int i = 0; i < gs_jval_count(words); i++) {
        if (i) SDL_strlcat(W.dictionary, "\n", sizeof W.dictionary);
        SDL_strlcat(W.dictionary, gs_jval_string(gs_jval_at(words, i), ""), sizeof W.dictionary);
    }
    SDL_strlcpy(W.prompt, gs_jval_string(gs_jval_path(c, "cleanup.systemPrompt"), ""), sizeof W.prompt);
    W.history_enabled = gs_jval_boolean(gs_jval_path(c, "history.enabled"), true);
    number_text(W.max_seconds, sizeof W.max_seconds, gs_jval_path(c, "audio.maxRecordingSeconds"));
    number_text(W.idle_unload, sizeof W.idle_unload, gs_jval_path(c, "engines.idleUnloadMinutes"));
    eh_form_baseline_take(&W.baseline, c);
}

// A number typed into a field, kept inside its limits; `fallback` when it is not a number.
static double typed(const char *text, double min, double max, double fallback) {
    char *end;
    double v = SDL_strtod(text, &end);
    while (*end == ' ') end++;
    if (end == text || *end || !isfinite(v)) return fallback;
    return SDL_min(max, SDL_max(min, v));
}

static char *trimmed(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t n = SDL_strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    return SDL_strndup(s, n);
}

static void set_trimmed(gs_jval *o, const char *path, const char *text) {
    char *t = trimmed(text);
    gs_jval_set_path(o, path, gs_jval_str(t));
    SDL_free(t);
}

static gs_jval *dictionary_of(const char *text) {
    gs_jval *list = gs_jval_array();
    for (const char *p = text; *p;) {
        const char *end = SDL_strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : SDL_strlen(p);
        char *line = SDL_strndup(p, n), *t = trimmed(line);
        if (*t) gs_jval_push(list, gs_jval_str(t));
        SDL_free(t);
        SDL_free(line);
        p += n + (end ? 1 : 0);
    }
    return list;
}

// The draft as settings: what was loaded, with every field of this window written over it.
static gs_jval *collect(void) {
    gs_jval *n = gs_jval_clone(W.base);
    for (int i = 0; i < 4; i++) gs_jval_set(n, hotkey_paths[i], gs_jval_str(W.hotkeys[i]));
    gs_jval_set_path(n, "output.mode", gs_jval_str(W.output_mode == 2 ? "clipboard" : W.output_mode == 1 ? "paste-copy" : "paste"));
    gs_jval_set_path(n, "output.restoreClipboard", gs_jval_bool(true));
    gs_jval_set_path(n, "audio.deviceId", gs_jval_str(W.mic == 0 ? "" : W.mic_missing && W.mic == W.mic_count - 1 ? W.saved_mic : W.mics[W.mic]));
    double before = gs_jval_number(gs_jval_path(W.base, "audio.maxRecordingSeconds"), 600);
    gs_jval_set_path(n, "audio.maxRecordingSeconds", gs_jval_num(floor(typed(W.max_seconds, 10, 3600, before) + 0.5)));
    gs_jval_set_path(n, "engines.idleUnloadMinutes", gs_jval_num(floor(typed(W.idle_unload, 0, 240, 0) + 0.5)));
    gs_jval_set_path(n, "history.enabled", gs_jval_bool(W.history_enabled));
    gs_jval_set_path(n, "stt.engine", gs_jval_str(W.stt.engine ? "remote" : "builtin"));
    const eh_model *m = model_at(EH_MODEL_STT, W.stt.model);
    if (m) gs_jval_set_path(n, "stt.builtin.model", gs_jval_str(m->id));
    set_trimmed(n, "stt.baseUrl", W.stt.url);
    set_trimmed(n, "stt.apiKey", W.stt.key);
    set_trimmed(n, "stt.model", W.stt.remote_model);
    set_trimmed(n, "stt.language", W.language);
    gs_jval_set_path(n, "stt.livePreview.enabled", gs_jval_bool(W.live_preview));
    gs_jval_set_path(n, "cleanup.enabled", gs_jval_bool(W.cleanup_enabled));
    gs_jval_set_path(n, "cleanup.engine", gs_jval_str(W.cleanup.engine ? "remote" : "builtin"));
    m = model_at(EH_MODEL_CLEANUP, W.cleanup.model);
    if (m) gs_jval_set_path(n, "cleanup.builtin.model", gs_jval_str(m->id));
    set_trimmed(n, "cleanup.baseUrl", W.cleanup.url);
    set_trimmed(n, "cleanup.apiKey", W.cleanup.key);
    set_trimmed(n, "cleanup.model", W.cleanup.remote_model);
    gs_jval_set_path(n, "cleanup.style", gs_jval_str(W.style_mode ? "custom" : eh_styles[W.style].id));
    const gs_jval *cur = gs_jval_path(W.base, "cleanup.custom");
    gs_jval_set_path(n, "cleanup.custom.temperature", gs_jval_num(typed(W.temperature, 0, 2, gs_jval_number(gs_jval_get(cur, "temperature"), 0.2))));
    gs_jval_set_path(n, "cleanup.custom.topP", gs_jval_num(typed(W.top_p, 0, 1, gs_jval_number(gs_jval_get(cur, "topP"), 1))));
    gs_jval_set_path(n, "cleanup.custom.topK", gs_jval_num(floor(typed(W.top_k, 0, 200, gs_jval_number(gs_jval_get(cur, "topK"), 0)) + 0.5)));
    gs_jval_set_path(n, "cleanup.custom.minP", gs_jval_num(typed(W.min_p, 0, 1, gs_jval_number(gs_jval_get(cur, "minP"), 0))));
    gs_jval_set_path(n, "cleanup.dictionary", dictionary_of(W.dictionary));
    gs_jval_set_path(n, "cleanup.systemPrompt", gs_jval_str(W.prompt));
    return n;
}

/* ---------------- what runs in the background ---------------- */

// Save. Hotkeys that could not be registered keep their saved value, and the draft keeps what was
// typed so it can be corrected.
static void save_task(void *arg) {
    gs_jval *next = arg;
    eh_hotkey_results results;
    gs_jval *typed_keys = gs_jval_object();
    for (int i = 0; i < 4; i++) gs_jval_set(typed_keys, hotkey_paths[i], gs_jval_clone(gs_jval_get(next, hotkey_paths[i])));
    char error[256];
    if (!eh_commit_settings(next, &W.baseline, &results, error, sizeof error)) {
        say(&W.save, ST_ERR, "Could not save: %s", error);
        gs_jval_free(typed_keys);
        W.saving = false;
        return;
    }
    int failed = 0, failed_at = 0;
    for (int i = 0; i < 4; i++)
        if (!results.r[i].ok) failed++, failed_at = i;
    // What the limits made of the numbers typed.
    char adjusted[200] = "";
    double seconds = eh_setting_num("audio.maxRecordingSeconds"), idle = eh_setting_num("engines.idleUnloadMinutes");
    char *end;
    if (W.max_seconds[0] && (SDL_strtod(W.max_seconds, &end) != seconds || *end)) SDL_snprintf(adjusted, sizeof adjusted, "Max dictation length set to %.0f s (allowed 10–3600)", seconds);
    if (W.idle_unload[0] && (SDL_strtod(W.idle_unload, &end) != idle || *end))
        SDL_snprintf(adjusted + SDL_strlen(adjusted), sizeof adjusted - SDL_strlen(adjusted), "%sIdle unload set to %.0f min (allowed 0–240)", adjusted[0] ? "; " : "", idle);
    gs_jval_free(W.base);
    W.base = gs_jval_clone(eh_settings());
    populate();
    // The hotkeys that were refused stay in their fields as typed, each with its reason under it.
    static const char *const names[4] = { "hotkey", "pause hotkey", "discard hotkey", "correction hotkey" };
    for (int i = 0; i < 4; i++) {
        bool bad = !results.r[i].ok;
        if (bad) SDL_strlcpy(W.hotkeys[i], gs_jval_string(gs_jval_get(typed_keys, hotkey_paths[i]), ""), sizeof W.hotkeys[i]);
        say(&W.hotkey_status[i], bad ? ST_ERR : ST_NONE, "%s", bad ? results.r[i].error : "");
    }
    gs_jval_free(typed_keys);
    W.saving = false;
    if (failed) {
        if (failed > 1) say(&W.save, ST_ERR, "Saved, but the hotkeys could not be changed");
        else say(&W.save, ST_ERR, "Saved, but the %s could not be registered", names[failed_at]);
        W.save_clear_at = SDL_GetTicks() + 4000;
    } else if (adjusted[0]) {
        say(&W.save, ST_OK, "Saved — %s", adjusted);
    } else {
        say(&W.save, ST_OK, "Saved");
        W.close_when_saved = true;
    }
}

typedef struct { engine_draft *d; gs_jval *cfg; bool cleanup; } probe;

static void test_task(void *arg) {
    probe *p = arg;
    char error[320] = "";
    if (p->cleanup) {
        char *out = eh_clean("um so this is uh a test of the cleanup service", p->cfg, NULL, NULL, NULL, error, sizeof error);
        if (out) say(&p->d->test, ST_OK, "OK — \"%.200s\"", out);
        else say(&p->d->test, ST_ERR, "%s", error);
        SDL_free(out);
    } else {
        // Half a second of silence: enough for a service to answer, with nothing to hear.
        static int16_t silence[8000];
        size_t len = 0;
        uint8_t *wav = eh_wav_encode(silence, 8000, 16000, &len);
        char *text = NULL;
        int ms = 0;
        eh_decode_code code = eh_transcribe(p->cfg, NULL, wav, len, &text, &ms, error, sizeof error);
        if (code == EH_DECODE_OK) say(&p->d->test, ST_OK, "OK");
        else say(&p->d->test, ST_ERR, "%s", error[0] ? error : "The service did not answer");
        SDL_free(text);
        SDL_free(wav);
    }
    p->d->testing = false;
    gs_jval_free(p->cfg);
    SDL_free(p);
}

static void fetch_task(void *arg) {
    probe *p = arg;
    char error[320] = "";
    gs_jval *models = eh_remote_models(p->cfg, NULL, error, sizeof error);
    engine_draft *d = p->d;
    for (int i = 0; i < d->remote_count; i++) SDL_free(d->remote_models[i]);
    SDL_free(d->remote_models);
    d->remote_models = NULL;
    d->remote_count = 0;
    if (!models) {
        say(&d->fetch, ST_ERR, "%s", error);
    } else {
        int n = gs_jval_count(models);
        d->remote_models = SDL_calloc((size_t)n + 1, sizeof *d->remote_models);
        d->remote_pick = -1;
        for (int i = 0; i < n; i++) {
            d->remote_models[i] = SDL_strdup(gs_jval_string(gs_jval_at(models, i), ""));
            if (!SDL_strcmp(d->remote_models[i], d->remote_model)) d->remote_pick = i;
        }
        d->remote_count = n;
        if (n) say(&d->fetch, ST_OK, "%d model%s — choose one from the list", n, n == 1 ? "" : "s");
        else say(&d->fetch, ST_PLAIN, "No models reported by this service");
        gs_jval_free(models);
    }
    d->fetching = false;
    gs_jval_free(p->cfg);
    SDL_free(p);
}

static void probe_start(engine_draft *d, bool cleanup, eh_task_fn *fn) {
    gs_jval *all = collect();
    probe *p = SDL_calloc(1, sizeof *p);
    p->d = d;
    p->cleanup = cleanup;
    p->cfg = gs_jval_clone(gs_jval_get(all, cleanup ? "cleanup" : "stt"));
    if (cleanup) gs_jval_set(p->cfg, "enabled", gs_jval_bool(true));
    gs_jval_free(all);
    eh_spawn(fn, p);
}

typedef struct { char *text; int row, kind; } copy;

static void copy_task(void *arg) {
    copy *c = arg;
    char error[256] = "";
    eh_delivery result;
    gs_jval *output = gs_jval_object();
    gs_jval_set(output, "mode", gs_jval_str("clipboard"));
    if (eh_pipeline_state() == EH_PROCESSING) say(&W.history_status, ST_ERR, "Could not copy: Wait for the current dictation to finish processing before copying a transcript.");
    else if (!eh_deliver(c->text, output, NULL, &result, error, sizeof error)) say(&W.history_status, ST_ERR, "Could not copy: %s", error);
    else W.copied_row = c->row, W.copied_kind = c->kind, W.copied_at = SDL_GetTicks();
    gs_jval_free(output);
    SDL_free(c->text);
    SDL_free(c);
}

static void remove_task(void *arg) {
    const eh_model *m = arg;
    char error[200] = "";
    engine_draft *d = m->kind == EH_MODEL_STT ? &W.stt : &W.cleanup;
    if (eh_model_job_remove(m, error, sizeof error)) say(&d->manage, ST_PLAIN, "Removed");
    else say(&d->manage, ST_ERR, "%s", error[0] ? error : "Could not remove model");
}

typedef struct { eh_model_kind kind; engine_draft *d; char *address, *variant, *id; } hf_job;

static void hf_clear(engine_draft *d) {
    for (int i = 0; i < d->hf_count; i++) SDL_free(d->hf_labels[i]), SDL_free(d->hf_shown[i]);
    SDL_free(d->hf_labels);
    SDL_free(d->hf_shown);
    d->hf_labels = d->hf_shown = NULL;
    d->hf_count = 0;
}

static void hf_find_task(void *arg) {
    hf_job *j = arg;
    engine_draft *d = j->d;
    char error[1400] = "";
    gs_jval *listing;
    EH_AWAIT(listing = eh_hf_discover(j->kind, j->address, error, sizeof error));
    hf_clear(d);
    if (!listing) {
        say(&d->hf, ST_ERR, "%s", error);
    } else {
        const gs_jval *variants = gs_jval_get(listing, "variants");
        const char *recommended = gs_jval_string(gs_jval_get(listing, "recommended"), "");
        int n = gs_jval_count(variants);
        d->hf_labels = SDL_calloc((size_t)n + 1, sizeof *d->hf_labels);
        d->hf_shown = SDL_calloc((size_t)n + 1, sizeof *d->hf_shown);
        d->hf_pick = 0;
        for (int i = 0; i < n; i++) {
            const gs_jval *v = gs_jval_at(variants, i);
            const char *label = gs_jval_string(gs_jval_get(v, "label"), "");
            double bytes = gs_jval_number(gs_jval_get(v, "totalBytes"), 0);
            char size[32] = "", shown[240];
            if (bytes >= 1e9) SDL_snprintf(size, sizeof size, " · %.1f GB", bytes / 1e9);
            else if (bytes > 0) SDL_snprintf(size, sizeof size, " · %.0f MB", bytes / 1e6);
            bool best = !SDL_strcmp(label, recommended);
            SDL_snprintf(shown, sizeof shown, "%s%s%s", label, size, best ? " · recommended" : "");
            d->hf_labels[i] = SDL_strdup(label);
            d->hf_shown[i] = SDL_strdup(shown);
            if (best) d->hf_pick = i;
        }
        d->hf_count = n;
        say(&d->hf, ST_OK, "%s — %d version%s", gs_jval_string(gs_jval_get(listing, "repo"), ""), n, n == 1 ? "" : "s");
        gs_jval_free(listing);
    }
    d->hf_busy = false;
    SDL_free(j->address);
    SDL_free(j);
}

static void hf_add_task(void *arg) {
    hf_job *j = arg;
    engine_draft *d = j->d;
    char error[1400] = "", id[300] = "";
    if (eh_custom_model_add(j->kind, j->address, j->variant, id, sizeof id, error, sizeof error)) {
        d->model = model_index(j->kind, id);
        d->hf_url[0] = 0;
        hf_clear(d);
        d->manage.tone = ST_NONE;
        say(&d->hf, ST_OK, "Added — click Download to fetch it");
    } else {
        say(&d->hf, ST_ERR, "%s", error);
    }
    d->hf_busy = false;
    SDL_free(j->address);
    SDL_free(j->variant);
    SDL_free(j);
}

static void custom_remove_task(void *arg) {
    hf_job *j = arg;
    engine_draft *d = j->d;
    // The other models keep their places in the list by their ids.
    const eh_model *chosen = model_at(j->kind, d->model);
    char keep[300], error[300] = "";
    SDL_strlcpy(keep, chosen && SDL_strcmp(chosen->id, j->id) ? chosen->id : "", sizeof keep);
    if (eh_custom_model_remove(j->id, error, sizeof error)) {
        d->model = model_index(j->kind, keep[0] ? keep : eh_setting_str(j->kind == EH_MODEL_STT ? "stt.builtin.model" : "cleanup.builtin.model"));
        say(&d->manage, ST_PLAIN, "Removed");
    } else {
        say(&d->manage, ST_ERR, "%s", error[0] ? error : "Could not remove model");
    }
    SDL_free(j->id);
    SDL_free(j);
}

/* ---------------- laying the page out ---------------- */

static struct { float x, w, y; } pg;

static bool on_screen(float y, float h) { return y + h > W.view_top && y < W.view_bottom; }

static gs_rect take(float h) {
    gs_rect r = { pg.x, pg.y, pg.w, h };
    pg.y += h;
    return r;
}

static void text(const char *s, float px, SDL_FColor c) {
    float h = eh_w_text_height(W.ui, s, px, pg.w);
    if (on_screen(pg.y, h)) eh_w_text(W.ui, (gs_rect){ pg.x, pg.y, pg.w, h }, s, px, c);
    pg.y += h;
}

static void label(const char *s) {
    eh_w_line(W.ui, take(20), s, 13, eh_ui_ink(W.ui, EH_TEXT, 1), -1);
    pg.y += 6;
}

static void hint(const char *s) {
    pg.y += 7;
    text(s, 12, eh_ui_ink(W.ui, EH_TEXT_DIM, 1));
}

static void show(const status *s) {
    if (!s->text[0] || s->tone == ST_NONE) return;
    pg.y += 6;
    text(s->text, 12, eh_ui_ink(W.ui, s->tone == ST_OK ? EH_OK : s->tone == ST_ERR ? EH_ERR : EH_TEXT_DIM, 1));
}

// A card: its background is drawn at the height it had the last time, which a change corrects on
// the next frame.
static float card_x, card_w, card_top;

static void card_begin(const char *title) {
    card_top = pg.y;
    card_x = pg.x, card_w = pg.w;
    float h = W.card_h[W.card];
    if (h > 0) {
        eh_w_round(W.ui, (gs_rect){ pg.x, pg.y, pg.w, h }, 12, EH_WASH_FAINT);
        eh_w_outline(W.ui, (gs_rect){ pg.x, pg.y, pg.w, h }, 12, EH_EDGE);
    }
    pg.x += 18, pg.w -= 36, pg.y += 16;
    if (title) {
        eh_w_line(W.ui, take(20), title, 13, eh_ui_ink(W.ui, EH_TEXT, 1), -1);
        pg.y += 10;
    }
}

static void card_end(void) {
    pg.y += 16;
    float h = pg.y - card_top;
    if (SDL_fabsf(h - W.card_h[W.card]) > 0.5f) W.card_h[W.card] = h, eh_ui_again(W.ui, 0);
    W.card++;
    pg.x = card_x, pg.w = card_w;
    pg.y += 14;
}

static void legend(int section) {
    W.section_y[section] = pg.y;
    gs_rect r = take(26);
    float s = gs_ui_scale(eh_ui_gs(W.ui));
    gs_draw_circle(SDL_GetRenderer(eh_ui_window(W.ui)), (r.x + 9) * s, (r.y + 13) * s, 5 * s, gs_rgba(section_colours[section], 1));
    float w = gs_ui_text_width(eh_ui_gs(W.ui), section_names[section], 16);
    eh_w_line(W.ui, (gs_rect){ r.x + 28, r.y, r.w - 28, r.h }, section_names[section], 16, gs_rgba(EH_TEXT, 1), -1);
    gs_ui_fill(eh_ui_gs(W.ui), (gs_rect){ r.x + 28 + w + 14, r.y + 13, SDL_max(0, r.w - 42 - w), 1 }, EH_SEAM);
    pg.y += 14;
}

// A switch with its title and a line of explanation, the switch at the right.
static bool switch_row(const char *id, const char *title, const char *sub, bool *on) {
    float top = pg.y, w = pg.w;
    pg.w -= 60;
    eh_w_line(W.ui, take(20), title, 13, eh_ui_ink(W.ui, EH_TEXT, 1), -1);
    if (sub) pg.y += 2, text(sub, 12, eh_ui_ink(W.ui, EH_TEXT_DIM, 1));
    pg.w = w;
    return eh_w_switch(W.ui, (gs_rect){ pg.x + pg.w - 40, top, 40, 22 }, id, on);
}

static bool button_at(float *x, float y, const char *id, const char *label_text, eh_btn kind) {
    float w = eh_w_button_width(W.ui, label_text);
    bool hit = eh_w_button(W.ui, (gs_rect){ *x, y, w, 34 }, id, label_text, kind);
    *x += w + 10;
    return hit;
}

/* ---------------- the sections ---------------- */

static void general(void) {
    legend(SEC_GENERAL);
    card_begin("Hotkeys");
    static const char *const titles[4] = { "Global hotkey", "Pause hotkey", "Discard hotkey", "Correct last transcript hotkey" };
    static const char *const ids[4] = { "hotkey", "pause-hotkey", "discard-hotkey", "correct-hotkey" };
    static const char *const clear_ids[4] = { "hotkey-clear", "pause-hotkey-clear", "discard-hotkey-clear", "correct-hotkey-clear" };
#ifdef __linux__
    static const char *const hints[4] = {
        "Press once to start dictating, again to stop and transcribe. On GNOME/KDE Wayland, apps may not be able to grab global keys — instead, bind a system keyboard shortcut to earheart --toggle.",
        "Optional. While dictating, pauses and resumes the take without ending it — same as the overlay's pause key. On Wayland, bind a system shortcut to earheart --pause instead.",
        "Optional. Throws away the dictation in progress — same as the overlay's × key. Held only while dictating. On Wayland, bind a system shortcut to earheart --discard instead.",
        "Open your last saved transcript, correct a spelling and remember it. Optional. On Wayland, bind earheart --correct-last in your desktop settings.",
    };
#else
    static const char *const hints[4] = {
        "Press once to start dictating, again to stop and transcribe.",
        "Optional. While dictating, pauses and resumes the take without ending it — same as the overlay's pause key.",
        "Optional. Throws away the dictation in progress — same as the overlay's × key. Held only while dictating.",
        "Open your last saved transcript, correct a spelling and remember it. Optional.",
    };
#endif
    for (int i = 0; i < 4; i++) {
        if (i) pg.y += 16;
        label(titles[i]);
        gs_rect r = take(38);
        float bw = eh_w_button_width(W.ui, i ? "Clear" : "Reset");
        char got[64];
        if (eh_w_hotkey(W.ui, (gs_rect){ r.x, r.y, r.w - bw - 10, r.h }, ids[i], W.hotkeys[i], i ? "Not set — click, then press a key combination" : "Click, then press a key combination", got, sizeof got)) {
            SDL_strlcpy(W.hotkeys[i], got, sizeof W.hotkeys[i]);
            W.hotkey_status[i].tone = ST_NONE;
        }
        if (eh_w_button(W.ui, (gs_rect){ r.x + r.w - bw, r.y + 2, bw, 34 }, clear_ids[i], i ? "Clear" : "Reset", EH_BTN_PLAIN)) {
            gs_jval *d = gs_jval_parse(eh_defaults_json, sizeof eh_defaults_json - 1);
            SDL_strlcpy(W.hotkeys[i], i ? "" : gs_jval_string(gs_jval_get(d, "hotkey"), ""), sizeof W.hotkeys[i]);
            gs_jval_free(d);
        }
        hint(hints[i]);
        show(&W.hotkey_status[i]);
    }
    card_end();

    card_begin("Microphone");
    label("Microphone");
    eh_w_select(W.ui, take(36), "mic-device", (const char *const *)W.mics, W.mic_count, &W.mic);
    hint("The input dictation records from. System default follows whatever your OS has selected.");
    if (W.mic_missing && W.mic == W.mic_count - 1) {
        pg.y += 6;
        text("Saved microphone not found. Dictation can temporarily use the system default; this selection stays saved.", 12, eh_ui_ink(W.ui, EH_TEXT_DIM, 1));
    }
    card_end();

    card_begin("Where the text goes");
    static const char *const modes[3] = { "Paste into the active app", "Paste and keep the transcript on the clipboard", "Copy to clipboard only — I'll paste myself" };
    static const char *const mode_ids[3] = { "output-paste", "output-paste-copy", "output-clipboard" };
    for (int i = 0; i < 3; i++)
        if (eh_w_radio(W.ui, take(30), mode_ids[i], modes[i], W.output_mode == i)) W.output_mode = i;
    hint("Pasting inserts the transcript at your cursor by briefly using the clipboard — plain paste puts your previous clipboard back afterwards.");
    card_end();

#ifdef __APPLE__
    card_begin("Auto-paste permission");
    float x = pg.x, y = pg.y;
    if (button_at(&x, y, "accessibility-fix", "Fix auto-paste permission…", EH_BTN_PLAIN)) {
        if (gs_paste_permitted()) say(&W.accessibility, ST_OK, "Auto-paste is allowed");
        else {
            gs_paste_ask_permission();
            say(&W.accessibility, ST_PLAIN, "Allow Earheart under Privacy & Security ▸ Accessibility, then come back here.");
        }
    }
    pg.y += 34;
    show(&W.accessibility);
    hint("Auto-paste types into the focused app, which macOS allows only with the Accessibility permission. If pasting stopped working, click here to check it and open the System Settings pane, then allow Earheart.");
    card_end();
#endif
}

static void live(void) {
    legend(SEC_LIVE);
    card_begin(NULL);
    switch_row("stt-live-preview", "Show a live transcript while you speak",
               "Fills in the overlay as you talk, with a cleaned version settling in on pauses. Adds some CPU load while recording. Turned off, built-in speech-to-text still decodes as you talk, so the final transcript is ready sooner.", &W.live_preview);
    card_end();
}

static void progress_label(char *out, size_t size, const eh_model_job *j) {
    int pct = j->total ? (int)floor((double)j->received / (double)j->total * 100 + 0.5) : 0;
    if (!j->total) SDL_snprintf(out, size, "Downloading…");
    else SDL_snprintf(out, size, "%d%% · %.0f MB / %.0f MB", pct, (double)j->received / 1e6, (double)j->total / 1e6);
}

// The chosen built-in model: what it is, whether it is here, and the button that changes that.
static void manage(eh_model_kind kind, engine_draft *d) {
    const eh_model *m = model_at(kind, d->model);
    if (!m) return;
    const char *prefix = kind == EH_MODEL_STT ? "stt" : "cleanup";
    char id[48], dir[1200], line[160];
    if (m->note) hint(m->note);
    eh_model_job job = eh_model_job_get(m);
    bool installed = eh_model_installed(m, dir, sizeof dir);
    if (job.active) {
        pg.y += 10;
        gs_rect bar = take(4);
        eh_w_round(W.ui, bar, 2, gs_rgba(0xffffff, 0.12f));
        if (job.total) eh_w_round(W.ui, (gs_rect){ bar.x, bar.y, bar.w * (float)((double)job.received / (double)job.total), bar.h }, 2, gs_rgba(EH_TEXT, 1));
        eh_ui_again(W.ui, 100);
    }
    pg.y += 10;
    float x = pg.x, y = pg.y;
    SDL_FColor tone_c = eh_ui_ink(W.ui, EH_TEXT_DIM, 1);
    if (installed) {
        SDL_snprintf(id, sizeof id, "%s-model-remove", prefix);
        if (button_at(&x, y, id, "Remove", EH_BTN_DANGER)) {
            char q[400];
            SDL_snprintf(q, sizeof q, m->custom ? "Remove %s and its downloaded files? You'll need to download it again to use it." : "Remove %s? You'll need to download it again to use it.", m->label ? m->label : m->id);
            eh_ui_ask(W.ui, q, "Remove", "Cancel", true);
            W.ask = m->custom ? ASK_REMOVE_CUSTOM : ASK_REMOVE_MODEL;
            W.ask_model = m;
        }
        SDL_strlcpy(line, "Downloaded", sizeof line);
        tone_c = eh_ui_ink(W.ui, EH_OK, 1);
    } else if (job.active) {
        SDL_snprintf(id, sizeof id, "%s-model-cancel", prefix);
        if (button_at(&x, y, id, "Cancel", EH_BTN_PLAIN)) eh_model_job_cancel(m);
        progress_label(line, sizeof line, &job);
    } else {
        bool failed = job.done && !job.ok && !job.cancelled;
        SDL_snprintf(id, sizeof id, "%s-model-download", prefix);
        if (button_at(&x, y, id, failed ? "Retry download" : "Download", EH_BTN_PLAIN)) {
            char error[200];
            d->manage.tone = ST_NONE;
            if (!eh_model_job_start(m, error, sizeof error)) say(&d->manage, ST_ERR, "%s", error);
        }
        SDL_strlcpy(line, failed ? (job.error[0] ? job.error : "Download failed") : job.cancelled ? "Cancelled" : "Not downloaded", sizeof line);
        if (failed) tone_c = eh_ui_ink(W.ui, EH_ERR, 1);
    }
    float text_w = gs_ui_text_width(eh_ui_gs(W.ui), line, 12);
    eh_w_line(W.ui, (gs_rect){ x + 4, y, pg.x + pg.w - x - 4, 34 }, line, 12, tone_c, -1);
    if (m->custom && !installed) {
        // One of the user's own that is not downloaded can simply be forgotten.
        x += SDL_min(text_w, 220) + 18;
        SDL_snprintf(id, sizeof id, "%s-model-forget", prefix);
        if (button_at(&x, y, id, "Remove from list", EH_BTN_DANGER)) {
            char q[400];
            SDL_snprintf(q, sizeof q, "Remove %s from your models?", m->label ? m->label : m->id);
            eh_ui_ask(W.ui, q, "Remove", "Cancel", true);
            W.ask = ASK_REMOVE_CUSTOM;
            W.ask_model = m;
        }
    }
    pg.y += 34;
    show(&d->manage);
}

// A model of the user's choosing: an address on Hugging Face, the versions found there, Add.
static void hf_block(eh_model_kind kind, const char *prefix, engine_draft *d, const char *placeholder, const char *explain) {
    char id[48];
    pg.y += 18;
    label("Add a model from Hugging Face");
    SDL_snprintf(id, sizeof id, "%s-hf-url", prefix);
    eh_w_field(W.ui, take(36), id, d->hf_url, sizeof d->hf_url, placeholder, 0);
    pg.y += 10;
    float x = pg.x, y = pg.y;
    bool was_inert = kind == EH_MODEL_CLEANUP && !W.cleanup_enabled;
    eh_ui_inert(W.ui, d->hf_busy || was_inert, was_inert);
    SDL_snprintf(id, sizeof id, "%s-hf-find", prefix);
    char *address = trimmed(d->hf_url);
    if (button_at(&x, y, id, "Find versions", EH_BTN_PLAIN) && *address) {
        hf_job *j = SDL_calloc(1, sizeof *j);
        j->kind = kind, j->d = d, j->address = SDL_strdup(address);
        d->hf_busy = true;
        hf_clear(d);
        say(&d->hf, ST_PLAIN, "Looking up versions…");
        eh_spawn(hf_find_task, j);
    }
    eh_ui_inert(W.ui, was_inert, was_inert);
    SDL_snprintf(id, sizeof id, "%s-hf-browse", prefix);
    if (button_at(&x, y, id, "Browse Hugging Face", EH_BTN_PLAIN) && !SDL_OpenURL(eh_hf_search_url(kind))) say(&d->hf, ST_ERR, "%s", SDL_GetError());
    pg.y += 34;
    show(&d->hf);
    if (d->hf_count) {
        pg.y += 10;
        gs_rect row = take(36);
        float add_w = eh_w_button_width(W.ui, "Add");
        SDL_snprintf(id, sizeof id, "%s-hf-variant", prefix);
        eh_w_select(W.ui, (gs_rect){ row.x, row.y, row.w - add_w - 10, row.h }, id, (const char *const *)d->hf_shown, d->hf_count, &d->hf_pick);
        eh_ui_inert(W.ui, d->hf_busy || was_inert, was_inert);
        SDL_snprintf(id, sizeof id, "%s-hf-add", prefix);
        if (eh_w_button(W.ui, (gs_rect){ row.x + row.w - add_w, row.y + 1, add_w, 34 }, id, "Add", EH_BTN_PLAIN) && *address) {
            hf_job *j = SDL_calloc(1, sizeof *j);
            j->kind = kind, j->d = d, j->address = SDL_strdup(address), j->variant = SDL_strdup(d->hf_labels[d->hf_pick]);
            d->hf_busy = true;
            say(&d->hf, ST_PLAIN, "Adding…");
            eh_spawn(hf_add_task, j);
        }
        eh_ui_inert(W.ui, was_inert, was_inert);
    }
    SDL_free(address);
    hint(explain);
    if (d->hf_busy) eh_ui_again(W.ui, 100);
}

static void engine_choice(const char *prefix, engine_draft *d, const char *explain, const char *here, const char *there) {
    char id[48];
    float x = pg.x, y = pg.y;
    static const char *const names[2] = { "On this computer", "OpenAI-compatible" };
    for (int i = 0; i < 2; i++) {
        float w = gs_ui_text_width(eh_ui_gs(W.ui), names[i], 13) + 40;
        SDL_snprintf(id, sizeof id, "%s-engine-%s", prefix, i ? "external" : "builtin");
        if (eh_w_radio(W.ui, (gs_rect){ x, y, w, 30 }, id, names[i], d->engine == i)) d->engine = i;
        x += w + 12;
    }
    pg.y += 30;
    hint(explain);
    pg.y += 8;
    gs_rect r = take(20);
    float s = gs_ui_scale(eh_ui_gs(W.ui));
    gs_draw_circle(SDL_GetRenderer(eh_ui_window(W.ui)), (r.x + 5) * s, (r.y + 10) * s, 4 * s, eh_ui_ink(W.ui, d->engine ? 0xea9941 : 0x16bbbc, 1));
    eh_w_line(W.ui, (gs_rect){ r.x + 18, r.y, r.w - 18, r.h }, d->engine ? there : here, 12, eh_ui_ink(W.ui, EH_TEXT, 0.9f), -1);
}

static void model_select(eh_model_kind kind, const char *id, engine_draft *d) {
    const eh_model *list[32];
    const char *labels[32];
    int n = models_of(kind, list, 32);
    for (int i = 0; i < n; i++) labels[i] = list[i]->label ? list[i]->label : list[i]->id;
    label("Model");
    if (eh_w_select(W.ui, take(36), id, labels, n, &d->model)) d->manage.tone = ST_NONE;
}

// The fields of an OpenAI-compatible service: where it is, its key, and which model to ask for.
static void remote_fields(const char *prefix, engine_draft *d, bool cleanup, const char *url_hint, const char *url_note, const char *model_hint) {
    char id[48];
    label("Base URL");
    SDL_snprintf(id, sizeof id, "%s-url", prefix);
    eh_w_field(W.ui, take(36), id, d->url, sizeof d->url, url_hint, 0);
    hint(url_note);
    pg.y += 14;
    label("API key (optional for local servers)");
    SDL_snprintf(id, sizeof id, "%s-key", prefix);
    eh_w_field(W.ui, take(36), id, d->key, sizeof d->key, NULL, EH_FIELD_MASKED);
    hint("Saved in the settings file, which is readable only by your user account.");
    pg.y += 14;
    label("Model");
    SDL_snprintf(id, sizeof id, "%s-model", prefix);
    eh_w_field(W.ui, take(36), id, d->remote_model, sizeof d->remote_model, model_hint, 0);
    if (d->remote_count) {
        // What the service listed, to choose from: a choice fills the field above.
        pg.y += 8;
        SDL_snprintf(id, sizeof id, "%s-model-list", prefix);
        int pick = d->remote_pick;
        const char *none[1] = { "Choose a model…" };
        if (pick < 0) {
            int zero = 0;
            const char **items = SDL_calloc((size_t)d->remote_count + 1, sizeof *items);
            items[0] = none[0];
            for (int i = 0; i < d->remote_count; i++) items[i + 1] = d->remote_models[i];
            if (eh_w_select(W.ui, take(36), id, items, d->remote_count + 1, &zero) && zero > 0) pick = zero - 1;
            SDL_free(items);
        } else {
            eh_w_select(W.ui, take(36), id, (const char *const *)d->remote_models, d->remote_count, &pick);
        }
        if (pick != d->remote_pick && pick >= 0) {
            d->remote_pick = pick;
            SDL_strlcpy(d->remote_model, d->remote_models[pick], sizeof d->remote_model);
        }
    }
    if (!cleanup) {
        pg.y += 14;
        label("Language (optional, e.g. en)");
        eh_w_field(W.ui, take(36), "stt-language", W.language, sizeof W.language, "auto", 0);
    }
    pg.y += 12;
    float x = pg.x, y = pg.y;
    SDL_snprintf(id, sizeof id, "%s-fetch-models", prefix);
    eh_ui_inert(W.ui, d->fetching || (cleanup && !W.cleanup_enabled), cleanup && !W.cleanup_enabled);
    if (button_at(&x, y, id, "Fetch models", EH_BTN_PLAIN)) {
        d->fetching = true;
        say(&d->fetch, ST_PLAIN, "Fetching…");
        probe_start(d, cleanup, fetch_task);
    }
    eh_ui_inert(W.ui, d->testing || (cleanup && !W.cleanup_enabled), cleanup && !W.cleanup_enabled);
    SDL_snprintf(id, sizeof id, "%s-test", prefix);
    if (button_at(&x, y, id, "Test connection", EH_BTN_PLAIN)) {
        d->testing = true;
        say(&d->test, ST_PLAIN, "Testing…");
        probe_start(d, cleanup, test_task);
    }
    eh_ui_inert(W.ui, cleanup && !W.cleanup_enabled, cleanup && !W.cleanup_enabled);
    pg.y += 34;
    show(&d->fetch);
    show(&d->test);
    if (d->fetching || d->testing) eh_ui_again(W.ui, 100);
}

static void speech(void) {
    legend(SEC_STT);
    card_begin("Transcription engine");
    engine_choice("stt", &W.stt, "Built-in runs NVIDIA Parakeet privately, with nothing to install. OpenAI-compatible points at a local server, OpenAI, Groq, and the like.",
                  "Runs on this device — your audio never leaves it", "Sends audio to the endpoint below");
    pg.y += 16;
    if (!W.stt.engine) {
        model_select(EH_MODEL_STT, "stt-builtin-model", &W.stt);
        manage(EH_MODEL_STT, &W.stt);
        hf_block(EH_MODEL_STT, "stt", &W.stt, "csukuangfj/sherpa-onnx-nemo-parakeet-tdt-0.6b-v2",
                 "Paste a Hugging Face URL or owner/model of a sherpa-onnx transducer bundle (encoder, decoder, joiner + tokens.txt) or Whisper export (encoder, decoder + tokens.txt), such as csukuangfj/sherpa-onnx-whisper-turbo. Other encoder-decoder models, such as NeMo Canary, aren't supported. Files with a published Hugging Face LFS SHA-256 are verified on download. Browse Hugging Face opens your browser on the Parakeet TDT exports, the family the built-in models come from.");
    } else {
        remote_fields("stt", &W.stt, false, "http://127.0.0.1:8484/v1", "The app calls {base URL}/audio/transcriptions.", "parakeet");
    }
    card_end();
}

static void number_field(const char *title, const char *id, char *buf, size_t size, const char *note) {
    label(title);
    float w = pg.w;
    pg.w = SDL_min(w, 160);
    eh_w_field(W.ui, take(36), id, buf, size, NULL, 0);
    pg.w = w;
    if (note) hint(note);
}

static void cleanup(void) {
    legend(SEC_CLEANUP);
    card_begin(NULL);
    switch_row("cleanup-enabled", "Clean up transcripts automatically", "After transcription, a language model fixes punctuation and removes filler words and false starts.", &W.cleanup_enabled);
    card_end();
    // With cleanup off, everything below is shown and cannot be used.
    bool off = !W.cleanup_enabled;
    eh_ui_inert(W.ui, off, off);

    card_begin("Cleanup engine");
    engine_choice("cleanup", &W.cleanup, "Built-in runs a model on this computer, with nothing to install. OpenAI-compatible points at Ollama, LM Studio, OpenAI, and the like.",
                  "Runs on this device — your words never leave it", "Sends the transcript to the endpoint below");
    pg.y += 16;
    if (!W.cleanup.engine) {
        model_select(EH_MODEL_CLEANUP, "cleanup-builtin-model", &W.cleanup);
        manage(EH_MODEL_CLEANUP, &W.cleanup);
        hf_block(EH_MODEL_CLEANUP, "cleanup", &W.cleanup, "unsloth/gemma-3-1b-it-GGUF",
                 "Paste a Hugging Face GGUF model URL or owner/model. Earheart lists its versions and downloads the one you pick. Files with a published Hugging Face LFS SHA-256 are verified on download. Browse Hugging Face opens your browser on chat GGUF models up to 12B parameters; smaller ones clean up faster.");
    } else {
        remote_fields("cleanup", &W.cleanup, true, "http://127.0.0.1:11434/v1", "The app calls {base URL}/chat/completions.", "llama3.2");
    }
    card_end();

    card_begin("Cleanup style");
    float x = pg.x, y = pg.y;
    static const char *const style_modes[2] = { "Preset", "Custom values" };
    static const char *const style_mode_ids[2] = { "cleanup-style-mode-preset", "cleanup-style-mode-custom" };
    for (int i = 0; i < 2; i++) {
        float w = gs_ui_text_width(eh_ui_gs(W.ui), style_modes[i], 13) + 40;
        if (eh_w_radio(W.ui, (gs_rect){ x, y, w, 30 }, style_mode_ids[i], style_modes[i], W.style_mode == i)) W.style_mode = i;
        x += w + 12;
    }
    pg.y += 30;
    hint("Presets pair editing instructions with matched sampling values. Custom sends only your system prompt, using the raw sampling values below — no preset instructions.");
    pg.y += 12;
    if (!W.style_mode) {
        eh_w_stops(W.ui, take(28), "cleanup-style", &W.style, (int)SDL_arraysize(eh_styles));
        gs_rect ends = take(18);
        eh_w_line(W.ui, ends, "Closer to your words", 12, eh_ui_ink(W.ui, EH_TEXT_DIM, 1), -1);
        eh_w_line(W.ui, ends, "More polished", 12, eh_ui_ink(W.ui, EH_TEXT_DIM, 1), 1);
        pg.y += 8;
        char line[200];
        SDL_snprintf(line, sizeof line, "%s — %s", eh_styles[W.style].label, eh_styles[W.style].hint);
        text(line, 13, eh_ui_ink(W.ui, EH_TEXT, 1));
    } else {
        text("Temperature and top-p allow more varied wording as they rise; top-k and min-p are cutoffs that filter harder as they tighten. top-k and min-p apply to the built-in engine only — external services receive temperature and top-p.", 12, eh_ui_ink(W.ui, EH_TEXT_DIM, 1));
        pg.y += 14;
        number_field("Temperature (0–2)", "cleanup-temperature", W.temperature, sizeof W.temperature, "How much randomness each word choice gets. 0 always picks the most likely word; higher values vary the wording more. Cleanup works best low (0–0.4).");
        pg.y += 14;
        number_field("top-p (0–1)", "cleanup-top-p", W.top_p, sizeof W.top_p, "Samples only from the most likely words whose combined probability reaches this value. 1 = off; lower values cut the long tail of unlikely words.");
        pg.y += 14;
        number_field("top-k (0 = off)", "cleanup-top-k", W.top_k, sizeof W.top_k, "Keeps only the K most likely words at each step. Lower is stricter — 20–40 is typical; 0 turns it off.");
        pg.y += 14;
        number_field("min-p (0 = off)", "cleanup-min-p", W.min_p, sizeof W.min_p, "Drops words less likely than this fraction of the most likely word's probability. Higher is stricter — 0.05 is a light filter; 0 turns it off.");
        pg.y += 14;
        char from[96];
        SDL_snprintf(from, sizeof from, "Start from preset: %s", eh_styles[W.style].label);
        x = pg.x;
        if (button_at(&x, pg.y, "cleanup-custom-from-preset", from, EH_BTN_PLAIN)) {
            SDL_snprintf(W.temperature, sizeof W.temperature, "%.4g", eh_styles[W.style].temperature);
            SDL_snprintf(W.top_p, sizeof W.top_p, "%.4g", eh_styles[W.style].top_p);
            SDL_snprintf(W.top_k, sizeof W.top_k, "%d", eh_styles[W.style].top_k);
            SDL_snprintf(W.min_p, sizeof W.min_p, "%.4g", eh_styles[W.style].min_p);
        }
        pg.y += 34;
    }
    card_end();

    card_begin("Dictionary");
    label("Preferred terms (one per line)");
    eh_w_textarea(W.ui, take(128), "cleanup-dictionary", W.dictionary, sizeof W.dictionary, "Earheart\nsherpa-onnx\nKubernetes");
    hint("Names, jargon and product terms that often get misheard. When the transcript comes close to one, cleanup corrects it to the exact spelling listed here. Keep the list short and specific — a handful of terms works better than hundreds.");
    card_end();

    card_begin("System prompt");
    eh_w_textarea(W.ui, take(180), "cleanup-prompt", W.prompt, sizeof W.prompt, NULL);
    hint("Base instructions sent with every transcript. Preset editing instructions and dictionary terms are appended to this; Custom mode sends it as written.");
    pg.y += 10;
    x = pg.x;
    if (button_at(&x, pg.y, "cleanup-prompt-reset", "Reset to default", EH_BTN_PLAIN)) {
        gs_jval *d = gs_jval_parse(eh_defaults_json, sizeof eh_defaults_json - 1);
        SDL_strlcpy(W.prompt, gs_jval_string(gs_jval_path(d, "cleanup.systemPrompt"), ""), sizeof W.prompt);
        gs_jval_free(d);
    }
    pg.y += 34;
    card_end();
    eh_ui_inert(W.ui, false, false);
}

#define HISTORY_PAGE 5

static void when_text(char *out, size_t size, const gs_jval *item) {
    // "2026-10-11T22:51:07.123Z" shown as the day and the minute, in local time.
    const char *at = gs_jval_string(gs_jval_get(item, "at"), "");
    SDL_DateTime dt = { 0 };
    SDL_Time t;
    if (SDL_sscanf(at, "%d-%d-%dT%d:%d:%d", &dt.year, &dt.month, &dt.day, &dt.hour, &dt.minute, &dt.second) == 6 && SDL_DateTimeToTime(&dt, &t) && SDL_TimeToDateTime(t, &dt, true))
        SDL_snprintf(out, size, "%04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
    else SDL_strlcpy(out, at, size);
    if (gs_jval_boolean(gs_jval_get(item, "cleaned"), false)) SDL_strlcat(out, " · cleaned", size);
    if (gs_jval_boolean(gs_jval_get(item, "incomplete"), false)) SDL_strlcat(out, " · incomplete", size);
}

static void copy_start(const char *value, int row, int kind) {
    copy *c = SDL_calloc(1, sizeof *c);
    c->text = SDL_strdup(value);
    c->row = row, c->kind = kind;
    W.history_status.tone = ST_NONE;
    eh_spawn(copy_task, c);
}

static void history(void) {
    legend(SEC_HISTORY);
    card_begin(NULL);
    switch_row("history-enabled", "Keep a local history of transcriptions", "Stored on this computer only. Never uploaded anywhere.", &W.history_enabled);
    card_end();

    card_begin(NULL);
    gs_rect head = take(34);
    eh_w_line(W.ui, head, "Recent transcriptions", 13, gs_rgba(EH_TEXT, 1), -1);
    float cw = eh_w_button_width(W.ui, "Clear history");
    if (eh_w_button(W.ui, (gs_rect){ head.x + head.w - cw, head.y, cw, 34 }, "history-clear", "Clear history", EH_BTN_DANGER)) {
        eh_ui_ask(W.ui, "Clear all saved transcriptions? This can't be undone.", "Clear history", "Cancel", true);
        W.ask = ASK_CLEAR_HISTORY;
    }
    pg.y += 8;
    const gs_jval *items = eh_history_list();
    int count = gs_jval_count(items), pages = SDL_max(1, (count + HISTORY_PAGE - 1) / HISTORY_PAGE);
    W.history_page = SDL_min(W.history_page, pages - 1);
    if (!count) text("No transcriptions yet.", 13, gs_rgba(EH_TEXT_DIM, 1));
    int start = W.history_page * HISTORY_PAGE;
    bool copied = W.copied_at && SDL_GetTicks() - W.copied_at < 1200;
    if (copied) eh_ui_again(W.ui, 200);
    for (int i = start; i < count && i < start + HISTORY_PAGE; i++) {
        const gs_jval *item = gs_jval_at(items, i);
        const char *body = gs_jval_string(gs_jval_get(item, "text"), ""), *raw = gs_jval_string(gs_jval_get(item, "raw"), NULL);
        int row = i - start;
        char id[40], when[96];
        if (row) gs_ui_fill(eh_ui_gs(W.ui), take(1), EH_SEAM);
        pg.y += 10;
        // Three lines, unless the row was opened.
        float line = gs_ui_line_height(eh_ui_gs(W.ui), 13), full = eh_w_text_height(W.ui, body, 13, pg.w), shown = W.expanded & 1u << row ? full : SDL_min(full, line * 3);
        if (on_screen(pg.y, shown)) {
            bool open = (W.expanded & 1u << row) != 0;
            if (open) eh_w_text(W.ui, (gs_rect){ pg.x, pg.y, pg.w, full }, body, 13, gs_rgba(EH_TEXT, 1));
            else gs_ui_text_lines(eh_ui_gs(W.ui), (gs_rect){ pg.x, pg.y, pg.w, shown }, body, 13, gs_rgba(EH_TEXT, 1), 3, NULL);
        }
        pg.y += shown + 6;
        // When it was said, beside the row's buttons where both fit and on a line above them
        // where they do not.
        when_text(when, sizeof when, item);
        bool has_original = gs_jval_boolean(gs_jval_get(item, "cleaned"), false) && raw && SDL_strcmp(raw, body);
        float buttons = eh_w_button_width(W.ui, "Copy") + eh_w_button_width(W.ui, "Correct…") + 16 + (has_original ? eh_w_button_width(W.ui, "Copy original") + 8 : 0) + (full > line * 3 + 1 ? eh_w_button_width(W.ui, "Show more") + 8 : 0);
        bool beside = gs_ui_text_width(eh_ui_gs(W.ui), when, 12) + 16 + buttons <= pg.w;
        if (!beside) eh_w_line(W.ui, take(22), when, 12, gs_rgba(EH_TEXT_DIM, 1), -1);
        gs_rect meta = take(30);
        if (beside) eh_w_line(W.ui, meta, when, 12, gs_rgba(EH_TEXT_DIM, 1), -1);
        float x = meta.x + meta.w;
#define ROW_BUTTON(ID, LABEL) (SDL_snprintf(id, sizeof id, "history-%s-%d", ID, row), x -= eh_w_button_width(W.ui, LABEL) + 8, eh_w_button(W.ui, (gs_rect){ x + 8, meta.y, eh_w_button_width(W.ui, LABEL), 30 }, id, LABEL, EH_BTN_PLAIN))
        if (has_original)
            if (ROW_BUTTON("original", copied && W.copied_row == row && W.copied_kind == 1 ? "Copied" : "Copy original")) copy_start(raw, row, 1);
        if (ROW_BUTTON("correct", "Correct…")) {
            SDL_free(W.correct);
            W.correct = SDL_strdup(body);
            W.history_status.tone = ST_NONE;
        }
        if (ROW_BUTTON("copy", copied && W.copied_row == row && W.copied_kind == 0 ? "Copied" : "Copy")) copy_start(body, row, 0);
        if (full > line * 3 + 1 && ROW_BUTTON("more", W.expanded & 1u << row ? "Show less" : "Show more")) W.expanded ^= 1u << row;
#undef ROW_BUTTON
        pg.y += 8;
    }
    show(&W.history_status);
    if (pages > 1) {
        pg.y += 6;
        gs_rect pager = take(34);
        char range[64];
        SDL_snprintf(range, sizeof range, "%d–%d of %d", start + 1, SDL_min(count, start + HISTORY_PAGE), count);
        eh_w_line(W.ui, pager, range, 12, gs_rgba(EH_TEXT_DIM, 1), 0);
        float ow = eh_w_button_width(W.ui, "Older"), nw = eh_w_button_width(W.ui, "Newer");
        eh_ui_inert(W.ui, W.history_page == 0, W.history_page == 0);
        if (eh_w_button(W.ui, (gs_rect){ pager.x, pager.y, nw, 34 }, "history-newer", "Newer", EH_BTN_PLAIN)) W.history_page--, W.expanded = 0;
        eh_ui_inert(W.ui, W.history_page >= pages - 1, W.history_page >= pages - 1);
        if (eh_w_button(W.ui, (gs_rect){ pager.x + pager.w - ow, pager.y, ow, 34 }, "history-older", "Older", EH_BTN_PLAIN)) W.history_page++, W.expanded = 0;
        eh_ui_inert(W.ui, false, false);
    }
    card_end();
}

static void advanced(void) {
    legend(SEC_ADVANCED);
    card_begin("Performance");
    float x = pg.x, w = pg.w, top = pg.y;
    pg.w = (w - 16) / 2;
    number_field("Max dictation length (s)", "max-seconds", W.max_seconds, sizeof W.max_seconds, NULL);
    float bottom = pg.y;
    pg.x = x + pg.w + 16, pg.y = top;
    number_field("Unload models after idle (min)", "idle-unload", W.idle_unload, sizeof W.idle_unload, NULL);
    pg.x = x, pg.w = w, pg.y = SDL_max(bottom, pg.y);
    hint("Max dictation length — a dictation that reaches the cap stops and transcribes on its own; nothing is lost. Raising it lets the cleanup model hold more of what you say, at the cost of some memory; a dictation longer than it can hold is still transcribed and delivered, just as spoken. 10–3600 seconds; a value outside that is saved as the nearest limit, and a blank keeps the current one.");
    hint("Unload models after idle — built-in models stay in memory for fast repeat dictations, then unload after this many idle minutes to free memory (~1.5 GB or more); 0 keeps them loaded for the whole session. The next dictation reloads them in a few seconds, usually hidden while you're still speaking. Only affects in-app models, not external services. 0–240 minutes; a value outside that is saved as the nearest limit, and a blank means 0.");
    card_end();

    card_begin("Setup wizard");
    x = pg.x;
    if (button_at(&x, pg.y, "open-wizard", "Run the setup wizard again…", EH_BTN_PLAIN)) W.open_wizard = true;
    pg.y += 34;
    hint("Walks through hotkey, microphone, speech-to-text and cleanup from scratch. Nothing changes until you finish the wizard.");
    card_end();

    card_begin("About");
    text("Earheart v" EARHEART_VERSION " — private, hotkey-driven voice dictation. Free and open source (MIT).", 13, gs_rgba(EH_TEXT, 1));
    pg.y += 12;
    x = pg.x;
    if (button_at(&x, pg.y, "open-logs", "Open error log", EH_BTN_PLAIN)) {
        char path[1300], url[1400];
        SDL_snprintf(path, sizeof path, "%s" EH_SEP "earheart.log", eh_data_dir());
        SDL_snprintf(url, sizeof url, "file://%s%s", path[0] == '/' ? "" : "/", path);
        if (SDL_OpenURL(url)) say(&W.logs, ST_PLAIN, "%s", path);
        else say(&W.logs, ST_ERR, "Couldn't open it — find it at %s", path);
    }
    pg.y += 34;
    show(&W.logs);
    hint("Errors are written here — attach it when reporting a problem.");
    card_end();
}

/* ---------------- the window ---------------- */

static void answer_question(void) {
    int answer = eh_ui_modal(W.ui);
    if (!answer) return;
    if (answer == 1 && W.ask == ASK_REMOVE_MODEL && W.ask_model) {
        engine_draft *d = W.ask_model->kind == EH_MODEL_STT ? &W.stt : &W.cleanup;
        say(&d->manage, ST_PLAIN, "Removing…");
        eh_spawn(remove_task, (void *)W.ask_model);
    }
    if (answer == 1 && W.ask == ASK_REMOVE_CUSTOM && W.ask_model) {
        hf_job *j = SDL_calloc(1, sizeof *j);
        j->kind = W.ask_model->kind, j->d = j->kind == EH_MODEL_STT ? &W.stt : &W.cleanup, j->id = SDL_strdup(W.ask_model->id);
        say(&j->d->manage, ST_PLAIN, "Removing…");
        eh_spawn(custom_remove_task, j);
    }
    if (answer == 1 && W.ask == ASK_CLEAR_HISTORY) {
        eh_history_clear();
        W.history_page = 0;
    }
    W.ask = ASK_NONE;
}

void eh_settings_window_open(const char *section) {
    if (!W.ui) {
        W.ui = eh_ui_new("Earheart", 772, 780, 640, 480);
        if (!W.ui) return;
        eh_lock();
        gs_jval_free(W.base);
        W.base = gs_jval_clone(eh_settings());
        populate();
        W.save.tone = ST_NONE;
        W.saving = W.close_when_saved = W.wizard_banner = false;
        for (int i = 0; i < 4; i++) {
            // A failure is shown while the field still holds the key it was about.
            eh_hotkey_result r = eh_hotkeys_status(i);
            bool stale = !r.unbound && SDL_strcmp(r.accelerator, W.hotkeys[i]) != 0;
            say(&W.hotkey_status[i], !r.ok && !stale ? ST_ERR : ST_NONE, "%s", r.error);
        }
        eh_unlock();
        eh_scroll_to(W.ui, 0, false);
    }
    for (int i = 0; section && i < SECTIONS; i++)
        if (!SDL_strcmp(section, section_ids[i])) W.section = i, eh_scroll_to(W.ui, SDL_max(0, W.section_y[i] - 12), false);
    SDL_ShowWindow(eh_ui_window(W.ui));
    gs_window_activate(eh_ui_window(W.ui));  // to the front with the keyboard, which on macOS takes making the app active
}

void eh_settings_window_open_from_wizard(void) {
    // Open already, it is filled again: the wizard just saved over what its draft was made from.
    eh_settings_window_close();
    eh_settings_window_open("general");
    W.wizard_banner = W.ui != NULL;
}

bool eh_settings_window_is_open(void) { return W.ui != NULL; }

void eh_settings_window_close(void) {
    if (!W.ui) return;
    if (eh_ui_capturing(W.ui)) {
        eh_lock();
        eh_hotkeys_suspend(false);
        eh_unlock();
    }
    eh_ui_free(W.ui);
    W.ui = NULL;
}

bool eh_settings_window_event(const SDL_Event *e) { return W.ui && eh_ui_event(W.ui, e); }

int eh_settings_window_frame(void) {
    if (!W.ui) return -1;
    if (eh_ui_close_asked(W.ui) || W.close_when_saved) {
        W.close_when_saved = false;
        eh_settings_window_close();
        return -1;
    }
    eh_lock();
    bool was_capturing = eh_ui_capturing(W.ui);
    gs_rect area = eh_ui_begin(W.ui);
    gs_ui *gs = eh_ui_gs(W.ui);
    W.card = 0;

    // The header.
    gs_rect header = gs_cut_top(&area, 72);
    eh_w_icon(W.ui, (gs_rect){ 24, 18, 34, 34 });
    eh_w_line(W.ui, (gs_rect){ 71, 16, header.w - 90, 22 }, "Earheart", 16, gs_rgba(EH_TEXT, 1), -1);
    eh_w_line(W.ui, (gs_rect){ 71, 38, header.w - 90, 18 }, "Press the hotkey, speak, press it again — your words appear where you type.", 12, gs_rgba(EH_TEXT_DIM, 1), -1);
    gs_ui_fill(gs, (gs_rect){ 0, header.h - 1, header.w, 1 }, EH_SEAM);

    // The footer.
    gs_rect footer = gs_cut_bottom(&area, 64);
    gs_ui_fill(gs, (gs_rect){ 0, footer.y, footer.w, 1 }, EH_SEAM);
    float sw = eh_w_button_width(W.ui, "Save") + 24;

    // The index.
    gs_rect nav = gs_cut_left(&area, 180);
    gs_ui_fill(gs, (gs_rect){ nav.w - 1, nav.y, 1, nav.h }, EH_SEAM);

    // The page.
    W.view_top = area.y, W.view_bottom = area.y + area.h;
    float top = eh_scroll_begin(W.ui, area);
    pg.x = area.x + 24, pg.w = SDL_min(area.w - 48, 640), pg.y = top + 18;
    if (W.wizard_banner) {
        card_begin(NULL);
        float w = pg.w, bw = eh_w_button_width(W.ui, "Dismiss"), banner_top = pg.y;
        pg.w -= bw + 14;
        text("Setup complete. These settings were pre-configured by the setup wizard — save them as they are, or adjust anything first.", 13, gs_rgba(EH_TEXT, 1));
        pg.w = w;
        if (eh_w_button(W.ui, (gs_rect){ pg.x + pg.w - bw, banner_top, bw, 34 }, "wizard-banner-dismiss", "Dismiss", EH_BTN_PLAIN)) W.wizard_banner = false;
        pg.y = SDL_max(pg.y, banner_top + 34);
        card_end();
    }
    general();
    live();
    speech();
    cleanup();
    history();
    advanced();
    float content = pg.y - top + 10;
    if (SDL_fabsf(content - W.content_h) > 0.5f) W.content_h = content, eh_ui_again(W.ui, 0);
    for (int i = 0; i < SECTIONS; i++) W.section_y[i] -= top;
    eh_scroll_end(W.ui, pg.y + 10);

    // The index follows the scroll: the section whose top has passed the read line is the current
    // one, and the end of the page always lights the last. A jump holds its entry until it lands.
    float y = eh_scroll_y(W.ui);
    if (!W.spy_held) {
        int at = 0;
        for (int i = 0; i < SECTIONS; i++)
            if (W.section_y[i] - 40 <= y) at = i;
        if (y + area.h >= W.content_h - 4) at = SECTIONS - 1;
        W.section = at;
    } else if (SDL_fabsf(y - SDL_clamp(W.section_y[W.section] - 12, 0, SDL_max(0, W.content_h - area.h))) < 1) {
        W.spy_held = false;
    }
    for (int i = 0; i < SECTIONS; i++) {
        char id[32];
        SDL_snprintf(id, sizeof id, "tab-%s", section_ids[i]);
        gs_rect r = { 12, nav.y + 14 + (float)i * 36, nav.w - 24, 34 };
        if (eh_w_button(W.ui, r, id, section_names[i], i == W.section ? EH_BTN_NAV_ON : EH_BTN_NAV)) {
            W.section = i;
            W.spy_held = true;
            eh_scroll_to(W.ui, SDL_max(0, W.section_y[i] - 12), true);
            // The keyboard goes into the section, so Tab carries on from there and not from the
            // top of the page.
            static const char *const first[SECTIONS] = { "hotkey", "stt-live-preview", "stt-engine-builtin", "cleanup-enabled", "history-enabled", "max-seconds" };
            eh_ui_focus(W.ui, first[i]);
        }
        float s = gs_ui_scale(gs);
        gs_draw_circle(SDL_GetRenderer(eh_ui_window(W.ui)), (r.x + r.w - 14) * s, (r.y + 17) * s, 3 * s, gs_rgba(section_colours[i], i == W.section ? 1 : 0.55f));
    }

    // Save, and what it says.
    if (W.save_clear_at && SDL_GetTicks() >= W.save_clear_at) W.save.tone = ST_NONE, W.save_clear_at = 0;
    if (W.save_clear_at) eh_ui_again(W.ui, 250);
    if (W.save.tone != ST_NONE)
        eh_w_line(W.ui, (gs_rect){ 24, footer.y, footer.w - sw - 60, footer.h }, W.save.text, 12, gs_rgba(W.save.tone == ST_OK ? EH_OK : W.save.tone == ST_ERR ? EH_ERR : EH_TEXT_DIM, 1), 1);
    eh_ui_inert(W.ui, W.saving, false);
    if (eh_w_button(W.ui, (gs_rect){ footer.w - sw - 24, footer.y + 14, sw, 36 }, "save", "Save", EH_BTN_PRIMARY)) {
        W.saving = true;
        say(&W.save, ST_PLAIN, "Saving…");
        W.save_clear_at = 0;
        eh_spawn(save_task, collect());
    }
    eh_ui_inert(W.ui, false, false);
    if (W.saving || eh_model_jobs_active()) eh_ui_again(W.ui, 100);

    answer_question();
    // While a hotkey field waits for keys, the hotkeys in force are let go, so pressing the
    // current one reaches the field and does not start a dictation.
    if (eh_ui_capturing(W.ui) != was_capturing) eh_hotkeys_suspend(eh_ui_capturing(W.ui));
    int again = eh_ui_end(W.ui);
    eh_unlock();
    if (W.open_wizard) {
        W.open_wizard = false;
        eh_wizard_window_open();
    }
    if (W.correct) {
        char error[200];
        if (!eh_correction_window_open(W.correct, error, sizeof error)) say(&W.history_status, ST_ERR, "%s", error);
        SDL_free(W.correct);
        W.correct = NULL;
        again = 0;
    }
    return again;
}

/* ---------------- for tests ---------------- */

struct eh_ui *eh_settings_window_ui(void) { return W.ui; }

const char *eh_settings_window_text(const char *what) {
    static const char *const ids[4] = { "hotkey", "pause-hotkey", "discard-hotkey", "correct-hotkey" };
    static char status_id[40];
    for (int i = 0; i < 4; i++) {
        if (!SDL_strcmp(what, ids[i])) return W.hotkeys[i];
        SDL_snprintf(status_id, sizeof status_id, "%s-status", ids[i]);
        if (!SDL_strcmp(what, status_id)) return W.hotkey_status[i].tone == ST_NONE ? "" : W.hotkey_status[i].text;
    }
    const status *s = !SDL_strcmp(what, "save") ? &W.save : !SDL_strcmp(what, "stt-test") ? &W.stt.test : !SDL_strcmp(what, "stt-fetch") ? &W.stt.fetch
                    : !SDL_strcmp(what, "cleanup-test") ? &W.cleanup.test : !SDL_strcmp(what, "cleanup-fetch") ? &W.cleanup.fetch
                    : !SDL_strcmp(what, "stt-hf") ? &W.stt.hf : !SDL_strcmp(what, "cleanup-hf") ? &W.cleanup.hf : NULL;
    if (s) return s->tone == ST_NONE ? "" : s->text;
    if (!SDL_strcmp(what, "section")) return section_ids[W.section];
    if (!SDL_strcmp(what, "max-seconds")) return W.max_seconds;
    return "";
}

bool eh_settings_window_untouched(void) {
    gs_jval *now = collect();
    bool same = gs_jval_equal(now, W.base);
    if (!same && SDL_getenv("EARHEART_SMOKE_VERBOSE")) {
        for (int i = 0; i < gs_jval_count(W.base); i++) {
            const gs_jval *a = gs_jval_at(W.base, i), *b = gs_jval_get(now, gs_jval_key(W.base, i));
            if (gs_jval_equal(a, b)) continue;
            char *was = eh_json_text(a, false), *is = b ? eh_json_text(b, false) : SDL_strdup("(missing)");
            SDL_Log("%s: %s -> %s", gs_jval_key(W.base, i), was, is);
            SDL_free(was);
            SDL_free(is);
        }
    }
    gs_jval_free(now);
    return same;
}
