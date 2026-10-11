// Where to cut a recording so each decode gets one stretch of speech, and whether a stretch holds
// speech at all. The speech model drops whole sentences when several share a decode, so every
// pause is a cut; but dictation hesitates and the room clicks, and a fragment or a click decoded
// alone comes back as wrong or invented words, so short sounds are folded into the pause around
// them and short stretches ride with a neighbour. Ported from main/util/split-silence.js,
// renderer/chunk-boundary.js and renderer/speech-probe.js, whose numbers it reproduces exactly
// (tests/vectors/audio.json). All positions are sample frames of mono audio.
#pragma once
#include <stdbool.h>

#define EH_QUIET_RMS 0.012  // the silence level, shared with the overlay's pause detector

typedef struct { int from, to; } eh_span;

// A pause: a quiet run with sound on both sides. `parts` are its truly quiet runs, more than one
// when it absorbed a short noise, which is never cut through or squeezed away.
typedef struct {
    int from, to;
    eh_span *parts;
    int part_count;
} eh_pause;

typedef struct {
    double max_sec;            // the longest piece; required by eh_split_points and eh_fit_cap
    double min_pause_sec;      // the shortest pause that is a cut (infinite: none)
    double quiet_rms;
    double min_sound_sec;      // shorter sound with quiet around it is noise (0: off)
    double min_utterance_sec;  // a stretch with less speech joins a neighbour (0: off)
    double keep_pause_sec;     // inside a merged piece the decoder sees pauses this long at most (infinite: off)
    double lookback_sec, window_sec, hop_sec;  // the forced cut's search for the quietest moment
} eh_split;

// Cut at every pause of 0.2 s, nothing folded or merged.
#define EH_SPLIT_DEFAULTS { 0, 0.2, EH_QUIET_RMS, 0, 0, 1.0 / 0.0, 10, 0.3, 0.05 }

// The pauses of the buffer (SDL_free each one's parts, then the array).
eh_pause *eh_pause_spans(const float *samples, int count, int rate, const eh_split *o, int *pause_count);
void eh_pauses_free(eh_pause *pauses, int count);

// The frames at which to cut: the middle of every pause kept, plus whatever cuts hold each piece's
// decoded length to max_sec. Ascending; SDL_free the result (NULL when there are none).
int *eh_split_points(const float *samples, int count, int rate, const eh_split *o, int *cut_count);
// Cuts inside [from, to) so what the decoder gets for it, after eh_squeeze_pauses, fits max_sec.
int *eh_fit_cap(const float *samples, int rate, int from, int to, const eh_split *o, int *cut_count);
// The decoder's view of a piece: each pause longer than keep_pause_sec shortened to it by dropping
// the middle of its quiet. Returns `samples` itself when nothing was dropped, else a new array
// (SDL_free it); *out_count is the result's length.
float *eh_squeeze_pauses(const float *samples, int count, int rate, const eh_split *o, int *out_count);
// Seconds of sound in the buffer, noise left out.
double eh_sound_seconds(const float *samples, int count, int rate, const eh_split *o);

// The offset just past the quietest `window`-long stretch, scanning every `hop`; ties go to the
// latest. `count` when the buffer is too short to hold a window.
int eh_quietest_offset(const float *samples, int count, int window, int hop);

// Whether the buffer holds words the decoder should find: enough of it loud on its own terms, or
// standing well above the buffer's own noise floor. Leans toward yes: a wrong yes costs a decode,
// a wrong no costs a sentence.
bool eh_contains_speech(const float *samples, int count, int rate);
