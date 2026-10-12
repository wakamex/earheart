#include "overlay.h"
#include "gs_desk.h"
#include <math.h>
#include "app.h"
#include "fonts.h"
#include "gs_draw.h"
#include "gs_text.h"
#include "loop.h"
#include "pipeline.h"
#include "recorder.h"
#include "settings.h"
#include "stb_image_write.h"
#include "text.h"

// The palette of renderer/tokens.css and overlay.css.
#define INK 0x18181b
#define TEXT 0xf4f4f5
#define TEXT_MID 0xd4d4d8
#define TEXT_DIM 0xa1a1aa
#define IDLE 0x71717a
#define ACCENT 0xfb4d5c  // the voice: capture dot, waveform, progress
#define OK 0x34d399
#define ERR 0xf87171

#define MARGIN 12.0f
#define WAVE_COL 2.5f      // points a column of the waveform
#define WAVE_PUSH_MS 80    // of audio a column
#define WAVE_GAIN 6.0f

/* ---------------- drawing a frame ---------------- */

typedef struct { gs_fontset *fonts; gs_glyphs *glyphs; SDL_Renderer *r; float scale, opacity; } pen;

static SDL_FColor ink(const pen *p, unsigned rgb, float alpha) { return gs_rgba(rgb, alpha * p->opacity); }

// One line of text with its top at y, cut with an ellipsis to fit `max` points. Returns its width.
static float text_line(const pen *p, const char *s, float x, float y, float px, float max, SDL_FColor c) {
    char cut[512];
    float size = px * p->scale;
    SDL_strlcpy(cut, s, sizeof cut);
    if (max > 0 && gs_fontset_width(p->fonts, size, cut) > max * p->scale) {
        size_t n = SDL_strlen(cut);
        while (n > 0) {
            n = (size_t)gs_utf8_grapheme_prev(cut, (int)n);
            SDL_strlcpy(cut + n, "…", sizeof cut - n);
            if (gs_fontset_width(p->fonts, size, cut) <= max * p->scale) break;
        }
    }
    // The baseline sits about four fifths of the way down the em.
    return gs_fontset_draw(p->glyphs, p->fonts, size, x * p->scale, (y + px * 0.8f) * p->scale, cut, c) / p->scale;
}

// The two layers of the live transcript: the cleaned text is the settled line, and the part of
// the raw text past the cleaned line's last word trails it, dimmer, until cleanup settles it too.
static char *transcript_tail(const char *clean, const char *raw) {
    char *c = eh_trim(clean ? clean : ""), *r = eh_trim(raw ? raw : ""), *tail = NULL;
    if (!*c) {
        tail = SDL_strdup(r);
    } else if (*r) {
        // The cleaned text's last word that has a letter or number in it, compared without case
        // or punctuation, is looked for in the raw text from its end; what follows it is the tail.
        char *anchor = NULL;
        for (const char *end = c + SDL_strlen(c); end > c && !anchor;) {
            const char *start = end;
            while (start > c && !SDL_isspace((unsigned char)start[-1])) start--;
            char *word = eh_norm_word(start, end);
            if (*word) anchor = word; else SDL_free(word);
            end = start;
            while (end > c && SDL_isspace((unsigned char)end[-1])) end--;
        }
        for (const char *end = r + SDL_strlen(r); anchor && end > r && !tail;) {
            const char *start = end;
            while (start > r && !SDL_isspace((unsigned char)start[-1])) start--;
            char *word = eh_norm_word(start, end);
            if (!SDL_strcmp(word, anchor)) {
                while (SDL_isspace((unsigned char)*end)) end++;
                tail = SDL_strdup(end);
            }
            SDL_free(word);
            end = start;
            while (end > r && SDL_isspace((unsigned char)end[-1])) end--;
        }
        SDL_free(anchor);
    }
    SDL_free(c);
    SDL_free(r);
    return tail ? tail : SDL_strdup("");
}

static float dot_pulse(double seconds, double period) { return 1.0f - 0.55f * (float)(0.5 - 0.5 * cos(seconds * 6.283185307 / period)); }

static void draw_dot(const pen *p, const char *status, float cx, float cy, double t) {
    SDL_Renderer *r = p->r;
    float s = p->scale, x = cx * s, y = cy * s;
    if (!SDL_strcmp(status, "starting") || !SDL_strcmp(status, "paused")) {
        // A ring: the microphone is not delivering, or is held.
        float a = SDL_strcmp(status, "starting") ? 1 : dot_pulse(t, 1);
        gs_draw_outline(r, (SDL_FRect){ x - 4 * s, y - 4 * s, 8 * s, 8 * s }, 4 * s, 2 * s, ink(p, ACCENT, a));
    } else if (!SDL_strcmp(status, "recording")) {
        float a = dot_pulse(t, 2);
        gs_draw_circle(r, x, y, 7 * s, ink(p, ACCENT, 0.16f * a));
        gs_draw_circle(r, x, y, 4 * s, ink(p, ACCENT, a));
    } else {
        bool busy = !SDL_strcmp(status, "transcribing") || !SDL_strcmp(status, "cleaning") || !SDL_strcmp(status, "delivering");
        unsigned colour = busy ? TEXT_DIM : !SDL_strcmp(status, "done") ? OK : !SDL_strcmp(status, "error") ? ERR : IDLE;
        gs_draw_circle(r, x, y, 4 * s, ink(p, colour, busy ? dot_pulse(t, 1.2) : 1));
    }
}

bool eh_overlay_key_enabled(const eh_overlay_model *m, int kind) {
    bool paused = !SDL_strcmp(m->status, "paused"), recording = !SDL_strcmp(m->status, "recording"), starting = !SDL_strcmp(m->status, "starting");
    return kind == 3 || (kind == 1 ? recording || paused : recording || paused || starting);
}

// A key: a 32-point disc with its mark. `kind`: 1 pause (or resume when paused), 2 done, 3 discard.
static void draw_key(const pen *p, const eh_overlay_model *m, int kind, float cx, float cy) {
    SDL_Renderer *r = p->r;
    float s = p->scale, x = cx * s, y = cy * s;
    bool paused = !SDL_strcmp(m->status, "paused"), enabled = eh_overlay_key_enabled(m, kind);
    bool hover = enabled && m->hover == kind;
    float radius = (m->pressed == kind && enabled ? 16 * 0.92f : 16) * s;
    SDL_FColor mark = ink(p, TEXT_DIM, 1);
    if (!enabled) {
        mark = ink(p, 0xffffff, 0.28f);
    } else if (kind == 2) {
        // The primary action: filled white, the one bright thing on the bar.
        gs_draw_circle(r, x, y, radius, ink(p, hover ? 0xffffff : TEXT, 1));
        mark = ink(p, INK, 1);
    } else if (kind == 3 && hover) {
        gs_draw_circle(r, x, y, radius, ink(p, ERR, 0.12f));
        mark = ink(p, ERR, 1);
    } else if (hover || (kind == 1 && paused)) {
        gs_draw_circle(r, x, y, radius, ink(p, 0xffffff, 0.08f));
        mark = ink(p, TEXT, 1);
    }
    // The marks are drawn in a 16-unit box, 14 points wide (15 for the check).
    float u = (kind == 2 ? 15.0f : 14.0f) / 16 * s, ox = x - 8 * u, oy = y - 8 * u, w = 2 * u;
#define L(x0, y0, x1, y1) gs_draw_line(r, ox + (x0) * u, oy + (y0) * u, ox + (x1) * u, oy + (y1) * u, w, mark)
    if (kind == 1 && paused) {
        gs_draw_triangle(r, ox + 6 * u, oy + 3.4f * u, ox + 14 * u, oy + 8 * u, ox + 6 * u, oy + 12.6f * u, mark);
    } else if (kind == 1) {
        L(5.6f, 4.2f, 5.6f, 11.8f);
        L(10.4f, 4.2f, 10.4f, 11.8f);
    } else if (kind == 2) {
        L(3.4f, 8.6f, 6.7f, 11.8f);
        L(6.7f, 11.8f, 12.6f, 4.6f);
    } else {
        L(4.3f, 4.3f, 11.7f, 11.7f);
        L(11.7f, 4.3f, 4.3f, 11.7f);
    }
#undef L
}

static float eh_overlay_draw_with(SDL_Renderer *r, gs_glyphs *glyphs, const eh_overlay_model *m, float width, float height, float scale, float opacity, eh_overlay_layout *out) {
    eh_overlay_layout layout = { 0 };
    pen p = { eh_fonts(), glyphs, r, scale, opacity };
    float s = scale, card_w = width - 2 * MARGIN;
    bool recording = !SDL_strcmp(m->status, "recording"), live = recording || !SDL_strcmp(m->status, "paused") || !SDL_strcmp(m->status, "starting");

    // The transcript's lines, clean text and raw tail wrapped as one paragraph.
    char *tail = live ? transcript_tail(m->clean, m->raw) : SDL_strdup(""), *clean = eh_trim(live && m->clean ? m->clean : ""), *both = NULL;
    SDL_asprintf(&both, "%s%s%s", clean, *clean && *tail ? " " : "", tail);
    int boundary = (int)SDL_strlen(clean);
    gs_wrap *wrap = *both ? gs_fontset_wrap(p.fonts, 15 * s, both, (card_w - 32) * s) : NULL;
    float line_h = 22.5f, transcript_h = wrap ? 6 + (float)wrap->count * line_h + 10 + 1 : 0;
    float card_h = 1 + 14 + transcript_h + 44 + 8 + 1, needed = SDL_max((float)EH_OVERLAY_HEIGHT, card_h + 2 * MARGIN);
    float card_x = MARGIN, card_y = height - MARGIN - card_h;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColorFloat(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    gs_glyphs_begin_frame(glyphs);
    // The bar: solid near-black with a hairline edge, and its grip.
    gs_draw_round(r, (SDL_FRect){ card_x * s, card_y * s, card_w * s, card_h * s }, 16 * s, ink(&p, INK, 1));
    gs_draw_outline(r, (SDL_FRect){ card_x * s, card_y * s, card_w * s, card_h * s }, 16 * s, SDL_max(1, s), ink(&p, 0xffffff, 0.1f));
    gs_draw_round(r, (SDL_FRect){ (card_x + card_w / 2 - 18) * s, (card_y + 7) * s, 36 * s, 4 * s }, 2 * s, ink(&p, 0xffffff, m->card_hover ? 0.28f : 0.16f));

    if (wrap) {
        float y = card_y + 15 + 6;
        for (int i = 0; i < wrap->count; i++, y += line_h) {
            gs_wrap_line ln = wrap->lines[i];
            // A line is all cleaned text, all raw tail, or cleaned text up to the boundary.
            int split = SDL_clamp(boundary, ln.start, ln.end);
            char *a = SDL_strndup(both + ln.start, (size_t)(split - ln.start)), *b = SDL_strndup(both + split, (size_t)(ln.end - split));
            float x = (card_x + 16) * s, base = (y + line_h * 0.5f + 15 * 0.3f) * s;
            x += gs_fontset_draw(glyphs, p.fonts, 15 * s, x, base, a, ink(&p, TEXT, 1));
            gs_fontset_draw(glyphs, p.fonts, 15 * s, x, base, b, ink(&p, TEXT_DIM, 1));
            SDL_free(a);
            SDL_free(b);
        }
        gs_draw_round(r, (SDL_FRect){ card_x * s, (card_y + 15 + transcript_h - 1) * s, card_w * s, SDL_max(1, s) }, 0, ink(&p, 0xffffff, 0.08f));
    }

    // The control row, from its two ends inward.
    float row_y = card_y + card_h - 1 - 8 - 44, mid = row_y + 22, left = card_x + 12 + 4, right = card_x + card_w - 12;
    layout.card = (SDL_FRect){ card_x, card_y, card_w, card_h };
    layout.transcript_lines = wrap ? wrap->count : 0;
    draw_key(&p, m, 3, right - 16, mid);
    layout.keys[2] = (SDL_FRect){ right - 32, mid - 16, 32, 32 };
    right -= 32 + 10 + 10;
    draw_key(&p, m, 2, right - 16, mid);
    layout.keys[1] = (SDL_FRect){ right - 32, mid - 16, 32, 32 };
    right -= 32 + 10;
    draw_key(&p, m, 1, right - 16, mid);
    layout.keys[0] = (SDL_FRect){ right - 32, mid - 16, 32, 32 };
    right -= 32 + 10;
    // The timer keeps one width whatever its digits, so the waveform's edge does not shift with them.
    char widest[sizeof m->timer];
    SDL_strlcpy(widest, m->timer, sizeof widest);
    for (char *c = widest; *c; c++) if (*c >= '0' && *c <= '9') *c = '0';
    float timer_w = gs_fontset_width(p.fonts, 12 * s, widest) / s;
    text_line(&p, m->timer, right - timer_w, mid - 6, 12, 0, ink(&p, m->cap_warning ? ACCENT : recording ? TEXT : TEXT_DIM, 1));
    right -= timer_w + 10;
    draw_dot(&p, m->status, left + 4, mid, m->seconds);
    float title_w = text_line(&p, m->title, left + 8 + 7, mid - 6.25f, 12.5f, 150 - 15, ink(&p, !SDL_strcmp(m->status, "done") ? TEXT : TEXT_MID, 1));
    left += 8 + 7 + title_w + 10;

    // The waveform: the voice drawn as it lands, newest at the right edge. Outside recording it
    // dims to a ghost of the take, and further when a detail line takes its place.
    float wave_w = right - left, wave_a = *m->detail ? 0.07f : recording ? 1 : 0.25f;
    layout.wave = (SDL_FRect){ left, mid - 16, wave_w, 32 };
    if (wave_w > 4) {
        SDL_Rect clip = { (int)(left * s), (int)((mid - 16) * s), (int)(wave_w * s), (int)(32 * s) };
        SDL_SetRenderClipRect(r, &clip);
        float col = WAVE_COL * s, gap = col * 0.32f, bar = col - gap, max_amp = 32 * s - 2 * SDL_max(1, floorf(s + 0.5f)), edge = right * s;
        SDL_FColor c = ink(&p, ACCENT, wave_a);
        for (int i = 0; i < m->level_count; i++) {
            float x = edge - ((float)i + 1 + m->column_fraction) * col + gap;
            if (x + bar < left * s) break;
            float amp = SDL_max(bar, SDL_min(1, m->levels[i] * WAVE_GAIN) * max_amp);
            gs_draw_round(r, (SDL_FRect){ x, mid * s - amp / 2, bar, amp }, bar / 2, c);
        }
        float live_w = m->column_fraction * col - gap;
        if (recording && live_w > 0) {
            float amp = SDL_max(bar, SDL_min(1, m->live_level * WAVE_GAIN) * max_amp);
            gs_draw_round(r, (SDL_FRect){ edge - live_w, mid * s - amp / 2, live_w, amp }, SDL_min(live_w, bar) / 2, c);
        }
        SDL_SetRenderClipRect(r, NULL);
        if (*m->detail) {
            // Two lines at most, in the middle of the row.
            gs_wrap *d = gs_fontset_wrap(p.fonts, 11.5f * s, m->detail, (wave_w - 4) * s);
            int lines = SDL_min(2, d->count);
            layout.detail = (SDL_FRect){ left + 2, mid - (float)lines * 8, wave_w - 4, (float)lines * 16 };
            layout.detail_lines = lines;
            layout.detail_clipped = d->count > lines;
            for (int i = 0; i < lines; i++) {
                char *text = SDL_strndup(m->detail + d->lines[i].start, (size_t)((i == lines - 1 && d->count > lines ? (int)SDL_strlen(m->detail) : d->lines[i].end) - d->lines[i].start));
                text_line(&p, text, left + 2, mid - (float)lines * 8 + (float)i * 16 + 2, 11.5f, wave_w - 4, ink(&p, TEXT_DIM, 1));
                SDL_free(text);
            }
            gs_wrap_free(d);
        }
        if (m->progress) {
            float y = (mid + 16 - 1 - 3) * s;
            layout.progress = (SDL_FRect){ left, mid + 16 - 1 - 3, wave_w, 3 };
            gs_draw_round(r, (SDL_FRect){ left * s, y, wave_w * s, 3 * s }, 2 * s, ink(&p, 0xffffff, 0.12f));
            gs_draw_round(r, (SDL_FRect){ left * s, y, wave_w * s * SDL_clamp(m->fraction, 0, 1), 3 * s }, 2 * s, ink(&p, ACCENT, 1));
        }
    }
    if (wrap) gs_wrap_free(wrap);
    SDL_free(both);
    SDL_free(clean);
    SDL_free(tail);
    if (out) *out = layout;
    return needed;
}

SDL_Surface *eh_overlay_render(const eh_overlay_model *m, float scale, eh_overlay_layout *layout) {
    // Drawn again at the height the card asks for, and twice each time so every glyph is in the atlas.
    float height = EH_OVERLAY_HEIGHT;
    for (int pass = 0;; pass++) {
        int w = (int)(EH_OVERLAY_WIDTH * scale), h = (int)(height * scale);
        SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
        SDL_Renderer *r = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
        if (!r) { SDL_DestroySurface(surface); return NULL; }
        gs_glyphs *glyphs = gs_glyphs_new(r, 1024);
        float needed = height;
        for (int frame = 0; frame < 2; frame++) needed = eh_overlay_draw_with(r, glyphs, m, EH_OVERLAY_WIDTH, height, scale, 1, layout);
        SDL_RenderPresent(r);
        gs_glyphs_free(glyphs);
        SDL_DestroyRenderer(r);
        if (needed == height || pass == 2) return surface;
        SDL_DestroySurface(surface);
        height = needed;
    }
}

bool eh_overlay_shot(const eh_overlay_model *m, const char *png_path, float scale) {
    SDL_Surface *surface = eh_overlay_render(m, scale, NULL);
    bool ok = surface && stbi_write_png(png_path, surface->w, surface->h, 4, surface->pixels, surface->pitch) != 0;
    SDL_DestroySurface(surface);
    return ok;
}

/* ---------------- the window ---------------- */

#define FADE_MS 200
#define PROGRESS_HOLD_MS 400  // a finished bar stays this long, so its end is seen

static struct {
    bool enabled;           // the app has an overlay at all
    SDL_Window *window;
    SDL_Renderer *renderer;
    gs_glyphs *glyphs;
    bool wanted;            // the pipeline wants the overlay shown
    float opacity;
    Uint64 fade_from;
    float height;           // of the window now, in points
    int base_x, base_y;     // where the window of the least height sits
    bool placed;
    // The waveform's history: a column every 80 ms of recording.
    float columns[600];
    int column_count;
    float display_level;
    Uint64 column_at;
    // Progress, with the hold at its end.
    char phase[16];
    float fraction;
    Uint64 complete_at;
    // The pointer.
    int hover, pressed;
    bool card_hover, dragging;
    float drag_x, drag_y;
    int drag_win_x, drag_win_y;
    char last_status[16];
    unsigned last_sid;
    eh_overlay_layout layout;  // of the frame last drawn: where the keys are
} O;

// The window exists only while the overlay shows: with its renderer it is most of what the app
// would otherwise hold in memory while it waits in the tray.
static bool make_window(void) {
    O.height = EH_OVERLAY_HEIGHT;
    O.window = gs_overlay_window("Earheart", EH_OVERLAY_WIDTH, EH_OVERLAY_HEIGHT);
    O.renderer = O.window ? SDL_CreateRenderer(O.window, NULL) : NULL;
    if (!O.renderer) {
        SDL_Log("no overlay here (%s)", SDL_GetError());
        if (O.window) SDL_DestroyWindow(O.window);
        O.window = NULL;
        return false;
    }
    SDL_SetRenderVSync(O.renderer, 1);
    O.glyphs = gs_glyphs_new(O.renderer, 1024);
    O.wanted = false;
    O.opacity = 0;
    O.hover = O.pressed = 0;
    O.card_hover = O.dragging = false;
    return true;
}

static void drop_window(void) {
    if (O.glyphs) gs_glyphs_free(O.glyphs);
    if (O.renderer) SDL_DestroyRenderer(O.renderer);
    if (O.window) SDL_DestroyWindow(O.window);
    O.glyphs = NULL, O.renderer = NULL, O.window = NULL;
}

bool eh_overlay_init(void) {
    O.enabled = true;
    return true;
}

void eh_overlay_quit(void) {
    drop_window();
    O.enabled = false;
}

// Bottom centre of the main display, or where the user dragged it to, if that is still on a display.
static void place(void) {
    SDL_Rect area;
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &area);
    O.base_x = area.x + (area.w - EH_OVERLAY_WIDTH) / 2;
    O.base_y = area.y + area.h - EH_OVERLAY_HEIGHT - 24;
    const gs_jval *x = gs_jval_path(eh_settings(), "overlay.x"), *y = gs_jval_path(eh_settings(), "overlay.y");
    if (x && y && gs_jval_kind(x) == GS_JSON_NUMBER && gs_jval_kind(y) == GS_JSON_NUMBER) {
        int count = 0;
        SDL_DisplayID *displays = SDL_GetDisplays(&count);
        for (int i = 0; displays && i < count; i++) {
            SDL_Rect d;
            if (!SDL_GetDisplayUsableBounds(displays[i], &d)) continue;
            if (gs_jval_number(x, 0) >= d.x && gs_jval_number(y, 0) >= d.y && gs_jval_number(x, 0) + EH_OVERLAY_WIDTH <= d.x + d.w && gs_jval_number(y, 0) + EH_OVERLAY_HEIGHT <= d.y + d.h) {
                O.base_x = (int)gs_jval_number(x, 0);
                O.base_y = (int)gs_jval_number(y, 0);
            }
        }
        SDL_free(displays);
    }
    O.placed = true;
}

// The window grows upward with the transcript: its bottom edge stays where it is.
static void fit(float needed, bool moved) {
    SDL_Rect area;
    SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(O.window), &area);
    float cap = SDL_max((float)EH_OVERLAY_HEIGHT, (float)area.h - 48), target = SDL_clamp(floorf(needed + 0.5f), (float)EH_OVERLAY_HEIGHT, cap);
    if (target == O.height && !moved) return;
    O.height = target;
    int y = SDL_max(area.y, O.base_y + EH_OVERLAY_HEIGHT - (int)target);
    SDL_SetWindowSize(O.window, EH_OVERLAY_WIDTH, (int)target);
    SDL_SetWindowPosition(O.window, O.base_x, y);
}

static int key_at(float x, float y) {
    for (int i = 0; i < 3; i++) {
        SDL_FRect k = O.layout.keys[i];  // with five points of slack around each
        if (x >= k.x - 5 && x < k.x + k.w + 5 && y >= k.y - 5 && y < k.y + k.h + 5) return i + 1;
    }
    return 0;
}

static void press(void *user) {
    switch ((int)(intptr_t)user) {
    case 1: eh_pipeline_pause_toggle(); break;
    case 2: if (eh_pipeline_state() == EH_RECORDING) eh_pipeline_toggle(); break;
    case 3: eh_pipeline_discard(); break;
    }
}

static void save_position(void *user) {
    char error[256];
    gs_jval *next = gs_jval_clone(eh_settings());
    gs_jval_set_path(next, "overlay.x", gs_jval_num(O.base_x));
    gs_jval_set_path(next, "overlay.y", gs_jval_num(O.base_y));
    if (!eh_settings_save(next, error, sizeof error)) SDL_Log("could not save the overlay's position: %s", error);
}

bool eh_overlay_event(const SDL_Event *e) {
    if (!O.window) return false;
    SDL_WindowID id = SDL_GetWindowID(O.window);
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (e->motion.windowID != id) return false;
        if (O.dragging) {
            float gx, gy;
            SDL_GetGlobalMouseState(&gx, &gy);
            SDL_Rect area;
            SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(O.window), &area);
            int x = SDL_clamp(O.drag_win_x + (int)(gx - O.drag_x), area.x, area.x + area.w - EH_OVERLAY_WIDTH);
            int y = SDL_clamp(O.drag_win_y + (int)(gy - O.drag_y), area.y, area.y + area.h - (int)O.height);
            SDL_SetWindowPosition(O.window, x, y);
            O.base_x = x;
            O.base_y = y + (int)O.height - EH_OVERLAY_HEIGHT;
        } else {
            O.hover = key_at(e->motion.x, e->motion.y);
            O.card_hover = true;
        }
        return true;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (e->window.windowID != id) return false;
        O.hover = 0;
        O.card_hover = false;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.windowID != id || e->button.button != SDL_BUTTON_LEFT) return false;
        O.pressed = key_at(e->button.x, e->button.y);
        if (!O.pressed) {
            // Anywhere else on the card is the drag surface.
            O.dragging = true;
            SDL_GetGlobalMouseState(&O.drag_x, &O.drag_y);
            SDL_GetWindowPosition(O.window, &O.drag_win_x, &O.drag_win_y);
            SDL_CaptureMouse(true);
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.windowID != id || e->button.button != SDL_BUTTON_LEFT) return false;
        if (O.dragging) {
            O.dragging = false;
            SDL_CaptureMouse(false);
            eh_spawn(save_position, NULL);
        } else if (O.pressed && O.pressed == key_at(e->button.x, e->button.y)) {
            eh_spawn(press, (void *)(intptr_t)O.pressed);
        }
        O.pressed = 0;
        return true;
    default: return false;
    }
}

static void clock_text(char *out, size_t size, int seconds) { SDL_snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60); }

void eh_overlay_reset(void) {
    O.column_count = 0;
    O.column_at = 0;
    O.display_level = 0;
    O.phase[0] = O.last_status[0] = 0;
    O.complete_at = 0;
    O.last_sid = 0;
}

int eh_overlay_step(const eh_overlay_input *in, Uint64 now, eh_overlay_model *m) {
    const eh_view *v = in->view;
    *m = (eh_overlay_model){ 0 };
    SDL_strlcpy(m->status, v->status[0] ? v->status : "starting", sizeof m->status);
    bool recording = !SDL_strcmp(m->status, "recording"), paused = !SDL_strcmp(m->status, "paused");
    const gs_jval *d = v->detail;
    bool incomplete = gs_jval_boolean(gs_jval_get(d, "incomplete"), false);
    const char *note = gs_jval_string(gs_jval_get(d, "note"), ""), *method = gs_jval_string(gs_jval_get(d, "method"), "");
    // The state in a word, and the line beside it.
    if (!SDL_strcmp(m->status, "starting")) SDL_strlcpy(m->title, "Starting mic…", sizeof m->title);
    else if (recording) { SDL_strlcpy(m->detail, v->notice, sizeof m->detail); }
    else if (paused) { SDL_strlcpy(m->title, "Paused", sizeof m->title); SDL_strlcpy(m->detail, v->notice[0] ? v->notice : "Press play to resume", sizeof m->detail); }
    else if (!SDL_strcmp(m->status, "transcribing")) SDL_strlcpy(m->title, "Transcribing…", sizeof m->title);
    else if (!SDL_strcmp(m->status, "cleaning")) SDL_strlcpy(m->title, "Cleaning up…", sizeof m->title);
    else if (!SDL_strcmp(m->status, "delivering")) SDL_strlcpy(m->title, "Typing…", sizeof m->title);
    else if (!SDL_strcmp(m->status, "done")) {
        const char *delivered = *note ? "Copied — auto-paste failed" : !SDL_strcmp(method, "clipboard") ? "Copied to clipboard" : !SDL_strcmp(method, "paste-copy") ? "Pasted & copied" : "Pasted";
        SDL_snprintf(m->title, sizeof m->title, "%s%s", delivered, incomplete ? " — incomplete" : "");
        SDL_strlcpy(m->detail, *note ? note : gs_jval_string(gs_jval_get(d, "preview"), ""), sizeof m->detail);
    } else if (!SDL_strcmp(m->status, "empty")) { SDL_strlcpy(m->title, "Nothing heard", sizeof m->title); SDL_strlcpy(m->detail, "Try again closer to the mic", sizeof m->detail); }
    else if (!SDL_strcmp(m->status, "error")) { SDL_strlcpy(m->title, "Failed", sizeof m->title); SDL_strlcpy(m->detail, gs_jval_string(gs_jval_get(d, "message"), ""), sizeof m->detail); }

    // A new dictation starts with an empty waveform; a new phase with an empty bar, unless the
    // bar it follows is still showing its end. Progress belongs to the phase it was reported for.
    if (v->sid != O.last_sid) { O.last_sid = v->sid; O.column_count = 0; O.column_at = 0; O.display_level = 0; }
    if (SDL_strcmp(m->status, O.last_status)) {
        bool holding = O.complete_at && now - O.complete_at < PROGRESS_HOLD_MS;
        bool keeps = !SDL_strcmp(m->status, "transcribing") || !SDL_strcmp(m->status, "cleaning") || !SDL_strcmp(m->status, "delivering");
        if (!(holding && keeps)) { O.phase[0] = 0; O.complete_at = 0; }
        SDL_strlcpy(O.last_status, m->status, sizeof O.last_status);
    }
    if (v->phase[0] && !SDL_strcmp(v->phase, m->status)) {
        SDL_strlcpy(O.phase, m->status, sizeof O.phase);
        O.fraction = v->fraction;
        if (v->fraction >= 1 && !O.complete_at) O.complete_at = now;
        if (v->fraction < 1) O.complete_at = 0;
    }
    if (O.complete_at && now - O.complete_at >= PROGRESS_HOLD_MS) { O.phase[0] = 0; O.complete_at = 0; }
    m->progress = O.phase[0] != 0;
    m->fraction = O.fraction;

    // The timer counts captured audio only; in the last minute it counts down to the cap.
    int left = in->cap > 0 ? (int)ceil((double)in->cap - in->captured) : 0;
    bool warned = in->cap > 0 && (recording || paused) && (double)in->cap - in->captured <= 60;
    if (warned) SDL_snprintf(m->timer, sizeof m->timer, "−%d:%02d", SDL_max(0, left) / 60, SDL_max(0, left) % 60);
    else clock_text(m->timer, sizeof m->timer, (int)in->captured);
    m->cap_warning = warned;
    if (recording) SDL_strlcpy(m->title, warned ? "Stopping soon…" : "Listening…", sizeof m->title);

    // The waveform: the latest level eased, and written as a column every 80 ms while recording.
    // Paused, the columns hold where they are.
    O.display_level += (in->level - O.display_level) * 0.18f;
    if (recording) {
        if (!O.column_at) O.column_at = now;
        while (now - O.column_at >= WAVE_PUSH_MS) {
            SDL_memmove(O.columns + 1, O.columns, sizeof(float) * (SDL_arraysize(O.columns) - 1));
            O.columns[0] = O.display_level;
            if (O.column_count < (int)SDL_arraysize(O.columns)) O.column_count++;
            O.column_at += WAVE_PUSH_MS;
        }
        m->column_fraction = (float)(now - O.column_at) / WAVE_PUSH_MS;
    } else {
        O.column_at = 0;
    }
    SDL_memcpy(m->levels, O.columns, sizeof m->levels);
    m->level_count = O.column_count;
    m->live_level = O.display_level;
    m->clean = v->cleaned;
    m->raw = v->raw;
    m->seconds = (double)now / 1000;

    // Things that move want the next frame at once; a still card is drawn again when asked.
    bool busy = !SDL_strcmp(m->status, "transcribing") || !SDL_strcmp(m->status, "cleaning") || !SDL_strcmp(m->status, "delivering");
    if (recording || busy || !SDL_strcmp(m->status, "starting") || O.complete_at) return 16;
    return paused ? 250 : -1;
}

int eh_overlay_frame(void) {
    if (!O.enabled) return -1;
    if (!O.window) {
        eh_lock();
        bool wanted = eh_app_view()->visible;
        eh_unlock();
        if (!wanted) return -1;
        if (!make_window()) { O.enabled = false; return -1; }
    }
    Uint64 now = SDL_GetTicks();
    eh_overlay_model m;

    // The app's state, read under the lock; the drawing then needs nothing from it.
    eh_lock();
    float levels[24];
    int level_count = eh_recorder_levels(levels, 24);
    eh_overlay_input in = { eh_app_view(), eh_recorder_seconds(), eh_recorder_cap_seconds(), level_count ? levels[level_count - 1] : 0 };
    bool wanted = in.view->visible;
    int again = eh_overlay_step(&in, now, &m);
    char *clean = m.clean ? SDL_strdup(m.clean) : NULL, *raw = m.raw ? SDL_strdup(m.raw) : NULL;
    eh_unlock();
    m.clean = clean;
    m.raw = raw;
    m.hover = O.hover;
    m.pressed = O.pressed;
    m.card_hover = O.card_hover;

    // Shown and hidden with a short fade.
    if (wanted != O.wanted) {
        O.wanted = wanted;
        O.fade_from = now;
        if (wanted) {
            if (!O.placed) place();
            fit(O.height, true);
            gs_overlay_show(O.window, true);  // on every workspace, and over full-screen apps on macOS
        }
    }
    float t = SDL_min(1, (float)(now - O.fade_from) / FADE_MS);
    O.opacity = O.wanted ? SDL_max(O.opacity, t) : SDL_min(O.opacity, 1 - t);
    bool fading = O.wanted ? O.opacity < 1 : O.opacity > 0;
    if (!O.wanted && O.opacity <= 0) {
        drop_window();
        again = -1;
    } else {
        float scale = SDL_GetWindowDisplayScale(O.window);
        if (scale <= 0) scale = 1;
        float needed = eh_overlay_draw_with(O.renderer, O.glyphs, &m, EH_OVERLAY_WIDTH, O.height, scale, O.opacity, &O.layout);
        SDL_RenderPresent(O.renderer);
        fit(needed, false);
        if (fading || gs_glyphs_waiting(O.glyphs)) again = 16;
    }
    SDL_free(clean);
    SDL_free(raw);
    return again;
}

float eh_overlay_draw(SDL_Renderer *r, const eh_overlay_model *m, float width, float height, float scale, float opacity) {
    static gs_glyphs *glyphs;
    static SDL_Renderer *owner;
    if (owner != r) { owner = r; glyphs = gs_glyphs_new(r, 1024); }
    return eh_overlay_draw_with(r, glyphs, m, width, height, scale, opacity, NULL);
}
