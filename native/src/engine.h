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
void eh_engine_stop(eh_engine *e);           // ends the worker, which is how its models' memory is given back

// Sends a request built with eh_jw (this adds "id"; put "type" in it) and returns its id. The
// worker is started when it is not running. `blob` may be NULL.
unsigned eh_engine_send(eh_engine *e, eh_jw *request, const void *blob, size_t blob_len);

typedef struct {
    unsigned id;
    bool final;      // false: a progress message
    bool ok;
    float progress;
    gs_json *doc;    // the whole message; gs_json_free it
    gs_jv v;         // its root
} eh_engine_msg;

// The next message from the worker, if one has arrived. When the worker dies, every request still
// waiting is answered with ok false and the error "The KIND engine stopped".
bool eh_engine_poll(eh_engine *e, eh_engine_msg *msg);
// Blocks until request `id` is answered, dropping other requests' messages. `progress` may be NULL.
bool eh_engine_wait(eh_engine *e, unsigned id, eh_engine_msg *msg, void (*progress)(void *user, float f), void *user);

// The worker's side: serves requests until its input closes. Returns the process's exit code.
int eh_worker_main(const char *kind);
