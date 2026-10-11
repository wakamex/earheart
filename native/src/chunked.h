// Speech-to-text in pieces: a recording reaches the engine one stretch of speech at a time, cut
// at the pauses between utterances and never more than 20 s at once, decoded in order and joined.
// A fragment left by a hesitation rides with its neighbour, and noise is never decoded alone.
//
// The pieces are driven from the app and not inside the worker because a piece that finished is
// text in this process, so it survives the worker dying on a later one. A piece whose worker died
// or went silent is tried once more on a fresh worker; one that still fails is skipped and the rest
// carry on, so the user gets every word that could be decoded, marked partial, and not an error.
// Ported from main/chunked-decode.js.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    EH_DECODE_OK,
    EH_DECODE_FAILED,       // the engine answered with an error, which a retry would repeat
    EH_DECODE_EXITED,       // the worker died: worth one more try on a fresh one
    EH_DECODE_TIMEOUT,      // the worker went silent: retire it, then one more try
    EH_DECODE_EMPTY_SPEECH, // speech was heard and no text came back
} eh_decode_code;

// Decodes one WAV. On EH_DECODE_OK *text is the transcript (SDL_malloc'd, may be empty) and
// *decode_ms the engine's own decode time when it reports one; otherwise `error` says why.
typedef eh_decode_code eh_transcribe_fn(void *user, const uint8_t *wav, size_t len, char **text, int *decode_ms, char *error, size_t error_size);

// A committed live-preview chunk: the text of the frames [from, to) of the recording.
typedef struct { int from, to; const char *text; } eh_salvage;

typedef struct {
    eh_transcribe_fn *run;
    void *user;
    void (*restart_stt)(void *user);  // retire a wedged worker before the retry; may be NULL
    bool (*stale)(void *user);        // true once the dictation was cancelled; may be NULL
    void (*on_decode_ms)(void *user, int ms);  // each piece's decode time; may be NULL
    double max_sec;            // 0: 20
    double min_pause_sec;      // 0: the splitter's own 0.2; infinite: never cut at a pause
    double min_sound_sec;      // negative: 0.15; 0 turns folding noise off
    double min_utterance_sec;  // negative: 2; 0 turns joining fragments off
    double keep_pause_sec;     // 0: 0.5; infinite: pauses reach the decoder whole
    const char *prefix_text;   // trusted text that precedes the WAV; kept even if every piece fails
    const eh_salvage *salvage; // a broken snapshot's chunks: one over a range that fails stands in for it
    int salvage_count;
    const char *salvage_text;  // that snapshot's whole text, the last resort
} eh_chunked;

typedef struct {
    int from, to, group, attempts;
    bool ok, skipped, noise, unconfirmed;
    char *text;
    eh_decode_code code;
    char error[256];
} eh_piece;

typedef struct {
    char *text;
    bool partial;  // some of the audio could not be decoded; `text` is what was recovered
    bool stale;    // cancelled part-way; `text` is empty
    eh_piece *pieces;
    int piece_count;
    eh_decode_code code;  // when the call fails: the first failure
    char error[256];
} eh_chunked_result;

// False only when nothing at all was recovered, so a model that is not installed still surfaces as
// an error and not as an empty dictation. Free the result either way.
bool eh_transcribe_chunked(const uint8_t *wav, size_t len, const eh_chunked *o, eh_chunked_result *r);
void eh_chunked_result_free(eh_chunked_result *r);

char *eh_join_text(const char *a, const char *b);  // the two joined by a space, either may be empty or NULL; SDL_free
