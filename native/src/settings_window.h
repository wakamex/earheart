// The settings window. Call everything on the main thread, without the loop's lock.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

// Opens the window, or brings it forward; `section` ("general", "live", "stt", "cleanup",
// "history", "advanced") scrolls to that section, NULL leaves the page where it is.
void eh_settings_window_open(const char *section);
bool eh_settings_window_is_open(void);
void eh_settings_window_close(void);
bool eh_settings_window_event(const SDL_Event *e);  // true when the event was the window's
// Draws a frame. Returns in how many milliseconds the window wants the next one without an event,
// or -1 when it waits for events or is closed.
int eh_settings_window_frame(void);
