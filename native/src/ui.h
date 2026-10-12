// The framed windows' controls (settings, wizard, correction): what gesso's gs_ui does not have
// yet, built on what it has. gs_ui supplies text fields, the dropdown's list, text and hit testing;
// this adds a window with its renderer, a Tab order with a visible focus mark and keyboard
// activation, one scrolling page per window, rounded buttons, switches, radio buttons, a hotkey
// field, a slider with stops, a modal question, and controls that can be inert. The look is
// DESIGN.md's. Everything runs on the main thread; all sizes are in points.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include "gs_ui.h"

typedef struct eh_ui eh_ui;

enum {
    EH_INK = 0x18181b, EH_TEXT = 0xf4f4f5, EH_TEXT_DIM = 0xa1a1aa, EH_ACCENT = 0xfb4d5c, EH_OK = 0x34d399, EH_ERR = 0xf87171,
    EH_FIELD = 0x101013, EH_FIELD_EDGE = 0x6b6b74,
};
#define EH_EDGE gs_rgba(0xffffff, 0.1f)
#define EH_WASH gs_rgba(0xffffff, 0.08f)
#define EH_WASH_FAINT gs_rgba(0xffffff, 0.04f)
#define EH_SEAM gs_rgba(0xffffff, 0.08f)

// A window with its renderer and controls, hidden until shown. NULL when the system gives none.
eh_ui *eh_ui_new(const char *title, int w, int h, int min_w, int min_h);
void eh_ui_free(eh_ui *ui);
SDL_Window *eh_ui_window(eh_ui *ui);
gs_ui *eh_ui_gs(eh_ui *ui);
// Give it every event; true when the event was this window's. A frame is then due.
bool eh_ui_event(eh_ui *ui, const SDL_Event *e);
bool eh_ui_close_asked(eh_ui *ui);  // the window's close button, or Escape with nothing to dismiss; reading it clears it
// A frame: begin, draw the controls, end. `end` presents and returns in how many milliseconds the
// window wants another frame without an event (-1: none).
gs_rect eh_ui_begin(eh_ui *ui);
int eh_ui_end(eh_ui *ui);
void eh_ui_again(eh_ui *ui, int ms);  // asks for another frame within `ms`, for something that changes by itself

// Controls drawn while inert are shown and cannot be used or reached by Tab; `dim` also fades them.
void eh_ui_inert(eh_ui *ui, bool inert, bool dim);
void eh_ui_focus(eh_ui *ui, const char *id);  // NULL: nothing has the keyboard
bool eh_ui_focused(eh_ui *ui, const char *id);
bool eh_ui_escaped(eh_ui *ui);  // Escape was pressed for this frame, with a field focused or a question open
float eh_ui_pointer_x(eh_ui *ui);
float eh_ui_pointer_y(eh_ui *ui);
bool eh_ui_pointer_down(eh_ui *ui);
// A control of the caller's own in the Tab order: true when it has the keyboard. `arrow` gets the
// arrow key pressed for it this frame (-1, 0 or 1), `shift` whether Shift was held with it, and
// `activated` whether Space or Enter was pressed on it.
bool eh_ui_custom_stop(eh_ui *ui, const char *id, gs_rect r, int *arrow, bool *shift, bool *activated);
// The Tab order of the frame last drawn, for tests: how many stops, the name of one, where one is.
int eh_ui_stops(eh_ui *ui);
const char *eh_ui_stop_name(eh_ui *ui, int index);
bool eh_ui_stop_rect(eh_ui *ui, const char *id, gs_rect *r);
bool eh_ui_stop_paged(eh_ui *ui, const char *id);  // it is on the scrolling page
const char *eh_ui_focus_name(eh_ui *ui);  // "" when nothing has the keyboard

// The window's one scrolling page. `content_h` is the height the page turned out to have the last
// time it was laid out. Returns the y at which the page's top is to be drawn; controls drawn until
// eh_scroll_end are clipped to `area`, and those outside it cannot be clicked.
float eh_scroll_begin(eh_ui *ui, gs_rect area, float content_h);
void eh_scroll_end(eh_ui *ui, float bottom);  // bottom: the y at which the page's content ended
void eh_scroll_reclip(eh_ui *ui);  // after a clip of the caller's own inside the page: the page's clip again
float eh_scroll_y(eh_ui *ui);
void eh_scroll_to(eh_ui *ui, float y, bool glide);

/* ---------------- drawing ---------------- */
void eh_w_round(eh_ui *ui, gs_rect r, float radius, SDL_FColor fill);
void eh_w_outline(eh_ui *ui, gs_rect r, float radius, SDL_FColor c);
void eh_w_icon(eh_ui *ui, gs_rect r);  // the app's icon
// One line, cut to fit; wrapped text from the top of r down, which returns its height.
void eh_w_line(eh_ui *ui, gs_rect r, const char *text, float px, SDL_FColor c, int align);
float eh_w_text(eh_ui *ui, gs_rect r, const char *text, float px, SDL_FColor c);
float eh_w_text_height(eh_ui *ui, const char *text, float px, float width);
SDL_FColor eh_ui_ink(eh_ui *ui, unsigned rgb, float alpha);  // faded when the controls are dim

/* ---------------- controls ---------------- */
typedef enum { EH_BTN_PLAIN, EH_BTN_PRIMARY, EH_BTN_DANGER, EH_BTN_NAV, EH_BTN_NAV_ON } eh_btn;
float eh_w_button_width(eh_ui *ui, const char *label);
bool eh_w_button(eh_ui *ui, gs_rect r, const char *id, const char *label, eh_btn kind);
bool eh_w_switch(eh_ui *ui, gs_rect r, const char *id, bool *on);          // r: the 40 x 22 switch itself
bool eh_w_radio(eh_ui *ui, gs_rect r, const char *id, const char *label, bool selected);  // true when chosen
bool eh_w_select(eh_ui *ui, gs_rect r, const char *id, const char *const *items, int count, int *selected);
enum { EH_FIELD_MASKED = 1 };  // shown as dots unless it has the keyboard
bool eh_w_field(eh_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder, int flags);
bool eh_w_textarea(eh_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder);
bool eh_w_stops(eh_ui *ui, gs_rect r, const char *id, int *value, int count);  // a slider of `count` positions
// A field that takes a key combination. It shows `shown` (or the placeholder) until clicked or
// activated, then "Press keys…" until a combination with a modifier is pressed, which it writes to
// `out` in Electron's accelerator form and returns true for. Escape and leaving it end the capture.
bool eh_w_hotkey(eh_ui *ui, gs_rect r, const char *id, const char *shown, const char *placeholder, char *out, size_t size);
bool eh_ui_capturing(eh_ui *ui);  // a hotkey field is waiting for keys

// A question over the window. Ask once; then each frame, after the page is drawn, eh_ui_modal
// draws it while it is open and returns 1 for the confirming answer, 2 for the other, 0 until then.
void eh_ui_ask(eh_ui *ui, const char *question, const char *yes, const char *no, bool dangerous);
bool eh_ui_asking(eh_ui *ui);
int eh_ui_modal(eh_ui *ui);
