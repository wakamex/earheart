// The live preview: the partial transcript shown while recording, and with it the head start for
// the final transcript. The recorder ships audio in chunks; only the chunk in progress is decoded
// again on each tick, so decode cost stays flat however long the dictation runs. A committed
// chunk's text is never decoded again: the final pass takes the committed text as the transcript's
// prefix and decodes only the tail, as long as the commits cover the recording's start without a
// hole. Any hole marks the snapshot broken and the final pass decodes everything, keeping the
// committed chunks as salvage. Cleanup, by contrast, re-cleans the whole committed text at each
// pause, because cleaning two halves reads differently from cleaning the whole.
//
// All of it is best effort and silent: a dropped or failed partial is cosmetic and never disturbs
// the dictation. Ported from main/live-preview.js. Called with the loop's lock held.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "chunked.h"
#include "engines.h"
#include "gs_json.h"

typedef struct {
    // Decodes a chunk for the preview: the text, or NULL with `error` set, and in *partial_text
    // (SDL_malloc'd, or NULL) the words recovered when the chunk decoded only in part.
    char *(*transcribe)(const uint8_t *wav, size_t len, const gs_jval *stt, eh_signal *signal, char **partial_text, char *error, size_t error_size);
    char *(*clean)(const char *raw, const gs_jval *cleanup, eh_signal *signal, char *error, size_t error_size);
    void (*send)(const char *kind, const char *text);  // a line for the overlay: "raw" or "cleaned"
    bool (*is_current)(unsigned sid);                  // sid is the active, still-recording session
    void (*on_error)(const char *message);
} eh_preview_deps;

void eh_preview_init(const eh_preview_deps *deps);

// One chunk's audio (owned by the call). `seq` identifies the chunk and `final` says this is its
// last send, which commits it. `from_sample` is where the chunk starts in the recording, and
// `has_speech` whether the recorder heard speech in it: -1 when it did not say.
void eh_preview_audio(unsigned sid, int seq, bool final, int from_sample, int has_speech, uint8_t *wav, size_t len);
void eh_preview_cancel(void);  // aborts partial work in flight and forgets the session

typedef struct {
    char *committed_raw;   // the committed chunks' text
    int decoded_samples;   // how much of the recording's start that text covers
    bool broken;           // the coverage has a hole
    eh_salvage *chunks;    // every committed chunk that produced text, with the frames it covers
    int chunk_count;
} eh_preview_snapshot;

// The committed transcript and the coverage it stands for. Read before eh_preview_cancel.
eh_preview_snapshot eh_preview_snapshot_final(void);
void eh_preview_snapshot_free(eh_preview_snapshot *s);
