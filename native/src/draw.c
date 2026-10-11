#include "draw.h"
#include <math.h>

#define ARC 10  // segments in a quarter circle

SDL_FColor eh_rgba(unsigned rgb, float alpha) {
    return (SDL_FColor){ (float)(rgb >> 16 & 0xff) / 255, (float)(rgb >> 8 & 0xff) / 255, (float)(rgb & 0xff) / 255, alpha };
}

// The outline of a rounded rectangle as points, clockwise. Returns how many.
static int outline(float x, float y, float w, float h, float r, SDL_FPoint *p) {
    r = SDL_max(0, SDL_min(r, SDL_min(w, h) / 2));
    const float cx[4] = { x + w - r, x + w - r, x + r, x + r }, cy[4] = { y + r, y + h - r, y + h - r, y + r };
    int n = 0;
    for (int corner = 0; corner < 4; corner++)
        for (int i = 0; i <= ARC; i++) {
            float a = ((float)corner - 1 + (float)i / ARC) * 1.5707963f;
            p[n++] = (SDL_FPoint){ cx[corner] + cosf(a) * r, cy[corner] + sinf(a) * r };
        }
    return n;
}

// A convex shape filled, with a one-pixel rim that fades out so its edge is smooth.
static void fill_convex(SDL_Renderer *r, const SDL_FPoint *p, int n, SDL_FColor c) {
    enum { MAX = 4 * (ARC + 1) };
    SDL_Vertex v[2 * MAX + 1];
    int idx[9 * MAX];
    float mx = 0, my = 0;
    for (int i = 0; i < n; i++) { mx += p[i].x; my += p[i].y; }
    mx /= (float)n;
    my /= (float)n;
    SDL_FColor clear = c;
    clear.a = 0;
    v[0] = (SDL_Vertex){ { mx, my }, c, { 0, 0 } };
    for (int i = 0; i < n; i++) {
        // Half a pixel in and half a pixel out along the way from the middle.
        float dx = p[i].x - mx, dy = p[i].y - my, len = SDL_max(0.001f, sqrtf(dx * dx + dy * dy));
        v[1 + i] = (SDL_Vertex){ { p[i].x - dx / len * 0.5f, p[i].y - dy / len * 0.5f }, c, { 0, 0 } };
        v[1 + n + i] = (SDL_Vertex){ { p[i].x + dx / len * 0.5f, p[i].y + dy / len * 0.5f }, clear, { 0, 0 } };
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        idx[k++] = 0; idx[k++] = 1 + i; idx[k++] = 1 + j;
        idx[k++] = 1 + i; idx[k++] = 1 + n + i; idx[k++] = 1 + n + j;
        idx[k++] = 1 + i; idx[k++] = 1 + n + j; idx[k++] = 1 + j;
    }
    SDL_RenderGeometry(r, NULL, v, 1 + 2 * n, idx, k);
}

void eh_fill_round(SDL_Renderer *r, float x, float y, float w, float h, float radius, SDL_FColor c) {
    if (w <= 0 || h <= 0 || c.a <= 0) return;
    SDL_FPoint p[4 * (ARC + 1)];
    fill_convex(r, p, outline(x, y, w, h, radius, p), c);
}

void eh_fill_circle(SDL_Renderer *r, float cx, float cy, float radius, SDL_FColor c) {
    eh_fill_round(r, cx - radius, cy - radius, radius * 2, radius * 2, radius, c);
}

void eh_stroke_round(SDL_Renderer *r, float x, float y, float w, float h, float radius, float width, SDL_FColor c) {
    enum { MAX = 4 * (ARC + 1) };
    SDL_FPoint outer[MAX], inner[MAX];
    int n = outline(x, y, w, h, radius, outer);
    outline(x + width, y + width, w - 2 * width, h - 2 * width, SDL_max(0, radius - width), inner);
    SDL_Vertex v[2 * MAX];
    int idx[6 * MAX], k = 0;
    for (int i = 0; i < n; i++) {
        v[i] = (SDL_Vertex){ outer[i], c, { 0, 0 } };
        v[n + i] = (SDL_Vertex){ inner[i], c, { 0, 0 } };
        int j = (i + 1) % n;
        idx[k++] = i; idx[k++] = j; idx[k++] = n + i;
        idx[k++] = j; idx[k++] = n + j; idx[k++] = n + i;
    }
    SDL_RenderGeometry(r, NULL, v, 2 * n, idx, k);
}

void eh_line(SDL_Renderer *r, float x0, float y0, float x1, float y1, float width, SDL_FColor c) {
    // A stadium: the segment swept by a disc. Built as a rounded rectangle's outline, turned.
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy), h = width / 2;
    if (len < 0.001f) { eh_fill_circle(r, x0, y0, h, c); return; }
    float ux = dx / len, uy = dy / len;
    SDL_FPoint flat[4 * (ARC + 1)], p[4 * (ARC + 1)];
    int n = outline(-h, -h, len + width, width, h, flat);
    for (int i = 0; i < n; i++) p[i] = (SDL_FPoint){ x0 + flat[i].x * ux - flat[i].y * uy, y0 + flat[i].x * uy + flat[i].y * ux };
    fill_convex(r, p, n, c);
}

void eh_fill_triangle(SDL_Renderer *r, float x0, float y0, float x1, float y1, float x2, float y2, SDL_FColor c) {
    const SDL_FPoint p[3] = { { x0, y0 }, { x1, y1 }, { x2, y2 } };
    fill_convex(r, p, 3, c);
}
