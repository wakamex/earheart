// The overlay: one solid bar along the bottom of the screen that says what the dictation is doing.
// The live transcript fills its top and grows freely; the control row sits along its bottom with
// the state in one word beside its dot, the voice drawn as it lands, the timer, and the three keys
// everyone knows: pause, done, discard. It never takes the keyboard from the app being dictated
// into. Design and its reasons: DESIGN.md. Ported from renderer/overlay.js, overlay.html and
// overlay.css. Everything here runs on the main thread.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include "app.h"

// What one frame shows: everything the drawing depends on, so a frame can be drawn from a model
// made up by a test as well as from the app's state.
typedef struct {
    char status[16];       // starting, recording, paused, transcribing, cleaning, delivering, done, empty, error
    char title[64];        // the state in a word: "Listening…"
    char detail[256];      // the line that takes the waveform's place after a dictation, or ""
    const char *clean;     // the live transcript's settled text, or NULL
    const char *raw;       // and the whole raw text so far
    float levels[600];     // the voice, newest first, one per column
    int level_count;
    float live_level;      // the column being written now, and how far into it the frame falls
    float column_fraction;
    char timer[16];
    bool cap_warning;      // the recording will stop by itself within a minute
    bool progress;         // a determinate bar for the phase
    float fraction;
    int hover;             // which key the pointer is over: 1 pause, 2 done, 3 discard, 0 none
    int pressed;
    bool card_hover;
    double seconds;        // the clock the dot's pulse runs on
} eh_overlay_model;

#define EH_OVERLAY_WIDTH 500
#define EH_OVERLAY_HEIGHT 95

// Where a frame put things, in points from the window's top left, for hit testing and for tests.
typedef struct {
    SDL_FRect card, wave, keys[3];  // keys: pause, done, discard
    SDL_FRect detail;               // the box the detail's lines take
    int detail_lines;               // drawn: two at most
    bool detail_clipped;            // it had more
    SDL_FRect progress;             // the track, when the model has one
    int transcript_lines;
} eh_overlay_layout;

// What the app says, at one moment.
typedef struct {
    const eh_view *view;
    double captured;  // seconds of audio in the take, pauses left out
    int cap;          // where the take stops by itself, or 0
    float level;      // the latest loudness
} eh_overlay_input;

// Brings the overlay's own state (the waveform's history, a finished bar's hold) up to `now`, in
// milliseconds, and fills the model; its texts point into the view. Returns in how many
// milliseconds the model will have changed by itself (-1: not until the app says something new).
int eh_overlay_step(const eh_overlay_input *in, Uint64 now, eh_overlay_model *m);
void eh_overlay_reset(void);  // forgets that state, between tests
// Pause works while audio is being taken or held; done also while the microphone is starting;
// discard, which turns into dismiss, always.
bool eh_overlay_key_enabled(const eh_overlay_model *m, int kind);

// Draws the model into a renderer whose output is `width` points wide at `scale` pixels a point,
// its bottom edge at `height`. Returns the height in points the card needs, margins included.
float eh_overlay_draw(SDL_Renderer *r, const eh_overlay_model *m, float width, float height, float scale, float opacity);
// One frame without a window, at the height the card asks for: as pixels (RGBA, to be destroyed
// by the caller) and as a PNG, for tests and the README's screenshots.
SDL_Surface *eh_overlay_render(const eh_overlay_model *m, float scale, eh_overlay_layout *layout);
bool eh_overlay_shot(const eh_overlay_model *m, const char *png_path, float scale);

// The window. `init` makes it, hidden; `event` gives it the events that are its own; `frame`
// brings it up to date with the app's state and draws it, and answers in how many milliseconds it
// wants to be drawn again whatever happens (-1: only when something changes).
bool eh_overlay_init(void);
bool eh_overlay_event(const SDL_Event *e);
int eh_overlay_frame(void);
void eh_overlay_quit(void);
