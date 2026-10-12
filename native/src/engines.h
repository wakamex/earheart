// The engines as the pipeline uses them: is the model there, load it, transcribe, clean. Built-in
// speech and cleanup each run in a worker process of their own (engine.h); a remote service is an
// HTTP call. Every function here is called by a task holding the loop's lock (loop.h) and waits
// without it. Ported from main/engines/index.js and main/services/route.js.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "chunked.h"
#include "gs_json.h"

// What an AbortController's signal is in the Electron build: something work is started with and
// that whoever started it can abort. Aborting also stops a cleanup the signal is running.
typedef struct eh_signal eh_signal;
eh_signal *eh_signal_new(void);
eh_signal *eh_signal_ref(eh_signal *s);
void eh_signal_unref(eh_signal *s);
void eh_signal_abort(eh_signal *s);
bool eh_signal_aborted(const eh_signal *s);  // false for NULL

void eh_engines_init(void);
void eh_engines_stop(void);         // ends both workers
void eh_engines_restart_stt(void);  // retires a wedged speech worker
// Ends each worker that holds a model and has nothing in flight. False when one was busy and
// stays loaded, so the caller can look again soon.
bool eh_engines_unload_idle(void);

// Whether a dictation with these speech settings can be transcribed, from the catalogue and the
// disk alone: cheap enough to ask on every hotkey press, before the microphone opens.
typedef struct {
    bool ok;
    bool unknown;  // not ok: the model is not in the catalogue; otherwise it is not downloaded
    char model_id[128], label[160];
} eh_stt_readiness;
eh_stt_readiness eh_stt_ready(const gs_jval *stt);

// Loads the built-in speech model if it is not the one loaded. False with `error` set.
bool eh_ensure_stt(const char *model_id, char *error, size_t error_size);
// One WAV through the engine the speech settings name. The eh_transcribe_fn shape, for the chunked
// decoder: `user` is the settings' "stt" section.
eh_decode_code eh_transcribe(const gs_jval *stt, const eh_signal *signal, const uint8_t *wav, size_t len, char **text, int *decode_ms, char *error, size_t error_size);
// The transcript cleaned by the engine the cleanup settings name, with the stumbles the model left
// stripped for the styles that ask for it. NULL with `error` set on failure, an abort included.
typedef void eh_fraction_fn(void *user, float fraction);
char *eh_clean(const char *transcript, const gs_jval *cleanup, eh_signal *signal, eh_fraction_fn *progress, void *user, char *error, size_t error_size);
// Evaluates the cleanup prompt, and the transcript known so far, into the built-in cleanup model
// ahead of the clean. Best effort: returns at once and reports nothing.
void eh_prime_cleanup(const gs_jval *cleanup, const char *transcript_prefix);
void eh_cancel_clean(void);  // stops every cleanup running or queued on the built-in worker; returns at once

int eh_clean_context_for(double max_recording_seconds);  // the cleanup context that fits the longest dictation allowed
