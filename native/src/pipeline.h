// The dictation pipeline: record, transcribe, clean up, deliver.
//
//   idle --hotkey--> recording --hotkey--> processing --> idle
//                        |                     |
//                        +----cancel--> idle <-+ (error or cancel)
//
// Every dictation gets a session number that the recorder hands back with what it captured, so
// events from a session already torn down are ignored and cannot corrupt the current one. If
// cleanup fails the raw transcript is delivered; if paste fails the text is on the clipboard;
// history keeps the text either way. Ported from main/pipeline.js. Everything here is called with
// the loop's lock held.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "engines.h"
#include "gs_json.h"

typedef enum { EH_IDLE, EH_RECORDING, EH_PROCESSING } eh_state;

// How a text reached the user, for the done card.
typedef struct {
    char method[16];  // "paste" or "clipboard"
    char note[96];    // a short remark for the card, or ""
    char hint[240];   // the whole instruction when auto-paste failed, or ""
} eh_delivery;

// What the pipeline needs from the rest of the app. Every function is called with the lock held.
typedef struct {
    // The overlay. A status is a phase change; `detail` is an object or NULL (for "done":
    // preview, method, note, incomplete; for "error": message).
    void (*status)(const char *status, const gs_jval *detail);
    void (*progress)(const char *phase, float fraction);  // "transcribing" or "cleaning"
    void (*partial)(const char *kind, const char *text);  // the live transcript: "raw" or "cleaned"
    void (*overlay_show)(bool visible);
    void (*notify)(const char *title, const char *body, bool opens_settings);
    void (*history_changed)(void);
    void (*state_changed)(eh_state state);
    // The recorder.
    void (*record_start)(unsigned sid, const char *device_id, double max_seconds, bool chunks, bool display, const gs_jval *live_preview);
    void (*record_stop)(void);          // answered by eh_pipeline_captured, _record_cancelled or _record_error
    void (*record_cancel)(void);
    void (*record_pause_toggle)(void);
    void (*record_discard)(void);       // the overlay's close key, which knows what it means in each phase
    // Puts the text where the output settings say. False with `error` set when it reached nowhere.
    bool (*deliver)(const char *text, const gs_jval *output, eh_signal *signal, eh_delivery *result, char *error, size_t error_size);
} eh_pipeline_deps;

void eh_pipeline_init(const eh_pipeline_deps *deps, const char *rtf_path);
eh_state eh_pipeline_state(void);

void eh_pipeline_toggle(void);        // the record hotkey: start, or stop and transcribe
void eh_pipeline_pause_toggle(void);
void eh_pipeline_discard(void);
void eh_pipeline_cancel(void);
void eh_pipeline_settings_changed(void);

// From the recorder. The audio is owned by the call.
void eh_pipeline_captured(unsigned sid, uint8_t *wav, size_t len);
void eh_pipeline_partial(unsigned sid, int seq, bool final, int from_sample, int has_speech, uint8_t *wav, size_t len);
void eh_pipeline_record_cancelled(unsigned sid);
void eh_pipeline_record_error(unsigned sid, const char *message);
void eh_pipeline_overlay_dismiss(void);  // the overlay's close key once the dictation has settled
