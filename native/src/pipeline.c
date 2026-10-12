#include "pipeline.h"
#include <SDL3/SDL.h>
#include <math.h>
#include "chunked.h"
#include "files.h"
#include "history.h"
#include "loop.h"
#include "preview.h"
#include "settings.h"
#include "text.h"
#include "wav.h"

#define BODY_MAX 180               // notification bodies are clipped, so a long native error cannot push the rest out of view
#define IDLE_UNLOAD_RETRY_MS 15000 // how soon to look again at a worker that was busy when the idle window ended
#define STT_PROGRESS_TICK_MS 120   // cadence of the estimated transcribing bar

static struct {
    eh_pipeline_deps deps;
    eh_state state;
    unsigned session;         // the current dictation
    eh_signal *signal;        // aborts the processing in flight
    unsigned idle_timer;
    bool idle_retrying;       // the armed timer is a short retry, not the full window
    // The final decode reports no progress, so the transcribing bar runs on an estimate: elapsed
    // time against the audio's length times the decode speed measured on earlier dictations.
    char *rtf_path;
    double rtf;
} P;

/* ---------------- decode speed ---------------- */

#define RTF_INITIAL 0.25  // a conservative guess: the model is faster than real time on a CPU
#define RTF_ALPHA 0.3     // weight of the newest observation
#define RTF_MIN 0.02
#define RTF_MAX 2
#define RTF_CAP 0.9       // estimated progress never exceeds this; only the answer says done

static double rtf_clamp(double x) { return SDL_min(RTF_MAX, SDL_max(RTF_MIN, x)); }

static void rtf_load(void) {
    P.rtf = rtf_clamp(RTF_INITIAL);
    size_t len;
    char *text = SDL_LoadFile(P.rtf_path, &len);
    gs_jval *stored = text ? gs_jval_parse(text, len) : NULL;
    double value = gs_jval_number(gs_jval_get(stored, "rtf"), 0);
    if (isfinite(value) && value > 0) P.rtf = rtf_clamp(value);
    gs_jval_free(stored);
    SDL_free(text);
}

static void rtf_record(double audio_sec, double elapsed_sec) {
    if (!isfinite(audio_sec) || audio_sec <= 0 || !isfinite(elapsed_sec) || elapsed_sec <= 0) return;
    double next = rtf_clamp(P.rtf + RTF_ALPHA * (elapsed_sec / audio_sec - P.rtf));
    if (next == P.rtf) return;
    P.rtf = next;
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "rtf", gs_jval_num(next));
    char *json = eh_json_text(o, false), error[256];
    eh_write_file_atomic(P.rtf_path, json, SDL_strlen(json), error, sizeof error);  // losing a sample is harmless
    SDL_free(json);
    gs_jval_free(o);
}

static double rtf_progress_at(double elapsed_sec, double audio_sec) {
    if (!isfinite(elapsed_sec) || elapsed_sec <= 0 || !isfinite(audio_sec) || audio_sec <= 0) return 0;
    double expected = audio_sec * P.rtf;
    return expected <= 0 ? 0 : SDL_min(RTF_CAP, elapsed_sec / expected);
}

/* ---------------- state ---------------- */

static void notify(const char *title, const char *body) {
    char clipped[BODY_MAX + 1];
    SDL_strlcpy(clipped, body, sizeof clipped);
    // Failure notices do not open Settings when the system shows no notifications: the overlay
    // has already said what happened.
    P.deps.notify(title, clipped, false);
}

static void arm_idle_unload(int delay_ms);

static void idle_unload(void *user) {
    P.idle_timer = 0;
    P.idle_retrying = false;
    // Only when still idle: a dictation in flight arms the timer again when it finishes.
    if (P.state != EH_IDLE) return;
    // A worker with a request in flight (a test from Settings) is skipped, and nothing else would
    // look at it again: without this retry its model would stay loaded until the next dictation.
    if (!eh_engines_unload_idle() && P.state == EH_IDLE) arm_idle_unload(IDLE_UNLOAD_RETRY_MS);
}

static void cancel_idle_unload(void) {
    eh_cancel(P.idle_timer);
    P.idle_timer = 0;
    P.idle_retrying = false;
}

// Arms the eviction timer. A negative delay means the configured window.
static void arm_idle_unload(int delay_ms) {
    cancel_idle_unload();
    double minutes = eh_setting_num("engines.idleUnloadMinutes");
    if (!(minutes > 0)) return;  // 0: the models stay loaded
    P.idle_retrying = delay_ms >= 0;
    P.idle_timer = eh_after(delay_ms >= 0 ? delay_ms : (int)(minutes * 60 * 1000), idle_unload, NULL);
}

static void set_state(eh_state next) {
    P.state = next;
    // The models stay loaded while a dictation is active; idle time counts from the return to idle.
    if (next == EH_IDLE) arm_idle_unload(-1);
    else cancel_idle_unload();
    if (P.deps.state_changed) P.deps.state_changed(next);
}

eh_state eh_pipeline_state(void) { return P.state; }

void eh_pipeline_settings_changed(void) {
    // A save landing inside a busy-worker retry keeps the retry's short delay.
    if (P.state == EH_IDLE) arm_idle_unload(P.idle_retrying ? IDLE_UNLOAD_RETRY_MS : -1);
}

static void hide_task(void *user) {
    unsigned sid = *(unsigned *)user;
    SDL_free(user);
    // Only if no new session started meanwhile.
    if (P.session == sid && P.state == EH_IDLE) P.deps.overlay_show(false);
}

static void hide_overlay_soon(unsigned sid, int ms) {
    unsigned *arg = SDL_malloc(sizeof *arg);
    *arg = sid;
    eh_after(ms, hide_task, arg);
}

static void error_status(const char *message) {
    gs_jval *detail = gs_jval_object();
    gs_jval_set(detail, "message", gs_jval_str(message));
    P.deps.status("error", detail);
    gs_jval_free(detail);
}

/* ---------------- decoding ---------------- */

typedef struct { const gs_jval *stt; eh_signal *signal; unsigned sid; bool check_session; } decode_ctx;

static eh_decode_code run_transcribe(void *user, const uint8_t *wav, size_t len, char **text, int *decode_ms, char *error, size_t error_size) {
    decode_ctx *c = user;
    return eh_transcribe(c->stt, c->signal, wav, len, text, decode_ms, error, error_size);
}

static void restart_stt(void *user) { eh_engines_restart_stt(); }

static bool decode_stale(void *user) {
    decode_ctx *c = user;
    return (c->check_session && P.session != c->sid) || eh_signal_aborted(c->signal);
}

// A live-preview chunk goes through the same piecewise decoding as the final pass: its text is
// reused verbatim in the final transcript, and a chunk holding two sentences is exactly what makes
// the model drop one of them. A chunk that decodes only in part fails, so the preview marks its
// snapshot broken and the final pass decodes that audio again.
static char *transcribe_live(const uint8_t *wav, size_t len, const gs_jval *stt, eh_signal *signal, char **partial_text, char *error, size_t error_size) {
    decode_ctx ctx = { stt, signal, 0, false };
    eh_chunked o = { .run = run_transcribe, .user = &ctx, .restart_stt = restart_stt, .stale = decode_stale, .min_sound_sec = -1, .min_utterance_sec = -1 };
    eh_chunked_result r;
    char *text = NULL;
    if (!eh_transcribe_chunked(wav, len, &o, &r)) {
        SDL_strlcpy(error, r.error[0] ? r.error : "live chunk decode failed", error_size);
    } else if (r.stale) {
        SDL_strlcpy(error, "aborted", error_size);
    } else if (r.partial) {
        SDL_strlcpy(error, "live chunk decode incomplete", error_size);
        if (r.text && *r.text) *partial_text = SDL_strdup(r.text);
    } else {
        text = SDL_strdup(r.text);
    }
    eh_chunked_result_free(&r);
    return text;
}

static char *clean_live(const char *raw, const gs_jval *cleanup, eh_signal *signal, char *error, size_t error_size) {
    return eh_clean(raw, cleanup, signal, NULL, NULL, error, error_size);
}

static bool recording_session(unsigned sid) { return sid == P.session && P.state == EH_RECORDING; }
static void preview_error(const char *message) { SDL_Log("live preview partial failed: %s", message); }

typedef struct { unsigned sid; eh_signal *signal; Uint64 started; double duration; int decode_ms; bool have_decode_ms; } estimate;

static bool stale(unsigned sid, const eh_signal *signal) { return P.session != sid || eh_signal_aborted(signal); }

static void estimate_tick(void *user) {
    estimate *e = user;
    if (stale(e->sid, e->signal)) return;
    P.deps.progress("transcribing", (float)rtf_progress_at((double)(SDL_GetTicks() - e->started) / 1000, e->duration));
}

typedef struct { decode_ctx ctx; estimate *e; } final_ctx;

static eh_decode_code final_run(void *user, const uint8_t *wav, size_t len, char **text, int *decode_ms, char *error, size_t error_size) {
    return run_transcribe(&((final_ctx *)user)->ctx, wav, len, text, decode_ms, error, error_size);
}
static bool final_stale(void *user) { return decode_stale(&((final_ctx *)user)->ctx); }
static void final_decode_ms(void *user, int ms) {
    estimate *e = ((final_ctx *)user)->e;
    e->decode_ms += ms;
    e->have_decode_ms = true;
}

// The final transcription, with the estimated transcribing bar. `assembly` is the live preview's
// snapshot (built-in speech only): when its committed chunks cover the recording's start intact,
// only the tail past them is decoded and joined onto the committed text, so stop-to-transcript
// stays near constant however long the dictation ran. Otherwise the whole recording is decoded,
// and a broken snapshot's words are salvage for the ranges that fail. False with `error` set when
// nothing could be recovered.
static bool transcribe_with_estimate(const uint8_t *wav, size_t len, const gs_jval *stt, unsigned sid, eh_signal *signal, eh_preview_snapshot *assembly,
                                     char **text, bool *partial, char *error, size_t error_size) {
    bool builtin = !SDL_strcmp(gs_jval_string(gs_jval_get(stt, "engine"), ""), "builtin");
    *text = NULL;
    *partial = false;
    const uint8_t *decode_wav = wav;
    size_t decode_len = len;
    uint8_t *tail = NULL;
    bool tail_only = false;
    const char *prefix = "", *salvage_text = "";
    const eh_salvage *salvage = NULL;
    int salvage_count = 0;
    if (builtin && assembly) {
        if (!assembly->broken && assembly->decoded_samples > 0) {
            tail = eh_wav_slice(wav, len, assembly->decoded_samples, 1LL << 40, &decode_len);
            if (tail) { decode_wav = tail; tail_only = true; prefix = assembly->committed_raw; }
            else decode_len = len;
        }
        if (!tail_only) {
            salvage_text = assembly->committed_raw;
            salvage = assembly->chunks;
            salvage_count = assembly->chunk_count;
        }
    }
    bool ok = true;
    if (builtin) {
        // The model is loaded before the clock starts: a cold load takes seconds and would both
        // freeze the bar at its cap and poison the speed sample with time that is not decoding.
        // A load that fails can decode nothing, but the words the live preview already decoded
        // are still the user's: those are delivered.
        if (!eh_ensure_stt(gs_jval_string(gs_jval_path(stt, "builtin.model"), ""), error, error_size)) {
            const char *recovered = *prefix ? prefix : salvage_text;
            if (*recovered) {
                SDL_Log("STT model load failed; delivering the live-preview text: %s", error);
                *text = SDL_strdup(recovered);
                *partial = true;
            } else {
                ok = false;
            }
            SDL_free(tail);
            return ok;
        }
        if (stale(sid, signal)) { *text = SDL_strdup(""); SDL_free(tail); return true; }
        // An effectively empty tail (stop landed right on a chunk boundary): the committed text
        // is the transcript, and nothing needs decoding.
        if (tail_only && eh_wav_seconds(decode_wav, decode_len) < 0.05) {
            P.deps.progress("transcribing", 1);
            *text = SDL_strdup(prefix);
            SDL_free(tail);
            return true;
        }
    }
    estimate e = { sid, signal, SDL_GetTicks(), eh_wav_seconds(decode_wav, decode_len), 0, false };
    unsigned tick = builtin ? eh_every(STT_PROGRESS_TICK_MS, estimate_tick, &e) : 0;
    if (!builtin) {
        // A remote service: one upload, as its API expects, and no local estimate worth showing.
        decode_ctx ctx = { stt, signal, sid, true };
        ok = run_transcribe(&ctx, decode_wav, decode_len, text, NULL, error, error_size) == EH_DECODE_OK;
    } else {
        final_ctx ctx = { { stt, signal, sid, true }, &e };
        eh_chunked o = {
            .run = final_run, .user = &ctx, .restart_stt = restart_stt, .stale = final_stale, .on_decode_ms = final_decode_ms,
            .min_sound_sec = -1, .min_utterance_sec = -1,
            .prefix_text = prefix, .salvage = salvage, .salvage_count = salvage_count, .salvage_text = salvage_text,
        };
        eh_chunked_result r;
        ok = eh_transcribe_chunked(decode_wav, decode_len, &o, &r);
        if (ok) {
            *text = SDL_strdup(r.text ? r.text : "");
            *partial = r.partial;
            if (!stale(sid, signal)) {
                // The speed sample is the engine's own decode time, not the wall clock, which also
                // holds queueing behind a live-preview decode. A partial run covers only some of
                // the audio and teaches nothing.
                if (e.have_decode_ms && !r.partial) rtf_record(e.duration, e.decode_ms / 1000.0);
                P.deps.progress("transcribing", 1);
            }
        } else {
            SDL_strlcpy(error, r.error[0] ? r.error : "transcription failed", error_size);
        }
        eh_chunked_result_free(&r);
    }
    eh_cancel(tick);
    SDL_free(tail);
    return ok;
}

/* ---------------- one dictation ---------------- */

typedef struct { unsigned sid; eh_signal *signal; } clean_ctx;

// Called from the engine's reading thread, without the lock.
static void clean_progress(void *user, float fraction) {
    clean_ctx *c = user;
    eh_lock();
    if (!stale(c->sid, c->signal)) P.deps.progress("cleaning", fraction);
    eh_unlock();
}

typedef struct {
    unsigned sid;
    eh_signal *signal;
    gs_jval *cfg;
    uint8_t *wav;
    size_t len;
    eh_preview_snapshot assembly;
    bool has_assembly;
} dictation;

static void process_task(void *user) {
    dictation *d = user;
    unsigned sid = d->sid;
    eh_signal *signal = d->signal;
    const gs_jval *cfg = d->cfg, *cleanup = gs_jval_get(cfg, "cleanup");
    char error[256] = "", *raw = NULL, *text = NULL;
    bool partial = false, cleaned = false, delivered = false, failed = false;

    failed = !transcribe_with_estimate(d->wav, d->len, gs_jval_get(cfg, "stt"), sid, signal, d->has_assembly ? &d->assembly : NULL, &raw, &partial, error, sizeof error);
    if (failed || stale(sid, signal)) goto done;
    if (!*raw) {
        P.deps.status("empty", NULL);
        hide_overlay_soon(sid, 1800);
        goto done;
    }
    if (partial) {
        // Some of the audio could not be decoded. What was recovered still goes through cleanup
        // and delivery like any dictation; this says it is not the whole thing.
        SDL_Log("transcription incomplete: delivering the recovered text");
        notify("Earheart: transcription interrupted", "The speech engine stopped part-way; delivered the text recovered so far.");
    }
    if (gs_jval_boolean(gs_jval_get(cleanup, "enabled"), false)) {
        P.deps.status("cleaning", NULL);
        clean_ctx cc = { sid, signal };
        char clean_error[256] = "";
        text = eh_clean(raw, cleanup, signal, clean_progress, &cc, clean_error, sizeof clean_error);
        if (text) {
            cleaned = true;
            if (!SDL_strcmp(gs_jval_string(gs_jval_get(cleanup, "engine"), ""), "builtin") && !stale(sid, signal)) P.deps.progress("cleaning", 1);
        } else if (!stale(sid, signal)) {
            // Cleanup is an enhancement, never a gate: the raw transcript becomes the text, and
            // the user is told why.
            SDL_Log("cleanup failed: %s", clean_error);
            notify("Earheart: cleanup failed, used raw transcript", clean_error);
        }
        if (stale(sid, signal)) goto done;
    }
    if (!text) text = SDL_strdup(raw);
    // History first: the final text is the user's as soon as it exists, whatever delivery does.
    if (gs_jval_boolean(gs_jval_path(cfg, "history.enabled"), false)) {
        eh_history_add(raw, text, cleaned, partial, (int)gs_jval_number(gs_jval_path(cfg, "history.limit"), 0));
        if (P.deps.history_changed) P.deps.history_changed();
    }
    P.deps.status("delivering", NULL);
    eh_delivery result = { 0 };
    if (!P.deps.deliver(text, gs_jval_get(cfg, "output"), signal, &result, error, sizeof error)) { failed = true; goto done; }
    // The words are on the clipboard or pasted; nothing after this may say they were lost.
    delivered = true;
    if (stale(sid, signal)) goto done;
    // The overlay's detail row clips after a couple of dozen characters and hides itself seconds
    // later; the full instruction needs somewhere to stay.
    if (result.hint[0]) notify("Earheart: auto-paste failed, copied to clipboard", result.hint);
    {
        gs_jval *detail = gs_jval_object();
        // The first 120 characters, counted as JavaScript counts them, and an ellipsis.
        const char *p = text;
        int units = 0;
        while (*p && units < 120) units += eh_utf8_next(&p) > 0xffff ? 2 : 1;
        char *preview = SDL_malloc((size_t)(p - text) + 8);
        SDL_memcpy(preview, text, (size_t)(p - text));
        SDL_strlcpy(preview + (p - text), *p ? "…" : "", 8);
        gs_jval_set(detail, "preview", gs_jval_str(preview));
        SDL_free(preview);
        gs_jval_set(detail, "method", gs_jval_str(result.method));
        gs_jval_set(detail, "note", gs_jval_str(result.note));
        gs_jval_set(detail, "incomplete", gs_jval_bool(partial));
        P.deps.status("done", detail);
        gs_jval_free(detail);
    }
    hide_overlay_soon(sid, result.note[0] ? 4000 : 1600);

done:
    if (failed && !stale(sid, signal)) {
        SDL_Log("pipeline failed: %s", error);
        if (delivered) {
            hide_overlay_soon(sid, 1600);
        } else {
            char message[201];
            SDL_strlcpy(message, error, sizeof message);
            error_status(message);
            // Nothing reached the target app. A transcription failure loses the dictation outright;
            // a delivery failure leaves the words in History. Either way the overlay line is gone
            // in five seconds, so the notice has to say it.
            notify("Earheart: dictation failed, nothing was delivered", error);
            hide_overlay_soon(sid, 5000);
        }
    }
    if (P.signal == signal) {
        eh_signal_unref(P.signal);
        P.signal = NULL;
    }
    if (P.session == sid) set_state(EH_IDLE);
    SDL_free(raw);
    SDL_free(text);
    if (d->has_assembly) eh_preview_snapshot_free(&d->assembly);
    eh_signal_unref(d->signal);
    gs_jval_free(d->cfg);
    SDL_free(d->wav);
    SDL_free(d);
}

void eh_pipeline_captured(unsigned sid, uint8_t *wav, size_t len) {
    if (sid != P.session || P.state != EH_RECORDING) { SDL_free(wav); return; }
    dictation *d = SDL_calloc(1, sizeof *d);
    d->sid = sid;
    d->cfg = gs_jval_clone(eh_settings());
    d->wav = wav;
    d->len = len;
    // The committed chunk decodes are read before the live preview is cancelled, which forgets
    // them: they are the final transcript's prefix.
    d->has_assembly = !SDL_strcmp(gs_jval_string(gs_jval_path(d->cfg, "stt.engine"), ""), "builtin");
    if (d->has_assembly) d->assembly = eh_preview_snapshot_final();
    // The final pass is authoritative; partial work must not contend with it on the workers.
    eh_preview_cancel();
    set_state(EH_PROCESSING);
    eh_signal_unref(P.signal);
    P.signal = eh_signal_new();
    d->signal = eh_signal_ref(P.signal);
    const gs_jval *cleanup = gs_jval_get(d->cfg, "cleanup");
    if (gs_jval_boolean(gs_jval_get(cleanup, "enabled"), false) && !SDL_strcmp(gs_jval_string(gs_jval_get(cleanup, "engine"), ""), "builtin")) {
        // A live-preview clean still generating would make the final clean queue behind it.
        eh_cancel_clean();
        // The committed text is a known prefix of the final transcript, so its prompt can be
        // evaluated on the cleanup worker while the tail decodes on the speech worker.
        if (d->has_assembly && !d->assembly.broken && *d->assembly.committed_raw) eh_prime_cleanup(cleanup, d->assembly.committed_raw);
    }
    P.deps.status("transcribing", NULL);
    eh_spawn(process_task, d);
}

void eh_pipeline_partial(unsigned sid, int seq, bool final, int from_sample, int has_speech, uint8_t *wav, size_t len) {
    eh_preview_audio(sid, seq, final, from_sample, has_speech, wav, len);
}

void eh_pipeline_record_cancelled(unsigned sid) {
    if (sid != P.session) return;
    eh_preview_cancel();
    if (P.state == EH_RECORDING) set_state(EH_IDLE);
    P.deps.overlay_show(false);
}

void eh_pipeline_record_error(unsigned sid, const char *message) {
    if (sid != P.session) return;
    eh_preview_cancel();
    if (P.state == EH_RECORDING) set_state(EH_IDLE);
    error_status(message);
    hide_overlay_soon(sid, 5000);
}

void eh_pipeline_overlay_dismiss(void) { P.deps.overlay_show(false); }

/* ---------------- the keys ---------------- */

typedef struct { char model[128]; } warm;

static void warm_stt(void *user) {
    warm *w = user;
    char error[256];
    eh_ensure_stt(w->model, error, sizeof error);  // best effort: the final pass loads again and reports
    SDL_free(w);
}

static void start_recording(void) {
    const gs_jval *cfg = eh_settings(), *stt = gs_jval_get(cfg, "stt"), *cleanup = gs_jval_get(cfg, "cleanup");
    // A built-in model that is not on disk cannot transcribe this dictation: said now, before the
    // microphone opens, and not after the user has spoken. Asked on every press, so finishing the
    // download or switching engines works at once.
    eh_stt_readiness ready = eh_stt_ready(stt);
    if (!ready.ok) {
        SDL_Log("not recording: speech model %s is %s", ready.model_id, ready.unknown ? "unknown" : "missing");
        char body[320];
        if (ready.unknown) SDL_strlcpy(body, "The selected speech model isn't available. Choose one in Settings → Speech-to-text. Click to open Settings.", sizeof body);
        else SDL_snprintf(body, sizeof body, "Download “%s” in Settings → Speech-to-text before dictating. Click to open Settings.", ready.label);
        P.deps.notify(ready.unknown ? "Earheart: no speech model available" : "Earheart: speech model not downloaded", body, true);
        return;
    }
    unsigned sid = ++P.session;
    set_state(EH_RECORDING);
    bool builtin = !SDL_strcmp(gs_jval_string(gs_jval_get(stt, "engine"), ""), "builtin");
    bool live_on = builtin && gs_jval_boolean(gs_jval_path(stt, "livePreview.enabled"), false);
    // The models are warmed as recording begins, so their load time hides under the time the user
    // spends speaking. With live preview on this also keeps the first partials from all being
    // dropped while the model loads. Priming prefills the static start of the cleanup prompt.
    if (builtin) {
        warm *w = SDL_calloc(1, sizeof *w);
        SDL_strlcpy(w->model, gs_jval_string(gs_jval_path(stt, "builtin.model"), ""), sizeof w->model);
        eh_spawn(warm_stt, w);
    }
    if (gs_jval_boolean(gs_jval_get(cleanup, "enabled"), false) && !SDL_strcmp(gs_jval_string(gs_jval_get(cleanup, "engine"), ""), "builtin")) eh_prime_cleanup(cleanup, "");
    P.deps.overlay_show(true);
    // Chunked partial decoding runs whenever speech is built in, since the committed chunks become
    // the final transcript's prefix; `display` also paints the live transcript and is the user's
    // toggle. A remote service gets neither: it would be hammered with repeated uploads.
    P.deps.record_start(sid, gs_jval_string(gs_jval_path(cfg, "audio.deviceId"), ""), gs_jval_number(gs_jval_path(cfg, "audio.maxRecordingSeconds"), 600),
                        builtin, live_on, gs_jval_get(stt, "livePreview"));
}

void eh_pipeline_toggle(void) {
    if (P.state == EH_IDLE) start_recording();
    else if (P.state == EH_RECORDING) P.deps.record_stop();
    // While processing the hotkey is ignored; cancel is on the overlay and in the tray menu.
}

void eh_pipeline_pause_toggle(void) {
    if (P.state == EH_RECORDING) P.deps.record_pause_toggle();
}

void eh_pipeline_discard(void) {
    if (P.state != EH_IDLE) P.deps.record_discard();
}

void eh_pipeline_cancel(void) {
    P.session++;  // events of the session in flight are now stale
    eh_preview_cancel();
    if (P.state == EH_RECORDING) {
        P.deps.record_cancel();
    } else if (P.state == EH_PROCESSING && P.signal) {
        // The abort only mutes the reply; the cleanup worker would keep generating for nothing
        // and delay the next dictation's clean.
        eh_signal_abort(P.signal);
        eh_cancel_clean();
    }
    set_state(EH_IDLE);
    P.deps.overlay_show(false);
}

void eh_pipeline_init(const eh_pipeline_deps *deps, const char *rtf_path) {
    P.deps = *deps;
    SDL_free(P.rtf_path);
    P.rtf_path = SDL_strdup(rtf_path);
    rtf_load();
    static const eh_preview_deps preview = { transcribe_live, clean_live, NULL, recording_session, preview_error };
    eh_preview_deps with_send = preview;
    with_send.send = deps->partial;
    eh_preview_init(&with_send);
}
