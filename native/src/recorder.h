// The recorder: owns the microphone. Records 16 kHz mono, ships the recording in chunks while it
// grows (for the live preview and the final transcript's head start), and hands over the whole WAV
// on stop. Ported from the recording half of renderer/overlay.js. Called with the loop's lock held.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "jv.h"

typedef struct {
    void (*captured)(unsigned sid, uint8_t *wav, size_t len);  // the whole recording, after a stop
    void (*partial)(unsigned sid, int seq, bool final, int from_sample, int has_speech, uint8_t *wav, size_t len);
    void (*cancelled)(unsigned sid);  // nothing was captured: cancelled, or stopped before the microphone was live
    void (*error)(unsigned sid, const char *message);
    void (*live)(unsigned sid, const char *notice);  // the first audio arrived; `notice` is "" or a remark about the microphone
    void (*paused)(unsigned sid, bool paused);
} eh_recorder_deps;

void eh_recorder_init(const eh_recorder_deps *deps);
// A WAV file to play as the microphone in place of a real one, at `speed` times real time: for
// tests and measurements. NULL goes back to the real microphone.
void eh_recorder_fake_microphone(const char *wav_path, double speed);

// `chunks` turns the partial sends on, `display` also sends the chunk in progress on every tick,
// and `live_preview` is the settings' stt.livePreview (intervalMs, chunkSeconds).
void eh_recorder_start(unsigned sid, const char *device_id, double max_seconds, bool chunks, bool display, const eh_jv *live_preview);
void eh_recorder_stop(void);
void eh_recorder_cancel(void);
void eh_recorder_pause_toggle(void);

bool eh_recorder_active(void);
bool eh_recorder_paused(void);
double eh_recorder_seconds(void);            // captured so far, pauses left out
int eh_recorder_cap_seconds(void);           // where the recording stops by itself
int eh_recorder_levels(float *out, int max); // the loudness of the latest batches, oldest first
