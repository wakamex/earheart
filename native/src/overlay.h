// The overlay: one solid bar along the bottom of the screen that says what the dictation is doing.
// The live transcript fills its top and grows freely; the control row sits along its bottom with
// the state in one word beside its dot, the voice drawn as it lands, the timer, and the three keys
// everyone knows: pause, done, discard. It never takes the keyboard from the app being dictated
// into. Design and its reasons: DESIGN.md. Ported from renderer/overlay.js, overlay.html and
// overlay.css. Everything here runs on the main thread.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

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

// Draws the model into a renderer whose output is `width` points wide at `scale` pixels a point,
// its bottom edge at `height`. Returns the height in points the card needs, margins included.
float eh_overlay_draw(SDL_Renderer *r, const eh_overlay_model *m, float width, float height, float scale, float opacity);
// One frame as a PNG, without a window: for tests and the README's screenshots.
bool eh_overlay_shot(const eh_overlay_model *m, const char *png_path, float scale);

// The window. `init` makes it, hidden; `event` gives it the events that are its own; `frame`
// brings it up to date with the app's state and draws it, and answers in how many milliseconds it
// wants to be drawn again whatever happens (-1: only when something changes).
bool eh_overlay_init(void);
bool eh_overlay_event(const SDL_Event *e);
int eh_overlay_frame(void);
void eh_overlay_quit(void);
