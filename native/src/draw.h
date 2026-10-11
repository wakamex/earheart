// The shapes Earheart's design is made of, drawn smooth on an SDL renderer: rounded rectangles,
// discs, and strokes with round ends. Coordinates are pixels. gesso is getting these (see
// gesso-requests.txt); this is the app's own until then.
#pragma once
#include <SDL3/SDL.h>

void eh_fill_round(SDL_Renderer *r, float x, float y, float w, float h, float radius, SDL_FColor c);
// The outline of a rounded rectangle, `width` thick, inside its edge.
void eh_stroke_round(SDL_Renderer *r, float x, float y, float w, float h, float radius, float width, SDL_FColor c);
void eh_fill_circle(SDL_Renderer *r, float cx, float cy, float radius, SDL_FColor c);
// A straight stroke from one point to another, `width` thick, with round ends.
void eh_line(SDL_Renderer *r, float x0, float y0, float x1, float y1, float width, SDL_FColor c);
void eh_fill_triangle(SDL_Renderer *r, float x0, float y0, float x1, float y1, float x2, float y2, SDL_FColor c);
SDL_FColor eh_rgba(unsigned rgb, float alpha);  // 0xRRGGBB and an opacity

// The system's interface font with its fallbacks, shared by every window.
struct gs_fontset *eh_fonts(void);
