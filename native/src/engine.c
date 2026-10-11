#include "engine.h"
#include "gs_http.h"
#include "paths.h"

typedef struct call {
    unsigned id;
    bool done;
    int reporting;      // progress callbacks running now; the caller does not return under them
    Uint64 last_heard;  // when it was sent or last reported progress
    eh_engine_progress *progress;
    void *user;
    eh_engine_reply *reply;
    struct call *next;
} call;

struct eh_engine {
    char kind[16];
    SDL_Process *proc;
    SDL_IOStream *in, *out;
    SDL_Thread *reader;
    SDL_Mutex *lock;        // the calls and the worker's state
    SDL_Mutex *write_lock;  // one request on the pipe at a time
    SDL_Condition *answered;
    SDL_Semaphore *wake;    // posted once per request sent, and once to end the reader
    SDL_AtomicInt quit, gone;  // gone: the reader saw the pipe close
    call *calls;            // sent and not yet answered
    unsigned next_id, generation;
    SDL_Mutex *order_lock;  // places in the queue to send
    SDL_Condition *order_changed;
    unsigned next_place, sending;
};

static void put_le32(Uint8 *p, Uint32 v) { p[0] = (Uint8)v; p[1] = (Uint8)(v >> 8); p[2] = (Uint8)(v >> 16); p[3] = (Uint8)(v >> 24); }
static Uint32 get_le32(const Uint8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (Uint32)p[3] << 24; }

void eh_engine_reply_free(eh_engine_reply *r) {
    gs_json_free(r->doc);
    r->doc = NULL;
    r->v = NULL;
}

// Reads exactly n bytes of the worker's output, which SDL hands over without waiting. False when
// the worker has closed it or the engine is being stopped.
static bool read_all(eh_engine *e, void *buf, size_t n) {
    for (size_t got = 0; got < n;) {
        size_t r = SDL_ReadIO(e->out, (Uint8 *)buf + got, n - got);
        got += r;
        if (r) continue;
        if (SDL_GetIOStatus(e->out) != SDL_IO_STATUS_NOT_READY || SDL_GetAtomicInt(&e->quit)) return false;
        SDL_Delay(2);
    }
    return true;
}

// Writes all n bytes to the worker's input, which takes only what its pipe has room for.
static bool write_all(eh_engine *e, const void *buf, size_t n) {
    for (size_t put = 0; put < n;) {
        size_t w = SDL_WriteIO(e->in, (const Uint8 *)buf + put, n - put);
        put += w;
        if (w) continue;
        if (SDL_GetIOStatus(e->in) != SDL_IO_STATUS_NOT_READY) return false;
        SDL_Delay(1);
    }
    return true;
}

// With the lock held: every waiting call learns the worker is gone.
static void fail_calls(eh_engine *e) {
    for (call *c = e->calls; c; c = c->next) {
        if (c->done) continue;
        c->done = true;
        c->reply->status = EH_ENGINE_EXITED;
        SDL_snprintf(c->reply->error, sizeof c->reply->error, "The %s engine stopped", e->kind);
    }
    e->generation++;
    SDL_BroadcastCondition(e->answered);
}

static int reader_main(void *user) {
    eh_engine *e = user;
    for (;;) {
        // Sleeps while nothing is owed: the worker speaks only when spoken to.
        SDL_WaitSemaphore(e->wake);
        if (SDL_GetAtomicInt(&e->quit)) return 0;
        for (bool answered = false; !answered;) {
            Uint8 head[8];
            char *json = NULL;
            bool ok = read_all(e, head, sizeof head);
            Uint32 json_len = ok ? get_le32(head) : 0, blob_len = ok ? get_le32(head + 4) : 0;
            ok = ok && json_len < (1u << 26) && blob_len == 0 && (json = SDL_malloc(json_len + 1)) && read_all(e, json, json_len);
            gs_json *doc = ok ? gs_json_parse(json, json_len) : NULL;
            SDL_free(json);
            SDL_LockMutex(e->lock);
            if (!doc) {
                // The pipe closes a moment before the process reads as exited: the worker is
                // marked gone here, so the next call starts another instead of writing to this one.
                SDL_SetAtomicInt(&e->gone, 1);
                fail_calls(e);
                SDL_UnlockMutex(e->lock);
                return 0;
            }
            gs_jv v = gs_json_root(doc);
            unsigned id = (unsigned)gs_json_num(gs_json_get(v, "id"), 0);
            gs_jv progress = gs_json_get(v, "progress");
            call *c = e->calls;
            while (c && (c->id != id || c->done)) c = c->next;
            if (progress) {
                // Proof the worker is alive: the caller's deadline starts again.
                if (c && gs_json_kind(progress) == GS_JSON_NUMBER) {
                    c->last_heard = SDL_GetTicks();
                    if (c->progress) {
                        // Called without the lock, so the callback may take locks of its own.
                        c->reporting++;
                        SDL_UnlockMutex(e->lock);
                        c->progress(c->user, (float)gs_json_num(progress, 0));
                        SDL_LockMutex(e->lock);
                        c->reporting--;
                        SDL_BroadcastCondition(e->answered);
                    }
                }
                gs_json_free(doc);
            } else {
                answered = true;
                if (c) {
                    c->done = true;
                    c->reply->doc = doc;
                    c->reply->v = v;
                    bool good = gs_json_bool(gs_json_get(v, "ok"), false);
                    c->reply->status = good ? EH_ENGINE_OK : EH_ENGINE_FAILED;
                    if (!good) SDL_strlcpy(c->reply->error, gs_json_str(gs_json_get(v, "error"), "engine error"), sizeof c->reply->error);
                    SDL_BroadcastCondition(e->answered);
                } else {
                    gs_json_free(doc);  // an answer nobody waits for any more
                }
            }
            SDL_UnlockMutex(e->lock);
        }
    }
}

eh_engine *eh_engine_new(const char *kind) {
    eh_engine *e = SDL_calloc(1, sizeof *e);
    SDL_strlcpy(e->kind, kind, sizeof e->kind);
    e->lock = SDL_CreateMutex();
    e->write_lock = SDL_CreateMutex();
    e->answered = SDL_CreateCondition();
    e->wake = SDL_CreateSemaphore(0);
    e->order_lock = SDL_CreateMutex();
    e->order_changed = SDL_CreateCondition();
    return e;
}

static bool running(eh_engine *e) {
    int code;
    return e->proc && !SDL_GetAtomicInt(&e->gone) && !SDL_WaitProcess(e->proc, false, &code);
}

bool eh_engine_running(eh_engine *e) {
    SDL_LockMutex(e->write_lock);
    bool r = running(e);
    SDL_UnlockMutex(e->write_lock);
    return r;
}

bool eh_engine_busy(eh_engine *e) {
    SDL_LockMutex(e->lock);
    bool busy = false;
    for (call *c = e->calls; c && !busy; c = c->next) busy = !c->done;
    SDL_UnlockMutex(e->lock);
    return busy;
}

unsigned eh_engine_generation(eh_engine *e) {
    SDL_LockMutex(e->lock);
    unsigned g = e->generation;
    SDL_UnlockMutex(e->lock);
    return g;
}

// With the write lock held.
static void stop(eh_engine *e) {
    if (!e->proc) return;
    SDL_SetAtomicInt(&e->quit, 1);
    SDL_KillProcess(e->proc, true);
    SDL_SignalSemaphore(e->wake);
    SDL_WaitThread(e->reader, NULL);
    SDL_DestroyProcess(e->proc);
    e->proc = NULL;
    e->reader = NULL;
    SDL_SetAtomicInt(&e->quit, 0);
    SDL_SetAtomicInt(&e->gone, 0);
    while (SDL_TryWaitSemaphore(e->wake)) {}
    SDL_LockMutex(e->lock);
    fail_calls(e);
    SDL_UnlockMutex(e->lock);
}

void eh_engine_stop(eh_engine *e) {
    SDL_LockMutex(e->write_lock);
    stop(e);
    SDL_UnlockMutex(e->write_lock);
}

void eh_engine_free(eh_engine *e) {
    if (!e) return;
    eh_engine_stop(e);
    SDL_DestroyMutex(e->lock);
    SDL_DestroyMutex(e->write_lock);
    SDL_DestroyCondition(e->answered);
    SDL_DestroySemaphore(e->wake);
    SDL_DestroyMutex(e->order_lock);
    SDL_DestroyCondition(e->order_changed);
    SDL_free(e);
}

// With the write lock held.
static bool start(eh_engine *e) {
    if (running(e)) return true;
    stop(e);  // a worker that died: collect it before starting another
    const char *args[] = { eh_exe_path(), "--engine", e->kind, NULL };
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    e->proc = gs_http_spawn(p);
    SDL_DestroyProperties(p);
    if (!e->proc) return false;
    e->in = SDL_GetProcessInput(e->proc);
    e->out = SDL_GetProcessOutput(e->proc);
    e->reader = SDL_CreateThread(reader_main, "engine reader", e);
    return true;
}

unsigned eh_engine_reserve(eh_engine *e) {
    SDL_LockMutex(e->order_lock);
    unsigned place = e->next_place++;
    SDL_UnlockMutex(e->order_lock);
    return place;
}

eh_engine_status eh_engine_call(eh_engine *e, eh_jw *request, const void *blob, size_t blob_len, int timeout_ms,
                                eh_engine_progress *progress, void *user, eh_engine_reply *reply) {
    return eh_engine_call_at(e, eh_engine_reserve(e), request, blob, blob_len, timeout_ms, progress, user, reply);
}

eh_engine_status eh_engine_call_at(eh_engine *e, unsigned place, eh_jw *request, const void *blob, size_t blob_len, int timeout_ms,
                                   eh_engine_progress *progress, void *user, eh_engine_reply *reply) {
    *reply = (eh_engine_reply){ .status = EH_ENGINE_EXITED };
    SDL_LockMutex(e->order_lock);
    while (e->sending != place) SDL_WaitCondition(e->order_changed, e->order_lock);
    SDL_UnlockMutex(e->order_lock);
    if (timeout_ms <= 0) timeout_ms = EH_ENGINE_TIMEOUT_MS;
    call c = { .progress = progress, .user = user, .reply = reply };

    // Started before the call is listed: collecting a dead worker answers everything listed.
    SDL_LockMutex(e->write_lock);
    bool ok = start(e);
    SDL_LockMutex(e->lock);
    c.id = ++e->next_id;
    c.last_heard = SDL_GetTicks();
    c.next = e->calls;
    e->calls = &c;
    SDL_UnlockMutex(e->lock);
    eh_jw_num(request, "id", c.id);
    char *json = eh_jw_end(request);
    Uint8 head[8];
    put_le32(head, (Uint32)SDL_strlen(json));
    put_le32(head + 4, (Uint32)blob_len);
    ok = ok && write_all(e, head, 8) && write_all(e, json, SDL_strlen(json)) && (!blob_len || write_all(e, blob, blob_len));
    SDL_free(json);
    if (ok) SDL_SignalSemaphore(e->wake);
    else stop(e);  // it would not start or take the request: everything waiting is answered
    SDL_UnlockMutex(e->write_lock);
    SDL_LockMutex(e->order_lock);
    e->sending++;
    SDL_BroadcastCondition(e->order_changed);
    SDL_UnlockMutex(e->order_lock);

    SDL_LockMutex(e->lock);
    bool timed_out = false;
    while (!c.done) {
        Uint64 silent = SDL_GetTicks() - c.last_heard;
        if (silent >= (Uint64)timeout_ms) { timed_out = true; break; }
        SDL_WaitConditionTimeout(e->answered, e->lock, (Sint32)SDL_min((Uint64)timeout_ms - silent, 1000));
    }
    for (call **at = &e->calls; *at; at = &(*at)->next)
        if (*at == &c) { *at = c.next; break; }
    while (c.reporting) SDL_WaitCondition(e->answered, e->lock);
    SDL_UnlockMutex(e->lock);
    if (timed_out) {
        // Silence this long is a native call that never returns, and every later request would
        // queue behind it. The worker is retired: its other requests are answered EXITED.
        eh_engine_stop(e);
        reply->status = EH_ENGINE_TIMEOUT;
        SDL_snprintf(reply->error, sizeof reply->error, "The %s engine did not answer in time", e->kind);
    }
    return reply->status;
}
