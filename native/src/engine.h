// The app's side of an engine worker: the same executable started as `earheart --engine KIND`,
// answering requests over its standard input and output. Speech-to-text and cleanup each get a
// worker of their own, so they run at the same time and a crash in one engine's native code takes
// down neither the other nor the app.
//
// A message is two little-endian 32-bit lengths, then a JSON object of that first length, then that
// second length of bytes (a WAV file, or nothing). Requests carry "id" and "type"; the worker
// answers each with {"id", "ok": true, ...} or {"id", "ok": false, "error"}, after any number of
// {"id", "progress"} messages.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include "gs_json.h"
#include "jw.h"

typedef struct eh_engine eh_engine;

eh_engine *eh_engine_new(const char *kind);  // "stt" or "cleanup"; the worker starts with the first request
void eh_engine_free(eh_engine *e);           // ends the worker
bool eh_engine_running(eh_engine *e);
bool eh_engine_busy(eh_engine *e);           // a request is waiting for its answer
// Ends the worker, which is how its models' memory goes back to the system. Requests waiting on
// it are answered EH_ENGINE_EXITED, and the next request starts a fresh worker.
void eh_engine_stop(eh_engine *e);
// Goes up each time a worker goes away, so a caller can tell that what it loaded is gone.
unsigned eh_engine_generation(eh_engine *e);

typedef enum {
    EH_ENGINE_OK,
    EH_ENGINE_FAILED,   // the worker answered with an error, which a retry would repeat
    EH_ENGINE_EXITED,   // the worker died, or was stopped, before answering
    EH_ENGINE_TIMEOUT,  // no answer and no progress within the deadline; the worker was retired
} eh_engine_status;

typedef struct {
    eh_engine_status status;
    gs_json *doc;     // the answer, when the worker gave one
    gs_jv v;          // its root
    char error[256];  // why, when status is not EH_ENGINE_OK
} eh_engine_reply;

void eh_engine_reply_free(eh_engine_reply *r);

#define EH_ENGINE_TIMEOUT_MS 180000

// Sends a request built with eh_jw (this adds "id"; put "type" in it) and waits for its answer.
// Safe from several threads at once: each caller gets its own answer. `timeout_ms` bounds silence,
// not duration: each progress message starts it again (0: EH_ENGINE_TIMEOUT_MS). `progress` is
// called on another thread. `blob` and `progress` may be NULL.
typedef void eh_engine_progress(void *user, float fraction);
eh_engine_status eh_engine_call(eh_engine *e, eh_jw *request, const void *blob, size_t blob_len, int timeout_ms,
                                eh_engine_progress *progress, void *user, eh_engine_reply *reply);

// The same with the request's place among the others taken beforehand: requests reach the worker
// in the order of eh_engine_reserve calls, whichever thread gets to send first. Every place
// reserved must be used.
unsigned eh_engine_reserve(eh_engine *e);
eh_engine_status eh_engine_call_at(eh_engine *e, unsigned place, eh_jw *request, const void *blob, size_t blob_len, int timeout_ms,
                                   eh_engine_progress *progress, void *user, eh_engine_reply *reply);

// The worker's side: serves requests until its input closes. Returns the process's exit code.
int eh_worker_main(const char *kind);
