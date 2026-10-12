#include "fonts.h"

gs_fontset *eh_fonts(void) {
    static gs_fontset *fonts;
    if (!fonts) fonts = gs_fontset_system();
    return fonts;
}
