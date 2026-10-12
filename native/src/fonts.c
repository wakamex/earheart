#include "fonts.h"

gs_fontset *eh_fonts(void) {
    static gs_fontset *fonts;
    if (!fonts) fonts = gs_fontset_system();
    return fonts;
}

gs_fontset *eh_fonts_semibold(void) {
    static gs_fontset *fonts;
    if (!fonts) fonts = gs_fontset_system_weight(600);
    return fonts;
}

gs_fontset *eh_fonts_mono(void) {
    static gs_fontset *fonts;
    if (!fonts) fonts = gs_fontset_system_mono(400);
    return fonts;
}
