// The system's interface font with its fallbacks, shared by every window.
#pragma once
#include "gs_text.h"

gs_fontset *eh_fonts(void);
gs_fontset *eh_fonts_semibold(void);  // for the overlay's status word and the windows' headings
gs_fontset *eh_fonts_mono(void);      // for the overlay's timer: digits of one width
