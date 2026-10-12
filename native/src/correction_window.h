// The correction editor. Call everything on the main thread, without the loop's lock.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

// Opens the editor on a transcript ("" when there is none saved), or brings the open one forward:
// an open draft is never replaced. False with the reason when a dictation is under way.
bool eh_correction_window_open(const char *text, char *error, size_t error_size);
bool eh_correction_window_is_open(void);
void eh_correction_window_close(void);
bool eh_correction_window_event(const SDL_Event *e);
int eh_correction_window_frame(void);  // as eh_settings_window_frame

// For tests: its controls, and what it holds: "text", "spelling", "status", "note".
struct eh_ui *eh_correction_window_ui(void);
const char *eh_correction_window_text(const char *what);
