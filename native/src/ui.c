#include "ui.h"
#include "fonts.h"
#include "gs_draw.h"
#include "icon_app.h"
#include "stb_image.h"

#define PAGE "page"
#define SEEN 256

typedef struct { char name[56]; gs_rect r; bool paged; } seen;

struct eh_ui {
    SDL_Window *win;
    SDL_Renderer *r;
    gs_ui *gs;
    SDL_Texture *icon;
    bool close_asked, inert, dim;
    float mx, my;  // the pointer, in points
    bool down;     // its left button
    bool escape;   // Escape was pressed since the last frame
    bool input;    // something was pressed, released, typed or scrolled since the last frame
    bool paging;   // between eh_scroll_begin and eh_scroll_end
    bool was_capturing;  // a hotkey field was waiting for keys when the frame began: Escape was its
    // The controls the frame drew, for tests to find them by name.
    seen drawn[SEEN];
    int count;
    // The question.
    char question[256], yes[32], no[32];
    bool asking, dangerous, asked_now;
};

void eh_ui_again(eh_ui *ui, int ms) { gs_ui_wake(ui->gs, ms); }

eh_ui *eh_ui_new(const char *title, int w, int h, int min_w, int min_h) {
    eh_ui *ui = SDL_calloc(1, sizeof *ui);
    ui->win = SDL_CreateWindow(title, w, h, SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | (min_w ? SDL_WINDOW_RESIZABLE : 0));
    ui->r = ui->win ? SDL_CreateRenderer(ui->win, NULL) : NULL;
    if (!ui->r) {
        SDL_Log("no window here (%s)", SDL_GetError());
        if (ui->win) SDL_DestroyWindow(ui->win);
        SDL_free(ui);
        return NULL;
    }
    if (min_w) SDL_SetWindowMinimumSize(ui->win, min_w, min_h);
    // The app's icon: the window's, and a texture for the header.
    int iw, ih, n;
    unsigned char *pixels = stbi_load_from_memory(icon_app, sizeof icon_app, &iw, &ih, &n, 4);
    SDL_Surface *icon = pixels ? SDL_CreateSurfaceFrom(iw, ih, SDL_PIXELFORMAT_RGBA32, pixels, iw * 4) : NULL;
    if (icon) {
        SDL_SetWindowIcon(ui->win, icon);
        ui->icon = SDL_CreateTextureFromSurface(ui->r, icon);
        if (ui->icon) SDL_SetTextureScaleMode(ui->icon, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(icon);
    }
    stbi_image_free(pixels);
    SDL_SetRenderVSync(ui->r, 1);
    ui->gs = gs_ui_new(ui->win, ui->r, eh_fonts());
    gs_ui_style *s = gs_ui_style_of(ui->gs);
    s->background = gs_rgba(EH_INK, 1);
    s->panel = gs_rgba(0x232327, 1);
    s->raised = gs_rgba(EH_FIELD, 1);
    s->text = gs_rgba(EH_TEXT, 1);
    s->muted = gs_rgba(EH_TEXT_DIM, 1);
    s->accent = gs_rgba(EH_TEXT, 1);  // a field with the keyboard has a white edge
    s->accent_text = gs_rgba(EH_INK, 1);
    s->border = gs_rgba(EH_FIELD_EDGE, 1);
    s->warning = gs_rgba(EH_ERR, 1);
    s->focus = gs_rgba(EH_TEXT, 0.9f);
    s->radius = 8;
    s->text_px = 13;
    s->small_px = 12;
    s->row = 32;
    return ui;
}

void eh_ui_free(eh_ui *ui) {
    if (!ui) return;
    if (ui->icon) SDL_DestroyTexture(ui->icon);
    gs_ui_free(ui->gs);
    SDL_DestroyRenderer(ui->r);
    SDL_DestroyWindow(ui->win);
    SDL_free(ui);
}

SDL_Window *eh_ui_window(eh_ui *ui) { return ui->win; }
gs_ui *eh_ui_gs(eh_ui *ui) { return ui->gs; }
bool eh_ui_capturing(eh_ui *ui) { return gs_ui_capturing(ui->gs); }
bool eh_ui_asking(eh_ui *ui) { return ui->asking; }
bool eh_ui_escaped(eh_ui *ui) { return ui->escape; }
float eh_ui_pointer_x(eh_ui *ui) { return ui->mx; }
float eh_ui_pointer_y(eh_ui *ui) { return ui->my; }
bool eh_ui_pointer_down(eh_ui *ui) { return ui->down; }

bool eh_ui_close_asked(eh_ui *ui) {
    bool asked = ui->close_asked;
    ui->close_asked = false;
    return asked;
}

// Whether the event is this window's, and what of it the layer itself keeps.
static bool mine(eh_ui *ui, const SDL_Event *e) {
    SDL_WindowID id = SDL_GetWindowID(ui->win);
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e->window.windowID != id) return false;
        ui->close_asked = true;
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        if (e->motion.windowID != id) return false;
        ui->mx = e->motion.x, ui->my = e->motion.y;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.windowID != id) return false;
        ui->mx = e->button.x, ui->my = e->button.y;
        if (e->button.button == SDL_BUTTON_LEFT) ui->down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        return true;
    case SDL_EVENT_MOUSE_WHEEL: return e->wheel.windowID == id;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.windowID != id) return false;
        if (e->key.key == SDLK_ESCAPE) ui->escape = true;
        return true;
    case SDL_EVENT_KEY_UP: return e->key.windowID == id;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == id;
    case SDL_EVENT_TEXT_EDITING: return e->edit.windowID == id;
    default: return e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST && e->window.windowID == id;
    }
}

bool eh_ui_event(eh_ui *ui, const SDL_Event *e) {
    if (!mine(ui, e)) return false;
    if (e->type != SDL_EVENT_MOUSE_MOTION) ui->input = true;
    gs_ui_event(ui->gs, e);
    return true;
}

void eh_ui_focus(eh_ui *ui, const char *id) { gs_ui_focus(ui->gs, id); }
bool eh_ui_focused(eh_ui *ui, const char *id) { return gs_ui_focused(ui->gs, id); }
int eh_ui_stops(eh_ui *ui) { return ui->count; }
const char *eh_ui_stop_name(eh_ui *ui, int index) { return index >= 0 && index < ui->count ? ui->drawn[index].name : ""; }

static seen *find(eh_ui *ui, const char *id) {
    for (int i = 0; i < ui->count; i++)
        if (!SDL_strcmp(ui->drawn[i].name, id)) return &ui->drawn[i];
    return NULL;
}

bool eh_ui_stop_rect(eh_ui *ui, const char *id, gs_rect *r) {
    seen *s = find(ui, id);
    if (s) *r = s->r;
    return s != NULL;
}

bool eh_ui_stop_paged(eh_ui *ui, const char *id) {
    seen *s = find(ui, id);
    return s && s->paged;
}

const char *eh_ui_focus_name(eh_ui *ui) {
    for (int i = 0; i < ui->count; i++)
        if (gs_ui_focused(ui->gs, ui->drawn[i].name)) return ui->drawn[i].name;
    return "";
}

// A control that can be used is noted, by its name and where it is.
static void note(eh_ui *ui, const char *id, gs_rect r) {
    if (ui->inert || ui->asking || ui->count >= SEEN) return;
    seen *s = &ui->drawn[ui->count++];
    SDL_strlcpy(s->name, id, sizeof s->name);
    s->r = r;
    s->paged = ui->paging;
}

void eh_ui_inert(eh_ui *ui, bool inert, bool dim) {
    ui->inert = inert;
    ui->dim = dim;
    gs_ui_disable(ui->gs, inert, dim);
}

SDL_FColor eh_ui_ink(eh_ui *ui, unsigned rgb, float alpha) { return gs_rgba(rgb, alpha); }

gs_rect eh_ui_begin(eh_ui *ui) {
    ui->was_capturing = gs_ui_capturing(ui->gs);
    gs_rect area = gs_ui_begin(ui->gs);
    ui->inert = ui->dim = false;
    ui->asked_now = false;
    ui->count = 0;
    SDL_SetRenderDrawColorFloat(ui->r, 0x18 / 255.0f, 0x18 / 255.0f, 0x1b / 255.0f, 1);
    SDL_RenderClear(ui->r);
    return area;
}

int eh_ui_end(eh_ui *ui) {
    // Escape that no field, question or capture took closes the window.
    if (gs_ui_key(ui->gs, SDLK_ESCAPE, 0) && !ui->asking && !ui->was_capturing) ui->close_asked = true;
    ui->escape = false;
    ui->paging = false;
    // What an input changed shows on the frame after the one that took it.
    if (ui->input) gs_ui_wake(ui->gs, 0);
    ui->input = false;
    gs_ui_end(ui->gs);
    SDL_RenderPresent(ui->r);
    return gs_ui_wait_ms(ui->gs);
}

/* ---------------- the page ---------------- */

float eh_scroll_begin(eh_ui *ui, gs_rect area, float content_h) {
    ui->paging = true;
    return gs_ui_page_begin(ui->gs, area, PAGE).y;
}

void eh_scroll_end(eh_ui *ui, float bottom) {
    gs_ui_page_end(ui->gs, bottom);
    ui->paging = false;
}

void eh_scroll_reclip(eh_ui *ui) {}
float eh_scroll_y(eh_ui *ui) { return gs_ui_page_scroll(ui->gs, PAGE); }
void eh_scroll_to(eh_ui *ui, float y, bool glide) { gs_ui_page_scroll_to(ui->gs, PAGE, y, glide); }

/* ---------------- drawing ---------------- */

void eh_w_round(eh_ui *ui, gs_rect r, float radius, SDL_FColor fill) { gs_ui_round(ui->gs, r, radius, fill); }
void eh_w_outline(eh_ui *ui, gs_rect r, float radius, SDL_FColor c) { gs_ui_outline(ui->gs, r, radius, 1, c); }

void eh_w_icon(eh_ui *ui, gs_rect r) {
    if (ui->icon) gs_ui_texture(ui->gs, r, ui->icon, NULL);
}

void eh_w_line(eh_ui *ui, gs_rect r, const char *text, float px, SDL_FColor c, int align) { gs_ui_text(ui->gs, r, text, px, c, align); }
float eh_w_text(eh_ui *ui, gs_rect r, const char *text, float px, SDL_FColor c) { return gs_ui_text_wrapped(ui->gs, r, text, px, c); }
float eh_w_text_height(eh_ui *ui, const char *text, float px, float width) { return gs_ui_text_height(ui->gs, text, px, width); }

/* ---------------- controls ---------------- */

float eh_w_button_width(eh_ui *ui, const char *label) { return gs_ui_text_width(ui->gs, label, 13) + 28; }

// The buttons are the design's own; gs_ui gives them their place in the Tab order, the click, the
// keys and the focus mark.
bool eh_w_button(eh_ui *ui, gs_rect r, const char *id, const char *label, eh_btn kind) {
    note(ui, id, r);
    bool hit = gs_ui_activated(ui->gs, r, id), hot = gs_ui_hovered(ui->gs, r);
    bool nav = kind == EH_BTN_NAV || kind == EH_BTN_NAV_ON, faded = ui->inert && !ui->dim;
    float radius = nav ? 8 : r.h / 2;
    SDL_FColor text = gs_rgba(EH_TEXT, faded ? 0.5f : 1);
    if (kind == EH_BTN_PRIMARY) {
        gs_ui_round(ui->gs, r, radius, gs_rgba(hot ? 0xffffff : EH_TEXT, faded ? 0.5f : 1));
        text = gs_rgba(EH_INK, 1);
    } else if (nav) {
        if (kind == EH_BTN_NAV_ON || hot) gs_ui_round(ui->gs, r, radius, kind == EH_BTN_NAV_ON ? EH_WASH : EH_WASH_FAINT);
        if (kind != EH_BTN_NAV_ON && !hot) text = gs_rgba(EH_TEXT_DIM, 1);
    } else {
        gs_ui_round(ui->gs, r, radius, kind == EH_BTN_DANGER && hot ? gs_rgba(EH_ERR, 0.14f) : gs_rgba(0xffffff, hot ? 0.14f : 0.08f));
        if (kind == EH_BTN_DANGER && hot) text = gs_rgba(EH_ERR, 1);
    }
    if (nav) gs_ui_text(ui->gs, (gs_rect){ r.x + 12, r.y, r.w - 20, r.h }, label, 13, text, -1);
    else gs_ui_text(ui->gs, gs_inset(r, 4), label, 13, text, 0);
    gs_ui_focus_mark(ui->gs, r, id, radius);
    return hit;
}

bool eh_w_switch(eh_ui *ui, gs_rect r, const char *id, bool *on) {
    note(ui, id, r);
    return gs_ui_switch(ui->gs, r, id, on);
}

bool eh_w_radio(eh_ui *ui, gs_rect r, const char *id, const char *label, bool selected) {
    note(ui, id, r);
    return gs_ui_radio(ui->gs, r, id, label, selected);
}

bool eh_w_select(eh_ui *ui, gs_rect r, const char *id, const char *const *items, int count, int *selected) {
    note(ui, id, r);
    return gs_ui_dropdown(ui->gs, r, id, items, count, selected);
}

bool eh_w_field(eh_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder, int flags) {
    note(ui, id, r);
    return gs_ui_input(ui->gs, r, id, buf, size, placeholder, GS_UI_KEEP | (flags & EH_FIELD_MASKED ? GS_UI_MASKED | GS_UI_REVEAL : 0));
}

bool eh_w_textarea(eh_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder) {
    note(ui, id, r);
    return gs_ui_textarea(ui->gs, r, id, buf, size, placeholder);
}

bool eh_w_stops(eh_ui *ui, gs_rect r, const char *id, int *value, int count) {
    note(ui, id, r);
    return gs_ui_stops(ui->gs, r, id, value, count);
}

bool eh_w_hotkey(eh_ui *ui, gs_rect r, const char *id, const char *shown, const char *placeholder, char *out, size_t size) {
    note(ui, id, r);
    return gs_ui_hotkey(ui->gs, r, id, shown, placeholder, out, size);
}

bool eh_ui_custom_stop(eh_ui *ui, const char *id, gs_rect r, int *arrow, bool *shift, bool *activated) {
    note(ui, id, r);
    bool hit = gs_ui_activated(ui->gs, r, id), focused = gs_ui_focused(ui->gs, id) && !ui->inert && !ui->asking;
    int a = 0;
    bool s = false;
    if (focused) {
        // Shift is asked for first: a plain press matches with or without it.
        if (gs_ui_key(ui->gs, SDLK_RIGHT, SDL_KMOD_SHIFT) || gs_ui_key(ui->gs, SDLK_DOWN, SDL_KMOD_SHIFT)) a = 1, s = true;
        else if (gs_ui_key(ui->gs, SDLK_LEFT, SDL_KMOD_SHIFT) || gs_ui_key(ui->gs, SDLK_UP, SDL_KMOD_SHIFT)) a = -1, s = true;
        else if (gs_ui_key(ui->gs, SDLK_RIGHT, 0) || gs_ui_key(ui->gs, SDLK_DOWN, 0)) a = 1;
        else if (gs_ui_key(ui->gs, SDLK_LEFT, 0) || gs_ui_key(ui->gs, SDLK_UP, 0)) a = -1;
    }
    if (arrow) *arrow = a;
    if (shift) *shift = s;
    if (activated) *activated = hit;
    return focused;
}

/* ---------------- the question ---------------- */

void eh_ui_ask(eh_ui *ui, const char *question, const char *yes, const char *no, bool dangerous) {
    SDL_strlcpy(ui->question, question, sizeof ui->question);
    SDL_strlcpy(ui->yes, yes, sizeof ui->yes);
    SDL_strlcpy(ui->no, no, sizeof ui->no);
    ui->asking = true;
    ui->dangerous = dangerous;
    ui->asked_now = true;  // the click that asked is not an answer
    gs_ui_wake(ui->gs, 0);
}

int eh_ui_modal(eh_ui *ui) {
    if (!ui->asking || ui->asked_now) return 0;
    int answer = gs_ui_confirm(ui->gs, ui->question, ui->yes, ui->no, ui->dangerous);
    if (answer) ui->asking = false;
    return answer > 0 ? 1 : answer < 0 ? 2 : 0;
}
