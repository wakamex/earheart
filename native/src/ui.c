#include "ui.h"
#include <math.h>
#include "draw.h"
#include "icon_app.h"
#include "stb_image.h"

#define ORDER 256
#define FOCUS_RING 2.0f

typedef struct { Uint32 id; char name[56]; gs_rect r; bool field, paged; } stop;  // paged: on the scrolling page

struct eh_ui {
    SDL_Window *win;
    SDL_Renderer *r;
    gs_ui *gs;
    SDL_Texture *icon;
    bool close_asked, inert, dim;
    float mx, my;            // the pointer, in points
    float press_x, press_y;  // where the left button went down
    bool down, escape;
    bool input;              // something was pressed, released, typed or scrolled since the last frame
    // The Tab order: the stops of the frame being drawn, and of the one before it, which Tab walks.
    stop order[ORDER], last[ORDER];
    int count, last_count;
    Uint32 focus;
    bool by_keyboard;        // the focus came from the keyboard, so it is marked
    bool activate;           // Space or Enter this frame, for the control with the focus
    int arrow;               // Up or Left: -1; Down or Right: 1
    // The page.
    bool scrolling;
    gs_rect view;
    float scroll, target, content, wheel;
    bool gliding;
    // The hotkey field that is waiting for keys, and what was pressed for it.
    Uint32 capturing;
    char captured[64];
    bool capture_ended;
    // The question.
    char question[256], yes[32];
    bool asking, dangerous, asked_now;
    int again;               // milliseconds until a frame is wanted for an animation
};

static Uint32 hash(const char *s) {
    Uint32 h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h ? h : 1;
}

static void want_frame(eh_ui *ui, int ms) {
    if (ui->again < 0 || ms < ui->again) ui->again = ms;
}

void eh_ui_again(eh_ui *ui, int ms) { want_frame(ui, ms); }

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
    s->background = eh_rgba(EH_INK, 1);
    s->panel = eh_rgba(0x232327, 1);
    s->raised = eh_rgba(EH_FIELD, 1);
    s->text = eh_rgba(EH_TEXT, 1);
    s->muted = eh_rgba(EH_TEXT_DIM, 1);
    s->accent = eh_rgba(EH_TEXT, 1);  // a field with the keyboard has a white edge
    s->accent_text = eh_rgba(EH_INK, 1);
    s->border = eh_rgba(EH_FIELD_EDGE, 1);
    s->warning = eh_rgba(EH_ERR, 1);
    s->text_px = 13;
    s->small_px = 12;
    s->row = 32;
    ui->again = -1;
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
bool eh_ui_capturing(eh_ui *ui) { return ui->capturing != 0; }
bool eh_ui_asking(eh_ui *ui) { return ui->asking; }

bool eh_ui_close_asked(eh_ui *ui) {
    bool asked = ui->close_asked;
    ui->close_asked = false;
    return asked;
}

// Electron's name for a key, or NULL for one that cannot be a hotkey by itself.
static const char *accelerator_key(const SDL_KeyboardEvent *k, char *buf, size_t size) {
    SDL_Scancode sc = k->scancode;
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { SDL_snprintf(buf, size, "%c", 'A' + (int)(sc - SDL_SCANCODE_A)); return buf; }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) { SDL_snprintf(buf, size, "%c", '1' + (int)(sc - SDL_SCANCODE_1)); return buf; }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) { SDL_snprintf(buf, size, "num%c", '1' + (int)(sc - SDL_SCANCODE_KP_1)); return buf; }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) { SDL_snprintf(buf, size, "F%d", 1 + (int)(sc - SDL_SCANCODE_F1)); return buf; }
    if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24) { SDL_snprintf(buf, size, "F%d", 13 + (int)(sc - SDL_SCANCODE_F13)); return buf; }
    switch (sc) {
    case SDL_SCANCODE_0: return "0";
    case SDL_SCANCODE_KP_0: return "num0";
    case SDL_SCANCODE_SPACE: return "Space";
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return "Enter";
    case SDL_SCANCODE_KP_PERIOD: return "numdec";
    case SDL_SCANCODE_KP_PLUS: return "numadd";
    case SDL_SCANCODE_KP_MINUS: return "numsub";
    case SDL_SCANCODE_KP_MULTIPLY: return "nummult";
    case SDL_SCANCODE_KP_DIVIDE: return "numdiv";
    case SDL_SCANCODE_SEMICOLON: return ";";
    case SDL_SCANCODE_APOSTROPHE: return "'";
    case SDL_SCANCODE_COMMA: return ",";
    case SDL_SCANCODE_PERIOD: return ".";
    case SDL_SCANCODE_SLASH: return "/";
    case SDL_SCANCODE_BACKSLASH: return "\\";
    case SDL_SCANCODE_LEFTBRACKET: return "[";
    case SDL_SCANCODE_RIGHTBRACKET: return "]";
    case SDL_SCANCODE_GRAVE: return "`";
    case SDL_SCANCODE_MINUS: return "-";
    case SDL_SCANCODE_EQUALS: return "=";
    case SDL_SCANCODE_UP: return "Up";
    case SDL_SCANCODE_DOWN: return "Down";
    case SDL_SCANCODE_LEFT: return "Left";
    case SDL_SCANCODE_RIGHT: return "Right";
    case SDL_SCANCODE_HOME: return "Home";
    case SDL_SCANCODE_END: return "End";
    case SDL_SCANCODE_PAGEUP: return "PageUp";
    case SDL_SCANCODE_PAGEDOWN: return "PageDown";
    case SDL_SCANCODE_INSERT: return "Insert";
    case SDL_SCANCODE_DELETE: return "Delete";
    case SDL_SCANCODE_BACKSPACE: return "Backspace";
    default: return NULL;
    }
}

bool eh_accelerator_from_key(const SDL_KeyboardEvent *k, char *out, size_t size) {
    char key[16];
    const char *name = accelerator_key(k, key, sizeof key);
    bool ctrl = k->mod & SDL_KMOD_CTRL, gui = k->mod & SDL_KMOD_GUI, alt = k->mod & SDL_KMOD_ALT, shift = k->mod & SDL_KMOD_SHIFT;
    if (!name || !(ctrl || gui || alt || shift)) return false;  // at least one modifier
#ifdef __APPLE__
    const char *ctrl_name = "Control+", *gui_name = "Command+";
#else
    const char *ctrl_name = "CommandOrControl+", *gui_name = "Super+";
#endif
    SDL_snprintf(out, size, "%s%s%s%s%s", ctrl ? ctrl_name : "", gui ? gui_name : "", alt ? "Alt+" : "", shift ? "Shift+" : "", name);
    return true;
}

static bool is_field_focused(eh_ui *ui) {
    for (int i = 0; i < ui->last_count; i++)
        if (ui->last[i].id == ui->focus) return ui->last[i].field;
    return false;
}

static bool take_event(eh_ui *ui, const SDL_Event *e) {
    SDL_WindowID id = SDL_GetWindowID(ui->win);
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e->window.windowID != id) return false;
        ui->close_asked = true;
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (e->window.windowID != id) return false;
        if (ui->capturing) ui->capture_ended = true;
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        if (e->motion.windowID != id) return false;
        ui->mx = e->motion.x, ui->my = e->motion.y;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.windowID != id) return false;
        ui->mx = e->button.x, ui->my = e->button.y;
        if (e->button.button == SDL_BUTTON_LEFT) {
            ui->down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (ui->down) ui->press_x = ui->mx, ui->press_y = ui->my, ui->by_keyboard = false;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (e->wheel.windowID != id) return false;
        ui->wheel += e->wheel.y;
        break;
    case SDL_EVENT_KEY_DOWN: {
        if (e->key.windowID != id) return false;
        SDL_Keycode k = e->key.key;
        if (ui->capturing) {
            // Everything but Tab is the field's: Escape ends the capture, a combination fills it.
            if (k == SDLK_TAB) { ui->capture_ended = true; break; }
            if (k == SDLK_ESCAPE) ui->capture_ended = true;
            else eh_accelerator_from_key(&e->key, ui->captured, sizeof ui->captured);
            return true;
        }
        if (k == SDLK_ESCAPE) {
            // gs_ui's fields empty themselves on Escape, which a settings field must not.
            if (ui->asking || is_field_focused(ui)) { ui->escape = true; return true; }
            ui->close_asked = true;
            return true;
        }
        bool field = is_field_focused(ui);
        if (!field && (k == SDLK_SPACE || k == SDLK_RETURN || k == SDLK_KP_ENTER)) { ui->activate = true; return true; }
        if (!field && (k == SDLK_UP || k == SDLK_LEFT)) ui->arrow = -1;
        if (!field && (k == SDLK_DOWN || k == SDLK_RIGHT)) ui->arrow = 1;
        break;
    }
    case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
        if (e->key.windowID != id) return false;
        if (ui->capturing) return true;
        break;
    case SDL_EVENT_WINDOW_EXPOSED: case SDL_EVENT_WINDOW_RESIZED: case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_FOCUS_GAINED: case SDL_EVENT_WINDOW_MOUSE_LEAVE: case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        return e->window.windowID == id;
    default: return false;
    }
    gs_ui_event(ui->gs, e);
    return true;
}

bool eh_ui_event(eh_ui *ui, const SDL_Event *e) {
    bool mine = take_event(ui, e);
    if (mine && e->type != SDL_EVENT_MOUSE_MOTION) ui->input = true;
    return mine;
}

void eh_ui_focus(eh_ui *ui, const char *id) {
    ui->focus = id ? hash(id) : 0;
    gs_ui_focus(ui->gs, NULL);
}

bool eh_ui_focused(eh_ui *ui, const char *id) { return ui->focus == hash(id); }
int eh_ui_stops(eh_ui *ui) { return ui->count; }
const char *eh_ui_stop_name(eh_ui *ui, int index) { return index >= 0 && index < ui->count ? ui->order[index].name : ""; }

bool eh_ui_stop_rect(eh_ui *ui, const char *id, gs_rect *r) {
    Uint32 h = hash(id);
    for (int i = 0; i < ui->count; i++)
        if (ui->order[i].id == h) { *r = ui->order[i].r; return true; }
    return false;
}

const char *eh_ui_focus_name(eh_ui *ui) {
    for (int i = 0; i < ui->count; i++)
        if (ui->order[i].id == ui->focus) return ui->order[i].name;
    return "";
}

void eh_ui_inert(eh_ui *ui, bool inert, bool dim) {
    ui->inert = inert || ui->asking;
    ui->dim = dim;
}

SDL_FColor eh_ui_ink(eh_ui *ui, unsigned rgb, float alpha) { return eh_rgba(rgb, ui->dim ? alpha * 0.45f : alpha); }

gs_rect eh_ui_begin(eh_ui *ui) {
    gs_rect area = gs_ui_begin(ui->gs);
    ui->again = -1;
    ui->inert = ui->asking;
    ui->dim = false;
    ui->asked_now = false;
    // Tab walks the stops the last frame drew.
    int step = gs_ui_key(ui->gs, SDLK_TAB, SDL_KMOD_SHIFT) ? -1 : gs_ui_key(ui->gs, SDLK_TAB, 0) ? 1 : 0;
    if (ui->escape && !ui->asking) gs_ui_focus(ui->gs, NULL);  // with a field focused: the field lets go
    SDL_memcpy(ui->last, ui->order, sizeof(stop) * (size_t)ui->count);
    ui->last_count = ui->count;
    ui->count = 0;
    if (step && ui->last_count) {
        int at = -1;
        for (int i = 0; i < ui->last_count; i++)
            if (ui->last[i].id == ui->focus) at = i;
        at = at < 0 ? (step > 0 ? 0 : ui->last_count - 1) : (at + step + ui->last_count) % ui->last_count;
        const stop *s = &ui->last[at];
        ui->focus = s->id;
        ui->by_keyboard = true;
        gs_ui_focus(ui->gs, s->field ? s->name : NULL);
        // Brought into view, with room around it.
        if (s->paged && (s->r.y < ui->view.y + 12 || s->r.y + s->r.h > ui->view.y + ui->view.h - 12)) {
            float mid = s->r.y + s->r.h / 2 - (ui->view.y + ui->view.h / 2);
            eh_scroll_to(ui, ui->scroll + mid, false);
        }
    }
    SDL_SetRenderDrawColorFloat(ui->r, 0x18 / 255.0f, 0x18 / 255.0f, 0x1b / 255.0f, 1);
    SDL_RenderClear(ui->r);
    return area;
}

int eh_ui_end(eh_ui *ui) {
    // A focus that no stop of this frame has is dropped, so Tab starts over from the top.
    bool found = false;
    for (int i = 0; i < ui->count; i++) found = found || ui->order[i].id == ui->focus;
    if (!found && ui->focus && !ui->asking) ui->focus = 0;
    ui->activate = false;
    ui->escape = false;
    ui->arrow = 0;
    ui->wheel = 0;
    ui->scrolling = false;
    // What an input changed shows on the frame after the one that took it.
    if (ui->input) want_frame(ui, 0);
    ui->input = false;
    gs_ui_end(ui->gs);
    SDL_RenderPresent(ui->r);
    int gs = gs_ui_wait_ms(ui->gs);
    return ui->again < 0 ? gs : gs < 0 ? ui->again : SDL_min(gs, ui->again);
}

/* ---------------- the page ---------------- */

float eh_scroll_begin(eh_ui *ui, gs_rect area, float content_h) {
    float max = SDL_max(0, content_h - area.h);
    ui->view = area;
    ui->content = content_h;
    ui->scrolling = true;
    if (ui->wheel != 0 && gs_rect_contains(area, ui->mx, ui->my) && !ui->asking) {
        ui->scroll -= ui->wheel * 48;
        ui->gliding = false;
    }
    if (ui->gliding) {
        float d = SDL_clamp(ui->target, 0, max) - ui->scroll;
        if (SDL_fabsf(d) < 0.5f) ui->scroll += d, ui->gliding = false;
        else ui->scroll += d * 0.28f, want_frame(ui, 16);
    }
    ui->scroll = SDL_clamp(ui->scroll, 0, max);
    gs_ui_clip(ui->gs, &area);
    return area.y - floorf(ui->scroll);
}

void eh_scroll_end(eh_ui *ui) {
    gs_ui_clip(ui->gs, NULL);
    gs_rect v = ui->view;
    if (ui->content > v.h + 1) {
        float h = SDL_max(28, v.h * v.h / ui->content), y = v.y + (v.h - h) * ui->scroll / (ui->content - v.h);
        eh_w_round(ui, (gs_rect){ v.x + v.w - 7, y, 4, h }, 2, eh_rgba(0xffffff, 0.22f));
    }
    ui->scrolling = false;
}

void eh_scroll_reclip(eh_ui *ui) { gs_ui_clip(ui->gs, ui->scrolling ? &ui->view : NULL); }
float eh_scroll_y(eh_ui *ui) { return ui->scroll; }

void eh_scroll_to(eh_ui *ui, float y, bool glide) {
    if (glide) ui->target = y, ui->gliding = true, want_frame(ui, 16);
    else ui->scroll = y, ui->gliding = false;
}

/* ---------------- drawing ---------------- */

void eh_w_round(eh_ui *ui, gs_rect r, float radius, SDL_FColor fill) {
    float s = gs_ui_scale(ui->gs);
    eh_fill_round(ui->r, r.x * s, r.y * s, r.w * s, r.h * s, radius * s, fill);
}

void eh_w_outline(eh_ui *ui, gs_rect r, float radius, SDL_FColor c) {
    float s = gs_ui_scale(ui->gs);
    eh_stroke_round(ui->r, r.x * s, r.y * s, r.w * s, r.h * s, radius * s, SDL_max(1, floorf(s)), c);
}

void eh_w_icon(eh_ui *ui, gs_rect r) {
    if (ui->icon) gs_ui_texture(ui->gs, r, ui->icon, NULL);
}

void eh_w_line(eh_ui *ui, gs_rect r, const char *text, float px, SDL_FColor c, int align) { gs_ui_text(ui->gs, r, text, px, c, align); }
float eh_w_text(eh_ui *ui, gs_rect r, const char *text, float px, SDL_FColor c) { return gs_ui_text_wrapped(ui->gs, r, text, px, c); }
float eh_w_text_height(eh_ui *ui, const char *text, float px, float width) { return gs_ui_text_height(ui->gs, text, px, width); }

/* ---------------- controls ---------------- */

// Whether the pointer can reach a control at r: not while the controls are inert, and not where
// the page's edge has cut it off.
static bool reachable(eh_ui *ui, gs_rect r) {
    if (ui->inert) return false;
    if (ui->scrolling && !gs_rect_contains(ui->view, ui->mx, ui->my)) return false;
    return gs_rect_contains(r, ui->mx, ui->my);
}

static bool visible(eh_ui *ui, gs_rect r) { return !ui->scrolling || (r.y + r.h > ui->view.y && r.y < ui->view.y + ui->view.h); }

// Enters a control into the Tab order. True when it has the keyboard.
static bool stop_at(eh_ui *ui, const char *id, gs_rect r, bool field) {
    if (ui->inert) return false;
    Uint32 h = hash(id);
    if (ui->count < ORDER) {
        stop *s = &ui->order[ui->count++];
        s->id = h, s->r = r, s->field = field, s->paged = ui->scrolling;
        SDL_strlcpy(s->name, id, sizeof s->name);
    }
    return ui->focus == h;
}

static void focus_ring(eh_ui *ui, gs_rect r, float radius) {
    float s = gs_ui_scale(ui->gs);
    r = gs_inset(r, -3);
    eh_stroke_round(ui->r, r.x * s, r.y * s, r.w * s, r.h * s, (radius + 3) * s, FOCUS_RING * s, eh_rgba(EH_TEXT, 0.9f));
}

// A click on a control that is not a field: it takes the focus, and any field loses the keyboard.
static bool clicked(eh_ui *ui, const char *id, gs_rect r) {
    if (!reachable(ui, r) || !gs_ui_clicked(ui->gs, r)) return false;
    ui->focus = hash(id);
    gs_ui_focus(ui->gs, NULL);
    return true;
}

float eh_w_button_width(eh_ui *ui, const char *label) { return gs_ui_text_width(ui->gs, label, 13) + 28; }

bool eh_w_button(eh_ui *ui, gs_rect r, const char *id, const char *label, eh_btn kind) {
    bool focused = stop_at(ui, id, r, false), hot = reachable(ui, r);
    bool nav = kind == EH_BTN_NAV || kind == EH_BTN_NAV_ON;
    float radius = nav ? 8 : r.h / 2;
    if (!visible(ui, r)) return focused && ui->activate;
    SDL_FColor text = eh_ui_ink(ui, EH_TEXT, ui->inert ? 0.5f : 1);
    if (kind == EH_BTN_PRIMARY) {
        eh_w_round(ui, r, radius, eh_ui_ink(ui, hot ? 0xffffff : EH_TEXT, ui->inert ? 0.5f : 1));
        text = eh_rgba(EH_INK, 1);
    } else if (nav) {
        if (kind == EH_BTN_NAV_ON || hot) eh_w_round(ui, r, radius, kind == EH_BTN_NAV_ON ? EH_WASH : EH_WASH_FAINT);
        if (kind != EH_BTN_NAV_ON && !hot) text = eh_ui_ink(ui, EH_TEXT_DIM, 1);
    } else {
        eh_w_round(ui, r, radius, kind == EH_BTN_DANGER && hot ? eh_rgba(EH_ERR, 0.14f) : eh_rgba(0xffffff, hot ? 0.14f : 0.08f));
        if (kind == EH_BTN_DANGER && hot) text = eh_rgba(EH_ERR, 1);
    }
    if (nav) gs_ui_text(ui->gs, (gs_rect){ r.x + 12, r.y, r.w - 20, r.h }, label, 13, text, -1);
    else gs_ui_text(ui->gs, gs_inset(r, 4), label, 13, text, 0);
    if (focused && ui->by_keyboard) focus_ring(ui, r, radius);
    return clicked(ui, id, r) || (focused && ui->activate);
}

bool eh_w_switch(eh_ui *ui, gs_rect r, const char *id, bool *on) {
    bool focused = stop_at(ui, id, r, false);
    bool hit = clicked(ui, id, r) || (focused && ui->activate);
    if (hit) *on = !*on;
    if (!visible(ui, r)) return hit;
    eh_w_round(ui, r, r.h / 2, *on ? eh_ui_ink(ui, EH_TEXT, 1) : eh_ui_ink(ui, 0xffffff, 0.1f));
    if (!*on) eh_w_outline(ui, r, r.h / 2, EH_EDGE);
    float s = gs_ui_scale(ui->gs), cx = *on ? r.x + r.w - 11 : r.x + 11;
    eh_fill_circle(ui->r, cx * s, (r.y + r.h / 2) * s, 8 * s, *on ? eh_rgba(EH_INK, 1) : eh_ui_ink(ui, EH_TEXT_DIM, 1));
    if (focused && ui->by_keyboard) focus_ring(ui, r, r.h / 2);
    return hit;
}

bool eh_w_radio(eh_ui *ui, gs_rect r, const char *id, const char *label, bool selected) {
    bool focused = stop_at(ui, id, r, false);
    bool hit = clicked(ui, id, r) || (focused && ui->activate);
    if (!visible(ui, r)) return hit && !selected;
    float s = gs_ui_scale(ui->gs), cx = (r.x + 9) * s, cy = (r.y + r.h / 2) * s;
    eh_stroke_round(ui->r, cx - 8 * s, cy - 8 * s, 16 * s, 16 * s, 8 * s, 1.5f * s, eh_ui_ink(ui, selected ? EH_TEXT : EH_FIELD_EDGE, 1));
    if (selected) eh_fill_circle(ui->r, cx, cy, 4.5f * s, eh_ui_ink(ui, EH_TEXT, 1));
    gs_ui_text(ui->gs, (gs_rect){ r.x + 26, r.y, r.w - 26, r.h }, label, 13, eh_ui_ink(ui, EH_TEXT, reachable(ui, r) || selected ? 1 : 0.86f), -1);
    if (focused && ui->by_keyboard) focus_ring(ui, (gs_rect){ r.x, r.y + r.h / 2 - 9, 18, 18 }, 9);
    return hit && !selected;
}

// A field's look without gs_ui behind it, for one that must not take clicks.
static void still_field(eh_ui *ui, gs_rect r, const char *text, bool placeholder) {
    gs_ui_fill(ui->gs, r, eh_rgba(EH_FIELD, 1));
    gs_ui_frame(ui->gs, r, eh_ui_ink(ui, EH_FIELD_EDGE, 1));
    gs_ui_text(ui->gs, (gs_rect){ r.x + 8, r.y, r.w - 16, r.h }, text, 13, eh_ui_ink(ui, placeholder ? EH_TEXT_DIM : EH_TEXT, 1), -1);
}

// gs_ui's own controls know nothing of the page's edge or of inert controls: they are drawn only
// where the pointer may use all of what shows.
static bool live(eh_ui *ui, gs_rect r) {
    if (ui->inert) return false;
    if (!ui->scrolling) return true;
    bool whole = r.y >= ui->view.y && r.y + r.h <= ui->view.y + ui->view.h;
    return whole || gs_rect_contains(ui->view, ui->mx, ui->my);
}

// After a gs_ui field drew: the kit's focus follows a click into it or out of it.
static void follow_field(eh_ui *ui, const char *id) {
    Uint32 h = hash(id);
    if (gs_ui_focused(ui->gs, id)) ui->focus = h;
    else if (ui->focus == h) ui->focus = 0;
}

bool eh_w_field(eh_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder, int flags) {
    bool focused = stop_at(ui, id, r, true);
    if (!visible(ui, r)) return false;
    bool masked = (flags & EH_FIELD_MASKED) && !gs_ui_focused(ui->gs, id) && buf[0];
    if (!live(ui, r) || masked) {
        char dots[64] = "";
        for (size_t i = 0, n = SDL_min(SDL_utf8strlen(buf), 20); masked && i < n; i++) SDL_strlcat(dots, "•", sizeof dots);
        still_field(ui, r, masked ? dots : buf[0] ? buf : placeholder ? placeholder : "", !masked && !buf[0]);
        // A click or Tab into a masked field shows it for editing.
        if (masked && !ui->inert && ((reachable(ui, r) && gs_ui_clicked(ui->gs, r)) || focused)) gs_ui_focus(ui->gs, id), ui->focus = hash(id);
        return false;
    }
    bool changed = gs_ui_field(ui->gs, r, id, buf, size, placeholder);
    follow_field(ui, id);
    return changed;
}

bool eh_w_textarea(eh_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder) {
    stop_at(ui, id, r, true);
    if (!visible(ui, r)) return false;
    if (!live(ui, r)) {
        gs_ui_fill(ui->gs, r, eh_rgba(EH_FIELD, 1));
        gs_ui_frame(ui->gs, r, eh_ui_ink(ui, EH_FIELD_EDGE, 1));
        gs_rect in = gs_inset(r, 8);
        gs_rect clip = r;
        if (ui->scrolling) {  // the page's clip and the field's, together
            float top = SDL_max(r.y, ui->view.y), bottom = SDL_min(r.y + r.h, ui->view.y + ui->view.h);
            clip = (gs_rect){ r.x, top, r.w, SDL_max(0, bottom - top) };
        }
        gs_ui_clip(ui->gs, &clip);
        gs_ui_text_wrapped(ui->gs, in, buf[0] ? buf : placeholder ? placeholder : "", 13, eh_ui_ink(ui, buf[0] ? EH_TEXT : EH_TEXT_DIM, 1));
        gs_ui_clip(ui->gs, ui->scrolling ? &ui->view : NULL);
        return false;
    }
    bool changed = gs_ui_textarea(ui->gs, r, id, buf, size, placeholder);
    if (ui->scrolling) gs_ui_clip(ui->gs, &ui->view);  // the textarea ends its own clip, and the page's with it
    follow_field(ui, id);
    return changed;
}

bool eh_w_select(eh_ui *ui, gs_rect r, const char *id, const char *const *items, int count, int *selected) {
    bool focused = stop_at(ui, id, r, false);
    if (!visible(ui, r)) return false;
    bool changed = false;
    if (!live(ui, r)) {
        still_field(ui, r, *selected >= 0 && *selected < count ? items[*selected] : "", false);
    } else {
        changed = gs_ui_dropdown(ui->gs, r, id, items, count, selected);
        if (reachable(ui, r) && gs_ui_clicked(ui->gs, r)) ui->focus = hash(id), gs_ui_focus(ui->gs, NULL);
        gs_ui_frame(ui->gs, r, eh_rgba(EH_FIELD_EDGE, 1));
    }
    // With the keyboard the choice moves without the list opening, as a system's own select does.
    if (focused && ui->arrow && count > 0) {
        int next = SDL_clamp(*selected + ui->arrow, 0, count - 1);
        changed = changed || next != *selected;
        *selected = next;
    }
    if (focused && ui->by_keyboard) focus_ring(ui, r, 2);
    return changed;
}

bool eh_w_stops(eh_ui *ui, gs_rect r, const char *id, int *value, int count) {
    bool focused = stop_at(ui, id, r, false);
    if (!visible(ui, r) || count < 2) return false;
    int before = *value;
    float s = gs_ui_scale(ui->gs), x0 = r.x + 9, x1 = r.x + r.w - 9, y = r.y + r.h / 2;
    if (reachable(ui, r) && (gs_ui_clicked(ui->gs, r) || (ui->down && gs_rect_contains(r, ui->press_x, ui->press_y)))) {
        *value = SDL_clamp((int)floorf((ui->mx - x0) / (x1 - x0) * (float)(count - 1) + 0.5f), 0, count - 1);
        ui->focus = hash(id);
        gs_ui_focus(ui->gs, NULL);
        want_frame(ui, 16);
    }
    if (focused && ui->arrow) *value = SDL_clamp(*value + ui->arrow, 0, count - 1);
    float knob = x0 + (x1 - x0) * (float)*value / (float)(count - 1);
    eh_w_round(ui, (gs_rect){ x0, y - 2, x1 - x0, 4 }, 2, eh_ui_ink(ui, 0xffffff, 0.12f));
    eh_w_round(ui, (gs_rect){ x0, y - 2, knob - x0, 4 }, 2, eh_ui_ink(ui, EH_TEXT, 1));
    for (int i = 0; i < count; i++) eh_fill_circle(ui->r, (x0 + (x1 - x0) * (float)i / (float)(count - 1)) * s, y * s, 2.5f * s, eh_ui_ink(ui, i <= *value ? EH_TEXT : EH_TEXT_DIM, 1));
    eh_fill_circle(ui->r, knob * s, y * s, 8 * s, eh_ui_ink(ui, EH_TEXT, 1));
    if (focused && ui->by_keyboard) focus_ring(ui, (gs_rect){ knob - 8, y - 8, 16, 16 }, 8);
    return *value != before;
}

bool eh_w_hotkey(eh_ui *ui, gs_rect r, const char *id, const char *shown, const char *placeholder, char *out, size_t size) {
    Uint32 h = hash(id);
    bool focused = stop_at(ui, id, r, false), capturing = ui->capturing == h, got = false;
    if (capturing && (ui->capture_ended || ui->focus != h || ui->inert)) capturing = false, ui->capturing = 0, ui->capture_ended = false;
    if (capturing && ui->captured[0]) {
        SDL_strlcpy(out, ui->captured, size);
        got = true;
        capturing = false;
        ui->capturing = 0;
    }
    if (!capturing && (clicked(ui, id, r) || (focused && ui->activate))) {
        ui->capturing = h;
        ui->captured[0] = 0;
        ui->capture_ended = false;
        capturing = true;
    }
    if (!visible(ui, r)) return got;
    // Waiting for keys is the one coral moment of these windows.
    gs_ui_fill(ui->gs, r, eh_rgba(EH_FIELD, 1));
    gs_ui_frame(ui->gs, r, capturing ? eh_rgba(EH_ACCENT, 1) : eh_ui_ink(ui, reachable(ui, r) ? EH_TEXT_DIM : EH_FIELD_EDGE, 1));
    const char *text = capturing ? "Press keys…" : got ? out : shown && *shown ? shown : placeholder;
    bool dim = !capturing && !(got || (shown && *shown));
    gs_ui_text(ui->gs, (gs_rect){ r.x + 11, r.y, r.w - 22, r.h }, text, 13, capturing ? eh_rgba(EH_ACCENT, 1) : eh_ui_ink(ui, dim ? EH_TEXT_DIM : EH_TEXT, 1), -1);
    if (focused && ui->by_keyboard && !capturing) focus_ring(ui, r, 2);
    return got;
}

/* ---------------- the question ---------------- */

void eh_ui_ask(eh_ui *ui, const char *question, const char *yes, bool dangerous) {
    SDL_strlcpy(ui->question, question, sizeof ui->question);
    SDL_strlcpy(ui->yes, yes, sizeof ui->yes);
    ui->asking = true;
    ui->dangerous = dangerous;
    ui->asked_now = true;  // the click that asked is not an answer
    ui->focus = hash("ask-no");
    ui->by_keyboard = true;
    gs_ui_focus(ui->gs, NULL);
}

int eh_ui_modal(eh_ui *ui) {
    if (!ui->asking) return 0;
    int w, h;
    SDL_GetWindowSize(ui->win, &w, &h);
    gs_ui_fill(ui->gs, (gs_rect){ 0, 0, (float)w, (float)h }, eh_rgba(0x000000, 0.55f));
    float bw = SDL_min(420, (float)w - 48), text_h = gs_ui_text_height(ui->gs, ui->question, 14, bw - 44);
    gs_rect box = { ((float)w - bw) / 2, ((float)h - text_h - 96) / 2, bw, text_h + 96 };
    eh_w_round(ui, box, 14, eh_rgba(0x232327, 1));
    eh_w_outline(ui, box, 14, EH_EDGE);
    gs_ui_text_wrapped(ui->gs, (gs_rect){ box.x + 22, box.y + 22, bw - 44, text_h }, ui->question, 14, eh_rgba(EH_TEXT, 1));
    // The controls under the question are inert; its own two are not.
    bool was = ui->inert;
    ui->inert = false;
    float yes_w = eh_w_button_width(ui, ui->yes), no_w = eh_w_button_width(ui, "Cancel"), y = box.y + box.h - 22 - 34;
    int answer = 0;
    if (eh_w_button(ui, (gs_rect){ box.x + bw - 22 - yes_w, y, yes_w, 34 }, "ask-yes", ui->yes, ui->dangerous ? EH_BTN_DANGER : EH_BTN_PRIMARY)) answer = 1;
    if (eh_w_button(ui, (gs_rect){ box.x + bw - 22 - yes_w - 10 - no_w, y, no_w, 34 }, "ask-no", "Cancel", EH_BTN_PLAIN)) answer = 2;
    ui->inert = was;
    if (ui->escape) answer = 2;
    if (ui->asked_now) answer = 0;
    if (answer) ui->asking = false, ui->focus = 0;
    return answer;
}
