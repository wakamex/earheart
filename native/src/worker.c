// An engine worker: `earheart --engine stt` or `earheart --engine cleanup`. See engine.h for the
// messages. The speech worker answers one request at a time. The cleanup worker reads requests on
// a thread of its own, so that "cancel-clean" reaches a generation in progress and the requests
// queued behind it.
#include <stdio.h>
#include <stdlib.h>
#include "catalog.h"
#include "cleanup.h"
#include "engine.h"
#include "paths.h"
#include "stt.h"
#include "wav.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

typedef struct request {
    gs_json *doc;
    gs_jv v;
    unsigned id;
    Uint8 *blob;
    size_t blob_len;
    SDL_AtomicInt cancel;
    struct request *next;
} request;

static SDL_Mutex *out_lock;

static void send_json(eh_jw *w) {
    char *json = eh_jw_end(w);
    Uint32 len = (Uint32)SDL_strlen(json);
    Uint8 head[8] = { (Uint8)len, (Uint8)(len >> 8), (Uint8)(len >> 16), (Uint8)(len >> 24) };
    SDL_LockMutex(out_lock);
    fwrite(head, 1, 8, stdout);
    fwrite(json, 1, len, stdout);
    fflush(stdout);
    SDL_UnlockMutex(out_lock);
    SDL_free(json);
}

static void send_error(unsigned id, const char *error) {
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_num(&w, "id", id);
    eh_jw_bool(&w, "ok", false);
    eh_jw_str(&w, "error", error);
    send_json(&w);
}

static eh_jw ok_reply(unsigned id) {
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_num(&w, "id", id);
    eh_jw_bool(&w, "ok", true);
    return w;
}

static request *read_request(void) {
    Uint8 head[8];
    if (fread(head, 1, 8, stdin) != 8) return NULL;
    Uint32 json_len = head[0] | head[1] << 8 | head[2] << 16 | (Uint32)head[3] << 24;
    Uint32 blob_len = head[4] | head[5] << 8 | head[6] << 16 | (Uint32)head[7] << 24;
    char *json = SDL_malloc(json_len + 1);
    request *r = SDL_calloc(1, sizeof *r);
    r->blob = blob_len ? SDL_malloc(blob_len) : NULL;
    r->blob_len = blob_len;
    bool ok = fread(json, 1, json_len, stdin) == json_len && (!blob_len || fread(r->blob, 1, blob_len, stdin) == blob_len);
    r->doc = ok ? gs_json_parse(json, json_len) : NULL;
    SDL_free(json);
    if (!r->doc) { SDL_free(r->blob); SDL_free(r); return NULL; }
    r->v = gs_json_root(r->doc);
    r->id = (unsigned)gs_json_num(gs_json_get(r->v, "id"), 0);
    return r;
}

static void free_request(request *r) {
    gs_json_free(r->doc);
    SDL_free(r->blob);
    SDL_free(r);
}

static const char *str(request *r, const char *key) { return gs_json_str(gs_json_get(r->v, key), NULL); }
static bool is(request *r, const char *type) { return !SDL_strcmp(gs_json_str(gs_json_get(r->v, "type"), ""), type); }

/* ---------------- speech-to-text ---------------- */

static char *stt_model;  // the id of the model loaded

// "exit" ends the worker at once, as a crash in an engine's native code would: for tests.
static void serve_exit(request *r) {
    if (is(r, "exit")) _Exit(3);  // not exit(), which waits for the lock a thread reading standard input holds
}

static void stt_serve(request *r) {
    serve_exit(r);
    if (is(r, "ping")) {
        eh_jw w = ok_reply(r->id);
        eh_jw_bool(&w, "pong", true);
        send_json(&w);
    } else if (is(r, "load-stt")) {
        const char *id = str(r, "modelId");
        if (!stt_model || !id || SDL_strcmp(stt_model, id)) {
            eh_stt_files f = { str(r, "encoder"), str(r, "decoder"), str(r, "joiner"), str(r, "tokens"), str(r, "modelType") };
            SDL_free(stt_model);
            stt_model = NULL;
            if (!f.encoder || !f.decoder || !f.tokens || !str(r, "dir") || !eh_stt_load(eh_engines_dir(), str(r, "dir"), &f)) {
                send_error(r->id, f.encoder && f.decoder && f.tokens && str(r, "dir") ? eh_stt_error() : "load-stt needs dir, encoder, decoder and tokens");
                return;
            }
            stt_model = SDL_strdup(id ? id : "");
        }
        eh_jw w = ok_reply(r->id);
        eh_jw_bool(&w, "ready", true);
        eh_jw_num(&w, "numThreads", eh_stt_threads());
        eh_jw_str(&w, "provider", "cpu");
        send_json(&w);
    } else if (is(r, "transcribe")) {
        const char *error = NULL;
        int frames = 0, rate = 0, ms = 0;
        float *samples = r->blob ? eh_wav_floats(r->blob, r->blob_len, &frames, &rate, &error) : NULL;
        if (!r->blob) error = "transcribe needs a WAV";
        char *text = samples ? eh_stt_transcribe(samples, frames, rate, &ms) : NULL;
        if (text) {
            eh_jw w = ok_reply(r->id);
            eh_jw_str(&w, "text", text);
            eh_jw_num(&w, "decodeMs", ms);
            send_json(&w);
        } else {
            send_error(r->id, error ? error : eh_stt_error());
        }
        SDL_free(text);
        SDL_free(samples);
    } else {
        send_error(r->id, "Unknown request");
    }
}

/* ---------------- cleanup ---------------- */

static struct {
    SDL_Mutex *lock;
    SDL_Condition *ready;
    request *head, *tail, *current;
    bool closed;
} Q;

static bool matches(request *r, gs_jv operation) {
    if (!operation) return true;
    gs_jv mine = gs_json_get(r->v, "operationId");
    if (gs_json_kind(mine) != gs_json_kind(operation)) return false;
    return gs_json_kind(mine) == GS_JSON_STRING ? !SDL_strcmp(gs_json_str(mine, ""), gs_json_str(operation, "")) : gs_json_num(mine, 0) == gs_json_num(operation, 0);
}

static bool cancel_if(request *q, gs_jv operation) {
    if (!q || !(is(q, "clean") || is(q, "prime-cleanup")) || !matches(q, operation)) return false;
    SDL_SetAtomicInt(&q->cancel, 1);
    return true;
}

// Runs on the reading thread: marks the running operation and the queued ones, all of them or
// those of one operationId.
static void cancel_clean(request *r) {
    gs_jv operation = gs_json_get(r->v, "operationId");
    int cancelled = 0;
    SDL_LockMutex(Q.lock);
    cancelled += cancel_if(Q.current, operation);
    for (request *q = Q.head; q; q = q->next) cancelled += cancel_if(q, operation);
    SDL_UnlockMutex(Q.lock);
    eh_jw w = ok_reply(r->id);
    eh_jw_num(&w, "cancelled", cancelled);
    send_json(&w);
}

static int cleanup_reader(void *user) {
    for (request *r; (r = read_request());) {
        if (is(r, "cancel-clean")) { cancel_clean(r); free_request(r); continue; }
        SDL_LockMutex(Q.lock);
        if (Q.tail) Q.tail->next = r; else Q.head = r;
        Q.tail = r;
        SDL_SignalCondition(Q.ready);
        SDL_UnlockMutex(Q.lock);
    }
    SDL_LockMutex(Q.lock);
    Q.closed = true;
    SDL_SignalCondition(Q.ready);
    SDL_UnlockMutex(Q.lock);
    return 0;
}

typedef struct { unsigned id; Uint64 last; } progress_state;

// At most ten a second, so a callback per token cannot flood the pipe; the reply is the last word.
static void clean_progress(void *user, float fraction) {
    progress_state *p = user;
    Uint64 now = SDL_GetTicks();
    if (now - p->last < 100) return;
    p->last = now;
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_num(&w, "id", p->id);
    eh_jw_num(&w, "progress", fraction);
    send_json(&w);
}

static bool load_for(request *r) {
    const char *path = str(r, "modelPath");
    if (!path) { send_error(r->id, "The request names no cleanup model"); return false; }
    int context = (int)gs_json_num(gs_json_get(r->v, "contextSize"), 0);
    if (eh_cleanup_load(eh_engines_dir(), path, context, gs_json_bool(gs_json_get(r->v, "cpuOnly"), false))) return true;
    send_error(r->id, eh_cleanup_error());
    return false;
}

static void cleanup_serve(request *r) {
    serve_exit(r);
    if (is(r, "ping")) {
        eh_jw w = ok_reply(r->id);
        eh_jw_bool(&w, "pong", true);
        send_json(&w);
    } else if (is(r, "load-cleanup")) {
        if (!load_for(r)) return;
        eh_jw w = ok_reply(r->id);
        eh_jw_bool(&w, "ready", true);
        eh_jw_str(&w, "device", eh_cleanup_device());
        send_json(&w);
    } else if (is(r, "clean") || is(r, "prime-cleanup")) {
        // Each carries its own model, so overlapping callers cannot generate on another's.
        if (SDL_GetAtomicInt(&r->cancel)) { send_error(r->id, "cleanup cancelled"); return; }
        if (!load_for(r)) return;
        if (is(r, "prime-cleanup")) {
            if (!eh_cleanup_prime(gs_json_str(gs_json_get(r->v, "text"), ""), &r->cancel)) { send_error(r->id, eh_cleanup_error()); return; }
            eh_jw w = ok_reply(r->id);
            eh_jw_bool(&w, "primed", true);
            send_json(&w);
            return;
        }
        eh_sampling s = {
            .temperature = (float)gs_json_num(gs_json_get(r->v, "temperature"), -1),
            .top_p = (float)gs_json_num(gs_json_get(r->v, "topP"), -1),
            .top_k = (int)gs_json_num(gs_json_get(r->v, "topK"), 0),
            .min_p = (float)gs_json_num(gs_json_get(r->v, "minP"), 0),
            .seed = (unsigned)gs_json_num(gs_json_get(r->v, "seed"), 0),
        };
        if (gs_json_bool(gs_json_get(r->v, "fresh"), false)) eh_cleanup_forget();
        progress_state p = { r->id, 0 };
        char *text = eh_cleanup_clean(gs_json_str(gs_json_get(r->v, "systemPrompt"), ""), gs_json_str(gs_json_get(r->v, "transcript"), ""),
                                      &s, clean_progress, &p, &r->cancel);
        if (!text) { send_error(r->id, eh_cleanup_error()); return; }
        eh_jw w = ok_reply(r->id);
        eh_jw_str(&w, "text", text);
        send_json(&w);
        SDL_free(text);
    } else {
        send_error(r->id, "Unknown request");
    }
}

int eh_worker_main(const char *kind) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    out_lock = SDL_CreateMutex();
    if (!SDL_strcmp(kind, "stt")) {
        for (request *r; (r = read_request()); free_request(r)) stt_serve(r);
        return 0;
    }
    if (SDL_strcmp(kind, "cleanup")) return 2;
    Q.lock = SDL_CreateMutex();
    Q.ready = SDL_CreateCondition();
    SDL_DetachThread(SDL_CreateThread(cleanup_reader, "requests", NULL));
    for (;;) {
        SDL_LockMutex(Q.lock);
        while (!Q.head && !Q.closed) SDL_WaitCondition(Q.ready, Q.lock);
        request *r = Q.current = Q.head;
        if (r && !(Q.head = r->next)) Q.tail = NULL;
        if (r) r->next = NULL;
        SDL_UnlockMutex(Q.lock);
        if (!r) return 0;
        cleanup_serve(r);
        SDL_LockMutex(Q.lock);
        Q.current = NULL;
        SDL_UnlockMutex(Q.lock);
        free_request(r);
    }
}
