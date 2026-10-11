// The first-run wizard. Call everything on the main thread, without the loop's lock.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

void eh_wizard_window_open(void);  // or brings it forward
bool eh_wizard_window_is_open(void);
void eh_wizard_window_close(void);
bool eh_wizard_window_event(const SDL_Event *e);
int eh_wizard_window_frame(void);  // as eh_settings_window_frame

// For tests: its controls, and which of its seven steps shows (0 to 6).
struct eh_ui *eh_wizard_window_ui(void);
int eh_wizard_window_step(void);
