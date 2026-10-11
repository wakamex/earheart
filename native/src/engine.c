#include "engine.h"
#include "gs_http.h"
#include "paths.h"

typedef struct msg_node { eh_engine_msg msg; struct msg_node *next; } msg_node;

struct eh_engine {
    char kind[16];
    SDL_Process *proc;
    SDL_IOStream *in, *out;
    SDL_Thread *reader;
    SDL_Mutex *lock;
    SDL_Semaphore *wake;      // posted once per request sent, and once to end the reader
    SDL_AtomicInt quit;
    msg_node *head, *tail;    // messages read and not yet polled
    unsigned *pending;        // ids sent and not yet answered
    int pending_count, pending_cap;
    unsigned next_id;
};

static void put_le32(Uint8 *p, Uint32 v) { p[0] = (Uint8)v; p[1] = (Uint8)(v >> 8); p[2] = (Uint8)(v >> 16); p[3] = (Uint8)(v >> 24); }
static Uint32 get_le32(const Uint8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (Uint32)p[3] << 24; }

static void enqueue(eh_engine *e, eh_engine_msg msg) {
    msg_node *n = SDL_calloc(1, sizeof *n);
    n->msg = msg;
    if (e->tail) e->tail->next = n; else e->head = n;
    e->tail = n;
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

static void fail_pending(eh_engine *e) {
    for (int i = 0; i < e->pending_count; i++) {
        char text[128];
        int len = SDL_snprintf(text, sizeof text, "{\"id\":%u,\"ok\":false,\"error\":\"The %s engine stopped\"}", e->pending[i], e->kind);
        eh_engine_msg m = { .id = e->pending[i], .final = true, .doc = gs_json_parse(text, (size_t)len) };
        m.v = gs_json_root(m.doc);
        enqueue(e, m);
    }
    e->pending_count = 0;
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
            eh_engine_msg m = { 0 };
            if (ok) {
                m.doc = gs_json_parse(json, json_len);
                m.v = gs_json_root(m.doc);
                ok = m.doc != NULL;
            }
            SDL_free(json);
            SDL_LockMutex(e->lock);
            if (!ok) {
                fail_pending(e);
                SDL_UnlockMutex(e->lock);
                return 0;
            }
            m.id = (unsigned)gs_json_num(gs_json_get(m.v, "id"), 0);
            gs_jv progress = gs_json_get(m.v, "progress");
            m.final = !progress;
            m.ok = gs_json_bool(gs_json_get(m.v, "ok"), false);
            m.progress = (float)gs_json_num(progress, 0);
            if (m.final) {
                for (int i = 0; i < e->pending_count; i++)
                    if (e->pending[i] == m.id) { e->pending[i] = e->pending[--e->pending_count]; break; }
                answered = true;
            }
            enqueue(e, m);
            SDL_UnlockMutex(e->lock);
        }
    }
}

eh_engine *eh_engine_new(const char *kind) {
    eh_engine *e = SDL_calloc(1, sizeof *e);
    SDL_strlcpy(e->kind, kind, sizeof e->kind);
    e->lock = SDL_CreateMutex();
    e->wake = SDL_CreateSemaphore(0);
    return e;
}

bool eh_engine_running(eh_engine *e) {
    if (!e->proc) return false;
    int code;
    return !SDL_WaitProcess(e->proc, false, &code);
}

void eh_engine_stop(eh_engine *e) {
    if (!e->proc) return;
    SDL_SetAtomicInt(&e->quit, 1);
    SDL_KillProcess(e->proc, true);
    SDL_SignalSemaphore(e->wake);
    SDL_WaitThread(e->reader, NULL);
    SDL_DestroyProcess(e->proc);
    e->proc = NULL;
    e->reader = NULL;
    SDL_SetAtomicInt(&e->quit, 0);
    while (SDL_TryWaitSemaphore(e->wake)) {}
    SDL_LockMutex(e->lock);
    fail_pending(e);
    SDL_UnlockMutex(e->lock);
}

void eh_engine_free(eh_engine *e) {
    if (!e) return;
    eh_engine_stop(e);
    for (msg_node *n = e->head, *next; n; n = next) { next = n->next; gs_json_free(n->msg.doc); SDL_free(n); }
    SDL_DestroyMutex(e->lock);
    SDL_DestroySemaphore(e->wake);
    SDL_free(e->pending);
    SDL_free(e);
}

static bool start(eh_engine *e) {
    if (eh_engine_running(e)) return true;
    eh_engine_stop(e);  // a worker that died: collect it before starting another
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

unsigned eh_engine_send(eh_engine *e, eh_jw *request, const void *blob, size_t blob_len) {
    unsigned id = ++e->next_id;
    eh_jw_num(request, "id", id);
    char *json = eh_jw_end(request);
    Uint8 head[8];
    put_le32(head, (Uint32)SDL_strlen(json));
    put_le32(head + 4, (Uint32)blob_len);
    SDL_LockMutex(e->lock);
    if (e->pending_count == e->pending_cap) e->pending = SDL_realloc(e->pending, sizeof(unsigned) * (size_t)(e->pending_cap = e->pending_cap * 2 + 8));
    e->pending[e->pending_count++] = id;
    SDL_UnlockMutex(e->lock);
    bool ok = start(e) && write_all(e, head, 8) && write_all(e, json, SDL_strlen(json)) && (!blob_len || write_all(e, blob, blob_len));
    SDL_free(json);
    if (ok) {
        SDL_SignalSemaphore(e->wake);
    } else {
        // The worker would not start or take the request: answer it here.
        SDL_LockMutex(e->lock);
        fail_pending(e);
        SDL_UnlockMutex(e->lock);
    }
    return id;
}

bool eh_engine_poll(eh_engine *e, eh_engine_msg *msg) {
    SDL_LockMutex(e->lock);
    msg_node *n = e->head;
    if (n) {
        e->head = n->next;
        if (!e->head) e->tail = NULL;
        *msg = n->msg;
        SDL_free(n);
    }
    SDL_UnlockMutex(e->lock);
    return n != NULL;
}

bool eh_engine_wait(eh_engine *e, unsigned id, eh_engine_msg *msg, void (*progress)(void *user, float f), void *user) {
    for (;;) {
        while (eh_engine_poll(e, msg)) {
            if (msg->id == id && msg->final) return msg->ok;
            if (msg->id == id && progress) progress(user, msg->progress);
            gs_json_free(msg->doc);
        }
        SDL_Delay(2);
    }
}
