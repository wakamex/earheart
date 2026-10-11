#include "engines.h"
#include <SDL3/SDL.h>
#include <math.h>
#include "catalog.h"
#include "cleanup.h"
#include "engine.h"
#include "loop.h"
#include "paths.h"
#include "settings.h"
#include "text.h"

struct eh_signal {
    bool aborted;
    int refs;
    unsigned clean_op;  // the cleanup running under this signal, or 0
};

static struct {
    eh_engine *stt, *cleanup;
    unsigned stt_generation, cleanup_generation;  // the workers these facts were learned from
    char loaded_stt[128];   // the model the speech worker holds, or ""
    char loading_stt[128];  // the model a load in flight is for, or ""
    bool cleanup_resident;  // the cleanup worker may hold a model, even after a failed request
    unsigned next_op;
} E;

void eh_engines_init(void) {
    if (E.stt) return;
    E.stt = eh_engine_new("stt");
    E.cleanup = eh_engine_new("cleanup");
}

// A worker that went away took what it had loaded with it.
static void forget_dead(void) {
    unsigned g = eh_engine_generation(E.stt);
    if (g != E.stt_generation) { E.stt_generation = g; E.loaded_stt[0] = 0; }
    g = eh_engine_generation(E.cleanup);
    if (g != E.cleanup_generation) { E.cleanup_generation = g; E.cleanup_resident = false; }
}

void eh_engines_stop(void) {
    EH_AWAIT(eh_engine_stop(E.stt));
    EH_AWAIT(eh_engine_stop(E.cleanup));
    forget_dead();
}

void eh_engines_restart_stt(void) {
    EH_AWAIT(eh_engine_stop(E.stt));
    forget_dead();
}

bool eh_engines_unload_idle(void) {
    forget_dead();
    bool clear = true;
    if (E.loaded_stt[0] || eh_engine_busy(E.stt)) {
        if (eh_engine_busy(E.stt)) clear = false;
        else EH_AWAIT(eh_engine_stop(E.stt));
    }
    if (E.cleanup_resident || eh_engine_busy(E.cleanup)) {
        if (eh_engine_busy(E.cleanup)) clear = false;
        else EH_AWAIT(eh_engine_stop(E.cleanup));
    }
    forget_dead();
    return clear;
}

/* ---------------- signals ---------------- */

eh_signal *eh_signal_new(void) {
    eh_signal *s = SDL_calloc(1, sizeof *s);
    s->refs = 1;
    return s;
}

eh_signal *eh_signal_ref(eh_signal *s) {
    if (s) s->refs++;
    return s;
}

void eh_signal_unref(eh_signal *s) {
    if (s && !--s->refs) SDL_free(s);
}

bool eh_signal_aborted(const eh_signal *s) { return s && s->aborted; }

typedef struct { unsigned op; } cancel_request;

static void cancel_task(void *user) {
    cancel_request *c = user;
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", "cancel-clean");
    if (c->op) eh_jw_num(&w, "operationId", c->op);
    SDL_free(c);
    eh_engine_reply reply;
    unsigned place = eh_engine_reserve(E.cleanup);
    EH_AWAIT(eh_engine_call_at(E.cleanup, place, &w, NULL, 0, 0, NULL, NULL, &reply));
    eh_engine_reply_free(&reply);
}

static void cancel_clean(unsigned op) {
    forget_dead();
    if (!E.cleanup_resident) return;
    cancel_request *c = SDL_malloc(sizeof *c);
    c->op = op;
    eh_spawn(cancel_task, c);
}

void eh_cancel_clean(void) { cancel_clean(0); }

void eh_signal_abort(eh_signal *s) {
    if (!s || s->aborted) return;
    s->aborted = true;
    // The abort only mutes the reply; the worker would keep generating for nothing.
    if (s->clean_op) cancel_clean(s->clean_op);
}

/* ---------------- speech-to-text ---------------- */

eh_stt_readiness eh_stt_ready(const eh_jv *stt) {
    eh_stt_readiness r = { .ok = true };
    if (SDL_strcmp(eh_jv_string(eh_jv_get(stt, "engine"), ""), "builtin")) return r;
    const char *id = eh_jv_string(eh_jv_path(stt, "builtin.model"), "");
    SDL_strlcpy(r.model_id, id, sizeof r.model_id);
    const eh_model *m = eh_catalog_find(EH_MODEL_STT, id);
    if (!m) { r.ok = false; r.unknown = true; return r; }
    SDL_strlcpy(r.label, m->label ? m->label : id, sizeof r.label);
    r.ok = eh_model_installed(m, NULL, 0);
    return r;
}

static eh_engine_status call(eh_engine *e, eh_jw *w, const void *blob, size_t len, eh_engine_progress *progress, void *user, eh_engine_reply *reply) {
    eh_engine_status s;
    // The place is taken with the lock held, so requests go out in the order tasks made them.
    unsigned place = eh_engine_reserve(e);
    EH_AWAIT(s = eh_engine_call_at(e, place, w, blob, len, 0, progress, user, reply));
    forget_dead();
    return s;
}

bool eh_ensure_stt(const char *model_id, char *error, size_t error_size) {
    const eh_model *m = eh_catalog_find(EH_MODEL_STT, model_id);
    char dir[1024];
    if (!m) { SDL_snprintf(error, error_size, "Unknown stt model: %s", model_id); return false; }
    if (!eh_model_installed(m, dir, sizeof dir)) { SDL_snprintf(error, error_size, "STT model \"%s\" is not downloaded yet", model_id); return false; }
    forget_dead();
    // Concurrent callers share one load: a warm-up at record start, the first live-preview
    // decodes and the final pass all ask for the same model.
    while (!SDL_strcmp(E.loading_stt, model_id)) {
        EH_AWAIT(SDL_Delay(5));
        forget_dead();
    }
    if (!SDL_strcmp(E.loaded_stt, model_id)) return true;
    SDL_strlcpy(E.loading_stt, model_id, sizeof E.loading_stt);
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", "load-stt");
    eh_jw_str(&w, "modelId", m->id);
    eh_jw_str(&w, "dir", dir);
    eh_jw_str(&w, "encoder", m->sherpa.encoder);
    eh_jw_str(&w, "decoder", m->sherpa.decoder);
    eh_jw_str(&w, "joiner", m->sherpa.joiner);
    eh_jw_str(&w, "tokens", m->sherpa.tokens);
    eh_jw_str(&w, "modelType", m->sherpa.model_type);
    eh_engine_reply reply;
    eh_engine_status s = call(E.stt, &w, NULL, 0, NULL, NULL, &reply);
    if (!SDL_strcmp(E.loading_stt, model_id)) E.loading_stt[0] = 0;
    if (s == EH_ENGINE_OK) {
        E.stt_generation = eh_engine_generation(E.stt);
        SDL_strlcpy(E.loaded_stt, model_id, sizeof E.loaded_stt);
    } else {
        SDL_strlcpy(error, reply.error, error_size);
    }
    eh_engine_reply_free(&reply);
    return s == EH_ENGINE_OK;
}

eh_decode_code eh_transcribe(const eh_jv *stt, const eh_signal *signal, const uint8_t *wav, size_t len, char **text, int *decode_ms, char *error, size_t error_size) {
    if (eh_signal_aborted(signal)) { SDL_strlcpy(error, "aborted", error_size); return EH_DECODE_FAILED; }
    if (SDL_strcmp(eh_jv_string(eh_jv_get(stt, "engine"), ""), "builtin")) {
        SDL_strlcpy(error, "Remote speech-to-text is not part of this build yet", error_size);
        return EH_DECODE_FAILED;
    }
    if (!eh_ensure_stt(eh_jv_string(eh_jv_path(stt, "builtin.model"), ""), error, error_size)) return EH_DECODE_FAILED;
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", "transcribe");
    eh_engine_reply reply;
    eh_engine_status s = call(E.stt, &w, wav, len, NULL, NULL, &reply);
    eh_decode_code code = s == EH_ENGINE_OK ? EH_DECODE_OK : s == EH_ENGINE_EXITED ? EH_DECODE_EXITED : s == EH_ENGINE_TIMEOUT ? EH_DECODE_TIMEOUT : EH_DECODE_FAILED;
    if (code == EH_DECODE_OK) {
        *text = eh_trim(gs_json_str(gs_json_get(reply.v, "text"), ""));
        gs_jv ms = gs_json_get(reply.v, "decodeMs");
        if (ms && decode_ms) *decode_ms = (int)gs_json_num(ms, -1);
    } else {
        SDL_strlcpy(error, reply.error, error_size);
    }
    eh_engine_reply_free(&reply);
    return code;
}

/* ---------------- cleanup ---------------- */

int eh_clean_context_for(double max_recording_seconds) {
    // Speech runs about 2.5 words a second at 1.35 tokens a word, and the prompt about 700 tokens;
    // the turn holds the transcript once and the reply holds it again.
    double seconds = isfinite(max_recording_seconds) ? SDL_max(0, max_recording_seconds) : 0;
    int transcript = (int)ceil(seconds * 2.5 * 1.35), needed = 700 + transcript + transcript + EH_CLEAN_SLACK_TOKENS, size = EH_CLEAN_CONTEXT_MIN;
    while (size < needed && size < EH_CLEAN_CONTEXT_MAX) size *= 2;
    return size;
}

// The request fields that name the model, its context and where it may run. False with `error`
// set when the model is not there.
static bool cleanup_model(eh_jw *w, const char *model_id, char *error, size_t error_size) {
    const eh_model *m = eh_catalog_find(EH_MODEL_CLEANUP, model_id);
    char dir[1024], path[1200];
    if (!m) { SDL_snprintf(error, error_size, "Unknown cleanup model: %s", model_id); return false; }
    if (!eh_model_installed(m, dir, sizeof dir)) { SDL_snprintf(error, error_size, "Cleanup model \"%s\" is not downloaded yet", model_id); return false; }
    SDL_snprintf(path, sizeof path, "%s" EH_SEP "%s", dir, m->gguf);
    eh_jw_str(w, "modelPath", path);
    eh_jw_num(w, "contextSize", eh_clean_context_for(eh_setting_num("audio.maxRecordingSeconds")));
    return true;
}

typedef struct { char *model, *text; } prime_request;

static void prime_task(void *user) {
    prime_request *p = user;
    char error[256];
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", "prime-cleanup");
    eh_jw_str(&w, "text", p->text);
    if (cleanup_model(&w, p->model, error, sizeof error)) {
        E.cleanup_resident = true;
        eh_engine_reply reply;
        call(E.cleanup, &w, NULL, 0, NULL, NULL, &reply);
        eh_engine_reply_free(&reply);
    } else {
        SDL_free(eh_jw_end(&w));
    }
    SDL_free(p->model);
    SDL_free(p->text);
    SDL_free(p);
}

void eh_prime_cleanup(const eh_jv *cleanup, const char *transcript_prefix) {
    eh_cleanup_plan plan = eh_resolve_cleanup(cleanup);
    prime_request *p = SDL_malloc(sizeof *p);
    p->model = SDL_strdup(eh_jv_string(eh_jv_path(cleanup, "builtin.model"), ""));
    p->text = eh_cleanup_turn_prefix(plan.system_prompt, transcript_prefix ? transcript_prefix : "");
    SDL_free(plan.system_prompt);
    eh_spawn(prime_task, p);
}

char *eh_clean(const char *transcript, const eh_jv *cleanup, eh_signal *signal, eh_fraction_fn *progress, void *user, char *error, size_t error_size) {
    if (eh_signal_aborted(signal)) { SDL_strlcpy(error, "aborted", error_size); return NULL; }
    if (SDL_strcmp(eh_jv_string(eh_jv_get(cleanup, "engine"), ""), "builtin")) {
        SDL_strlcpy(error, "Remote cleanup is not part of this build yet", error_size);
        return NULL;
    }
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", "clean");
    if (!cleanup_model(&w, eh_jv_string(eh_jv_path(cleanup, "builtin.model"), ""), error, error_size)) {
        SDL_free(eh_jw_end(&w));
        return NULL;
    }
    eh_cleanup_plan plan = eh_resolve_cleanup(cleanup);
    unsigned op = ++E.next_op;
    eh_jw_num(&w, "operationId", op);
    eh_jw_str(&w, "transcript", transcript);
    eh_jw_str(&w, "systemPrompt", plan.system_prompt);
    if (plan.has_temperature) eh_jw_num(&w, "temperature", plan.sampling.temperature);
    if (plan.has_top_p) eh_jw_num(&w, "topP", plan.sampling.top_p);
    if (plan.sampling.top_k > 0) eh_jw_num(&w, "topK", plan.sampling.top_k);
    if (plan.sampling.min_p > 0) eh_jw_num(&w, "minP", plan.sampling.min_p);
    SDL_free(plan.system_prompt);

    E.cleanup_resident = true;
    if (signal) { eh_signal_ref(signal); signal->clean_op = op; }
    eh_engine_reply reply;
    eh_engine_status s = call(E.cleanup, &w, NULL, 0, progress, user, &reply);
    char *out = NULL;
    if (signal && signal->clean_op == op) signal->clean_op = 0;
    if (eh_signal_aborted(signal)) {
        SDL_strlcpy(error, "aborted", error_size);
    } else if (s != EH_ENGINE_OK) {
        SDL_strlcpy(error, reply.error, error_size);
    } else {
        char *cleaned = eh_trim(gs_json_str(gs_json_get(reply.v, "text"), ""));
        if (!*cleaned) {
            SDL_strlcpy(error, "Cleanup returned no usable text", error_size);
            SDL_free(cleaned);
        } else {
            // The safety net behind the model, for the styles that promise tidied text.
            const char *style = eh_jv_string(eh_jv_get(cleanup, "style"), "");
            bool tidied = !SDL_strcmp(style, "clean") || !SDL_strcmp(style, "polished");
            out = tidied ? eh_strip_stumbles(cleaned) : SDL_strdup(cleaned);
            SDL_free(cleaned);
        }
    }
    eh_engine_reply_free(&reply);
    eh_signal_unref(signal);
    return out;
}
