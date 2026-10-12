// `earheart dictate`: one or more whole dictations without a window or a real microphone, for
// tests and measurements. A WAV file plays as the microphone at its own pace, so the run goes
// through what a user's dictation does: capture, live-preview chunks, the final transcription,
// cleanup and delivery (into the result, in place of the clipboard). It prints each event as a
// JSON line when asked and one result line per run.
//
//   earheart dictate --wav FILE [--talk SECONDS] [--runs N] [--events] [--data DIR]
//                    [--set PATH=JSON]... [--pause-at S --resume-at S] [--kill-stt-at-stop]
// --talk stops the dictation that long after the microphone went live (default: the file's
// length). --set changes a setting for the run, for example --set cleanup.enabled=false.
// --data names the folder for this run's settings and history (default: a fresh temporary one).
// --kill-stt-at-stop makes the speech worker die on every decode from the stop on: what is
// delivered then is what the live preview had already decoded, marked incomplete.
#include <SDL3/SDL.h>
#include <stdio.h>
#include "files.h"
#include "app.h"
#include "loop.h"
#include "paths.h"
#include "recorder.h"
#include "settings.h"
#include "wav.h"

static struct {
    bool events;
    Uint64 started, live_at, stop_at;
    bool finished;         // done, empty or error was reached
    char outcome[16];
    char *delivered;       // what delivery was handed
    char *raw, *cleaned;   // the live transcript's last lines
    char error[256];
    gs_jval *phases;         // when each status after the stop began, in ms
    int partials, cleaned_partials, notices;
    bool incomplete;
} D;

static void observer(const char *kind, const char *a, const char *b, double number, const gs_jval *detail) {
    Uint64 now = SDL_GetTicks();
    if (!SDL_strcmp(kind, "status")) {
        if (!SDL_strcmp(a, "recording") && !D.live_at) D.live_at = now;
        if (D.stop_at) gs_jval_set(D.phases, a, gs_jval_num((double)(now - D.stop_at)));
        if (!SDL_strcmp(a, "done") || !SDL_strcmp(a, "empty") || !SDL_strcmp(a, "error")) {
            D.finished = true;
            SDL_strlcpy(D.outcome, a, sizeof D.outcome);
            SDL_strlcpy(D.error, gs_jval_string(gs_jval_get(detail, "message"), ""), sizeof D.error);
            D.incomplete = gs_jval_boolean(gs_jval_get(detail, "incomplete"), false);
        }
    } else if (!SDL_strcmp(kind, "partial")) {
        bool cleaned = !SDL_strcmp(a, "cleaned");
        char **slot = cleaned ? &D.cleaned : &D.raw;
        SDL_free(*slot);
        *slot = SDL_strdup(b);
        if (cleaned) D.cleaned_partials++; else D.partials++;
    } else if (!SDL_strcmp(kind, "notify")) {
        D.notices++;
    }
    if (!D.events) return;
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "event", gs_jval_str(kind));
    gs_jval_set(o, "ms", gs_jval_num((double)(now - D.started)));
    if (a) gs_jval_set(o, "a", gs_jval_str(a));
    if (b) gs_jval_set(o, "b", gs_jval_str(b));
    if (!SDL_strcmp(kind, "progress")) gs_jval_set(o, "fraction", gs_jval_num(number));
    if (detail) gs_jval_set(o, "detail", gs_jval_clone(detail));
    char *line = eh_json_text(o, false);
    puts(line);
    fflush(stdout);
    SDL_free(line);
    gs_jval_free(o);
}

static bool deliver(const char *text, const gs_jval *output, eh_signal *signal, eh_delivery *result, char *error, size_t error_size) {
    SDL_free(D.delivered);
    D.delivered = SDL_strdup(text);
    SDL_strlcpy(result->method, "clipboard", sizeof result->method);
    return true;
}

static int usage(void) {
    fprintf(stderr, "usage: earheart dictate --wav FILE [--talk SECONDS] [--runs N] [--events] [--data DIR] [--set PATH=JSON]... [--pause-at S --resume-at S]\n");
    return 2;
}

int eh_cmd_dictate(int argc, char **argv) {
    const char *wav_path = NULL, *data = NULL, *sets[32];
    double talk = 0, pause_at = 0, resume_at = 0;
    int runs = 1, set_count = 0;
    bool kill_stt = false;
    for (int i = 0; i < argc; i++) {
        bool more = i + 1 < argc;
        if (!SDL_strcmp(argv[i], "--wav") && more) wav_path = argv[++i];
        else if (!SDL_strcmp(argv[i], "--talk") && more) talk = SDL_atof(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--runs") && more) runs = SDL_atoi(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--data") && more) data = argv[++i];
        else if (!SDL_strcmp(argv[i], "--pause-at") && more) pause_at = SDL_atof(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--resume-at") && more) resume_at = SDL_atof(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--set") && more && set_count < 32) sets[set_count++] = argv[++i];
        else if (!SDL_strcmp(argv[i], "--events")) D.events = true;
        else if (!SDL_strcmp(argv[i], "--kill-stt-at-stop")) kill_stt = true;
        else return usage();
    }
    if (!wav_path) return usage();
    size_t len;
    uint8_t *wav = SDL_LoadFile(wav_path, &len);
    if (!wav) { fprintf(stderr, "earheart: %s: %s\n", wav_path, SDL_GetError()); return 1; }
    if (talk <= 0) talk = eh_wav_seconds(wav, len);
    SDL_free(wav);

    char temp[1200];
    if (!data) {
        SDL_snprintf(temp, sizeof temp, "%s" EH_SEP "dictate-%llu", eh_data_dir(), (unsigned long long)SDL_GetTicksNS());
        data = temp;
    }
    D.started = SDL_GetTicks();
    char die_file[1300];
    SDL_snprintf(die_file, sizeof die_file, "%s" EH_SEP "stt-dies", data);
    SDL_CreateDirectory(data);
    SDL_RemovePath(die_file);
    if (kill_stt) SDL_setenv_unsafe("EARHEART_TEST_STT_DIE_FILE", die_file, 1);  // the workers inherit it
    eh_app_init(data, observer, deliver);
    eh_lock();
    char error[256];
    for (int i = 0; i < set_count; i++) {
        const char *eq = SDL_strchr(sets[i], '=');
        char path[128];
        gs_jval *value = eq ? gs_jval_parse(eq + 1, SDL_strlen(eq + 1)) : NULL;
        if (!value || (size_t)(eq - sets[i]) >= sizeof path) { fprintf(stderr, "earheart: --set wants PATH=JSON, got %s\n", sets[i]); return 2; }
        SDL_strlcpy(path, sets[i], (size_t)(eq - sets[i]) + 1);
        if (!eh_setting_set(path, value, error, sizeof error)) { fprintf(stderr, "earheart: %s\n", error); return 1; }
    }
    eh_recorder_fake_microphone(wav_path, 1);
    eh_unlock();

    int failed = 0;
    for (int run = 1; run <= runs; run++) {
        D.live_at = D.stop_at = 0;
        D.finished = false;
        D.partials = D.cleaned_partials = D.notices = 0;
        SDL_free(D.delivered);
        SDL_free(D.raw);
        SDL_free(D.cleaned);
        D.delivered = D.raw = D.cleaned = NULL;
        gs_jval_free(D.phases);
        D.phases = gs_jval_object();
        eh_lock();
        eh_pipeline_toggle();
        bool recording = eh_pipeline_state() == EH_RECORDING;
        eh_unlock();
        if (!recording) { fprintf(stderr, "earheart: the dictation did not start\n"); return 1; }
        bool paused = false, resumed = false;
        for (;;) {
            SDL_Delay(5);
            eh_lock();
            Uint64 now = SDL_GetTicks(), live = D.live_at;
            double spoken = live ? (double)(now - live) / 1000 : 0;
            if (live && pause_at > 0 && !paused && spoken >= pause_at) { paused = true; eh_pipeline_pause_toggle(); }
            if (paused && !resumed && spoken >= resume_at) { resumed = true; eh_pipeline_pause_toggle(); }
            // The stop: the record hotkey again, once the talking is done.
            if (live && !D.stop_at && eh_recorder_seconds() >= talk) {
                if (kill_stt) SDL_CloseIO(SDL_IOFromFile(die_file, "w"));
                D.stop_at = now;
                eh_pipeline_toggle();
            }
            bool over = D.finished && eh_pipeline_state() == EH_IDLE;
            eh_unlock();
            if (over) break;
        }
        eh_lock();
        gs_jval *o = gs_jval_object();
        gs_jval_set(o, "run", gs_jval_num(run));
        gs_jval_set(o, "outcome", gs_jval_str(D.outcome));
        gs_jval_set(o, "text", gs_jval_str(D.delivered ? D.delivered : ""));
        gs_jval_set(o, "incomplete", gs_jval_bool(D.incomplete));
        if (D.error[0]) gs_jval_set(o, "error", gs_jval_str(D.error));
        gs_jval_set(o, "stopToDoneMs", gs_jval_num((double)(SDL_GetTicks() - D.stop_at)));
        gs_jval_set(o, "phaseStartsMs", gs_jval_clone(D.phases));
        gs_jval_set(o, "partials", gs_jval_num(D.partials));
        gs_jval_set(o, "cleanedPartials", gs_jval_num(D.cleaned_partials));
        gs_jval_set(o, "lastPartial", gs_jval_str(D.raw ? D.raw : ""));
        gs_jval_set(o, "notices", gs_jval_num(D.notices));
        char *line = eh_json_text(o, false);
        puts(line);
        fflush(stdout);
        SDL_free(line);
        gs_jval_free(o);
        if (SDL_strcmp(D.outcome, "done")) failed = 1;
        eh_unlock();
    }
    eh_lock();
    eh_engines_stop();
    eh_unlock();
    return failed;
}
