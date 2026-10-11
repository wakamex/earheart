// The settings window. Call everything on the main thread, without the loop's lock.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

// Opens the window, or brings it forward; `section` ("general", "live", "stt", "cleanup",
// "history", "advanced") scrolls to that section, NULL leaves the page where it is.
void eh_settings_window_open(const char *section);
// Opens it afresh with the wizard's choices and a line saying where they came from.
void eh_settings_window_open_from_wizard(void);
bool eh_settings_window_is_open(void);
void eh_settings_window_close(void);
bool eh_settings_window_event(const SDL_Event *e);  // true when the event was the window's
// Draws a frame. Returns in how many milliseconds the window wants the next one without an event,
// or -1 when it waits for events or is closed.
int eh_settings_window_frame(void);

// For tests. The window's controls are reached through eh_settings_window_ui; `text` gives what a
// part of the window says now: "save", "section", "hotkey" to "correct-hotkey", "hotkey-status" to
// "correct-hotkey-status", "stt-test", "stt-fetch", "cleanup-test", "cleanup-fetch", "max-seconds".
// `untouched` is whether the draft, written out, is the settings it was filled from.
struct eh_ui *eh_settings_window_ui(void);
const char *eh_settings_window_text(const char *what);
bool eh_settings_window_untouched(void);
