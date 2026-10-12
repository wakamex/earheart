// Always-on listening: the microphone stays open, gesso's voice detector says where speech starts
// and stops, and each stretch of speech goes through the pipeline as a dictation of its own, into
// whatever window has the keyboard. Nothing is recorded or transcribed between stretches. This is
// the native build's own; the Electron build has no such mode.
#pragma once
#include <stdbool.h>
#include "pipeline.h"

// Turn-taking on the detector's answers: one speech probability per 10 ms frame in, +1 when speech
// has started, -1 when it has stopped, 0 otherwise. Speech starts after 50 ms above one half and
// stops after 700 ms below it, which is longer than the pauses inside a sentence.
typedef struct { int run, quiet; bool speaking; } eh_turn;
enum { EH_TURN_START_FRAMES = 5, EH_TURN_STOP_FRAMES = 70 };
int eh_turn_step(eh_turn *t, float probability);

// All with the loop's lock held. Starting opens the microphone (the saved one by its name); false
// with `error` set when it cannot be opened. Starting again with another microphone moves to it.
bool eh_listen_start(const char *device_id, double max_seconds, char *error, size_t error_size);
void eh_listen_stop(void);
bool eh_listen_on(void);
// The pipeline changed state: a dictation by hotkey holds the listener off while it records, and
// an idle pipeline takes the next stretch that was waiting.
void eh_listen_state(eh_state state);
