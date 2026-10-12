// The correction editor: a saved transcript in which a word or phrase is picked, given its
// correct spelling, and replaced, with the spelling remembered in the dictionary for future
// cleanup. The corrected text is copied to the clipboard; the original stays in History. Ported
// from renderer/correction.js and the correction handlers of main/ipc.js and main/windows.js.
//
// gesso's text area does not say what is selected in it, so the transcript is shown as words to
// pick: a click picks a word, a drag or Shift with the arrow keys a phrase. "Edit text" swaps in
// a text area for changing the transcript by hand. Everything here runs on the main thread, with
// the loop's lock held for the length of a frame.
#include "correction_window.h"
#include "commit.h"
#include "deliver.h"
#include "activate.h"
#include "gs_desk.h"
#include "fonts.h"
#include "gs_draw.h"
#include "gs_text.h"
#include "loop.h"
#include "pipeline.h"
#include "settings.h"
#include "text.h"
#include "ui.h"

#define TEXT_MAX (256 * 1024)
#define WORDS_MAX 16384

typedef struct { char *before, *after; int start, end; } replacement;
typedef struct { int start, end; } span;

static struct {
    eh_ui *ui;
    char *text, *original;
    span words[WORDS_MAX];
    int word_count;
    int first, last, anchor;  // the words picked, or -1
    bool dragging, editing;
    char spelling[128], picked_for[128];  // the spelling typed, and what the pick put there
    char status[400], note[200];
    replacement *undo;
    int undo_count;
    bool busy, copied, close_now;
    int asking;  // the open question is about closing with a draft
    float content_h;
} C;

static void find_words(void) {
    C.word_count = 0;
    for (int i = 0; C.text[i] && C.word_count < WORDS_MAX;) {
        while (C.text[i] == ' ' || C.text[i] == '\n' || C.text[i] == '\t' || C.text[i] == '\r') i++;
        if (!C.text[i]) break;
        int start = i;
        while (C.text[i] && C.text[i] != ' ' && C.text[i] != '\n' && C.text[i] != '\t' && C.text[i] != '\r') i++;
        C.words[C.word_count++] = (span){ start, i };
    }
}

static bool picked(void) { return C.first >= 0 && C.last >= C.first && C.last < C.word_count; }
static int pick_start(void) { return C.words[C.first].start; }
static int pick_end(void) { return C.words[C.last].end; }

static void clear_undo(void) {
    for (int i = 0; i < C.undo_count; i++) SDL_free(C.undo[i].before), SDL_free(C.undo[i].after);
    SDL_free(C.undo);
    C.undo = NULL;
    C.undo_count = 0;
}

static void unpick(void) {
    C.first = C.last = C.anchor = -1;
    C.spelling[0] = C.picked_for[0] = 0;
}

// The words from `a` to `b` become the pick, and the spelling field is filled with them.
static void pick(int a, int b) {
    C.first = SDL_min(a, b), C.last = SDL_max(a, b);
    int n = SDL_min(pick_end() - pick_start(), (int)sizeof C.spelling - 1);
    SDL_memcpy(C.spelling, C.text + pick_start(), (size_t)n);
    C.spelling[n] = 0;
    SDL_strlcpy(C.picked_for, C.spelling, sizeof C.picked_for);
}

// A spelling typed for the pick that has not been applied.
static bool pending(void) {
    if (!picked()) return false;
    char *t = eh_trim(C.spelling);
    int n = pick_end() - pick_start();
    bool differs = *t && ((int)SDL_strlen(t) != n || SDL_strncmp(t, C.text + pick_start(), (size_t)n));
    SDL_free(t);
    return differs;
}

static bool dirty(void) { return SDL_strcmp(C.text, C.original) != 0 || pending(); }

static void replace(bool remember) {
    char *spelling = eh_trim(C.spelling);
    if (!picked() || !*spelling || C.busy) { SDL_free(spelling); return; }
    if (remember) {
        char error[200];
        gs_jval *list = eh_vocabulary_remember(gs_jval_path(eh_settings(), "cleanup.dictionary"), spelling, error, sizeof error);
        char save_error[200] = "";
        if (!list || !eh_setting_set("cleanup.dictionary", list, save_error, sizeof save_error)) {
            SDL_snprintf(C.status, sizeof C.status, "%s Your draft is still here; you can use Replace only.", list ? save_error : error);
            SDL_free(spelling);
            return;
        }
        SDL_strlcpy(C.note, eh_setting_bool("cleanup.enabled") ? "Remembered spellings guide future cleanup. Manage them in Settings → Dictionary."
                                                                  : "Cleanup is off. This spelling is saved, but will only guide future dictations when cleanup is enabled.", sizeof C.note);
    }
    int start = pick_start(), end = pick_end();
    size_t len = SDL_strlen(C.text), add = SDL_strlen(spelling);
    if (len - (size_t)(end - start) + add < TEXT_MAX) {
        char *before = SDL_strdup(C.text);
        SDL_memmove(C.text + start + add, C.text + end, len - (size_t)end + 1);
        SDL_memcpy(C.text + start, spelling, add);
        if (SDL_strcmp(before, C.text)) {
            C.undo = SDL_realloc(C.undo, sizeof *C.undo * (size_t)(C.undo_count + 1));
            C.undo[C.undo_count++] = (replacement){ before, SDL_strdup(C.text), start, end };
        } else {
            SDL_free(before);
        }
        find_words();
        unpick();
        if (remember) SDL_snprintf(C.status, sizeof C.status, "Replaced and remembered “%s”.", spelling);
        else SDL_strlcpy(C.status, "Replaced this occurrence.", sizeof C.status);
    }
    SDL_free(spelling);
}

static void undo(void) {
    if (!C.undo_count) return;
    replacement r = C.undo[--C.undo_count];
    if (SDL_strcmp(C.text, r.after)) {
        // The text was changed since: the replacements before it no longer apply.
        SDL_free(r.before);
        SDL_free(r.after);
        clear_undo();
        return;
    }
    SDL_strlcpy(C.text, r.before, TEXT_MAX);
    SDL_free(r.before);
    SDL_free(r.after);
    find_words();
    unpick();
    // The words the replacement had taken the place of are the pick again.
    for (int i = 0; i < C.word_count; i++) {
        if (C.words[i].start == r.start) C.first = i;
        if (C.words[i].end == r.end && C.first >= 0) { pick(C.first, i); break; }
    }
    SDL_strlcpy(C.status, "Replacement undone. Already remembered spellings stay in your dictionary.", sizeof C.status);
}

static void copy_task(void *arg) {
    char *text = arg, error[256] = "";
    eh_delivery result;
    gs_jval *output = gs_jval_object();
    gs_jval_set(output, "mode", gs_jval_str("clipboard"));
    bool has_text = false;
    for (const char *p = text; *p && !has_text; p++) has_text = *p != ' ' && *p != '\n' && *p != '\t' && *p != '\r';
    if (eh_pipeline_state() == EH_PROCESSING) SDL_strlcpy(error, "Wait for the current dictation to finish processing before copying a transcript.", sizeof error);
    else if (!has_text) SDL_strlcpy(error, "The corrected prompt must contain text and be at most 1 million characters.", sizeof error);
    else if (eh_deliver(text, output, NULL, &result, error, sizeof error)) C.copied = true;
    if (!C.copied) SDL_snprintf(C.status, sizeof C.status, "Could not copy: %s. Your draft is still here.", error);
    C.busy = false;
    gs_jval_free(output);
    SDL_free(text);
}

static void copy(void) {
    if (C.busy) return;
    if (pending()) {
        SDL_strlcpy(C.status, "Apply your spelling with Replace & remember or Replace only before copying.", sizeof C.status);
        eh_ui_focus(C.ui, "remember");
        return;
    }
    C.busy = true;
    eh_spawn(copy_task, SDL_strdup(C.text));
}

/* ---------------- the transcript as words ---------------- */

static float width_of(const char *from, int n) {
    char *piece = SDL_strndup(from, (size_t)n);
    float w = gs_ui_text_width(eh_ui_gs(C.ui), piece, 14);
    SDL_free(piece);
    return w;
}

// Draws the transcript in `box`, takes clicks and drags on its words, and returns its height.
static float words_view(gs_rect box, bool focused) {
    gs_ui *gs = eh_ui_gs(C.ui);
    float s = gs_ui_scale(gs), line_h = gs_ui_line_height(gs, 14);
    gs_wrap *wrap = *C.text ? gs_fontset_wrap(eh_fonts(), 14 * s, C.text, box.w * s) : NULL;
    int lines = wrap ? wrap->count : 0;
    float mx = eh_ui_pointer_x(C.ui), my = eh_ui_pointer_y(C.ui);
    // The word under the pointer.
    int hover = -1, row = (int)SDL_floorf((my - box.y) / line_h);
    if (row >= 0 && row < lines && mx >= box.x && mx <= box.x + box.w) {
        gs_wrap_line ln = wrap->lines[row];
        for (int i = 0; i < C.word_count && hover < 0; i++) {
            span w = C.words[i];
            if (w.start < ln.start || w.start >= ln.end) continue;
            float x0 = box.x + width_of(C.text + ln.start, w.start - ln.start), x1 = box.x + width_of(C.text + ln.start, SDL_min(w.end, ln.end) - ln.start);
            if (mx >= x0 - 3 && mx <= x1 + 3) hover = i;
        }
    }
    bool inside = gs_rect_contains(box, mx, my) || C.dragging;
    if (!C.busy && inside && eh_ui_pointer_down(C.ui)) {
        if (!C.dragging) {
            C.dragging = true;
            C.anchor = hover;
            if (hover >= 0) pick(hover, hover);
            else unpick();
            eh_ui_focus(C.ui, "transcript");
        } else if (hover >= 0 && C.anchor >= 0) {
            pick(C.anchor, hover);
        }
        eh_ui_again(C.ui, 16);
    } else {
        C.dragging = false;
    }
    for (int i = 0; i < lines; i++) {
        gs_wrap_line ln = wrap->lines[i];
        float y = box.y + (float)i * line_h;
        if (picked() && pick_start() < ln.end && pick_end() > ln.start) {
            int from = SDL_max(pick_start(), ln.start), to = SDL_min(pick_end(), ln.end);
            float x0 = box.x + width_of(C.text + ln.start, from - ln.start), x1 = box.x + width_of(C.text + ln.start, to - ln.start);
            eh_w_round(C.ui, (gs_rect){ x0 - 2, y + 1, x1 - x0 + 4, line_h - 2 }, 4, gs_rgba(EH_TEXT, focused ? 0.28f : 0.18f));
        }
        char *line = SDL_strndup(C.text + ln.start, (size_t)(ln.end - ln.start));
        gs_ui_text(gs, (gs_rect){ box.x, y, box.w + 40, line_h }, line, 14, gs_rgba(EH_TEXT, 1), -1);
        SDL_free(line);
    }
    if (wrap) gs_wrap_free(wrap);
    return (float)SDL_max(lines, 1) * line_h;
}

/* ---------------- the window ---------------- */

bool eh_correction_window_open(const char *text, char *error, size_t error_size) {
    if (C.ui) {
        // One draft at a time: another transcript does not replace it.
        if (text && *text && SDL_strcmp(text, C.original)) SDL_strlcpy(C.status, "Your current correction is still open. Copy or close it before opening another transcript.", sizeof C.status);
        SDL_ShowWindow(eh_ui_window(C.ui));
        SDL_RaiseWindow(eh_ui_window(C.ui));
    eh_app_activate();
        return true;
    }
    eh_lock();
    bool idle = eh_pipeline_state() == EH_IDLE, cleanup = eh_setting_bool("cleanup.enabled");
    eh_unlock();
    // A window that takes the keyboard must not take it from a dictation's paste target.
    if (!idle) { SDL_strlcpy(error, "Finish the current dictation before correcting a transcript.", error_size); return false; }
    C.ui = eh_ui_new("Correct transcript · Earheart", 720, 780, 520, 600);
    if (!C.ui) { SDL_strlcpy(error, "Could not open the editor", error_size); return false; }
    C.text = SDL_calloc(1, TEXT_MAX);
    SDL_strlcpy(C.text, text ? text : "", TEXT_MAX);
    C.original = SDL_strdup(C.text);
    find_words();
    unpick();
    C.dragging = C.editing = C.busy = C.copied = C.close_now = false;
    C.asking = 0;
    SDL_strlcpy(C.note, cleanup ? "Remember spellings to guide future cleanup. No automatic replacements or model retraining." : "Cleanup is off. Remembered spellings take effect when cleanup is enabled.", sizeof C.note);
    SDL_strlcpy(C.status, *C.text ? "" : "No saved transcript yet. Enable History and finish a dictation first.", sizeof C.status);
    eh_ui_focus(C.ui, "transcript");
    SDL_ShowWindow(eh_ui_window(C.ui));
    SDL_RaiseWindow(eh_ui_window(C.ui));
    eh_app_activate();
    return true;
}

bool eh_correction_window_is_open(void) { return C.ui != NULL; }

void eh_correction_window_close(void) {
    if (!C.ui) return;
    eh_ui_free(C.ui);
    C.ui = NULL;
    SDL_free(C.text);
    SDL_free(C.original);
    C.text = C.original = NULL;
    clear_undo();
}

bool eh_correction_window_event(const SDL_Event *e) { return C.ui && eh_ui_event(C.ui, e); }
struct eh_ui *eh_correction_window_ui(void) { return C.ui; }
const char *eh_correction_window_text(const char *what) {
    if (!C.ui) return "";
    return !SDL_strcmp(what, "status") ? C.status : !SDL_strcmp(what, "spelling") ? C.spelling : !SDL_strcmp(what, "note") ? C.note : C.text;
}

static void label(float x, float *y, float w, const char *s) {
    eh_w_line(C.ui, (gs_rect){ x, *y, w, 20 }, s, 13, gs_rgba(EH_TEXT, 1), -1);
    *y += 26;
}

static void hint(float x, float *y, float w, const char *s) { *y += eh_w_text(C.ui, (gs_rect){ x, *y, w, 100 }, s, 12, gs_rgba(EH_TEXT_DIM, 1)) + 10; }

int eh_correction_window_frame(void) {
    if (!C.ui) return -1;
    if (C.copied || C.close_now) {
        eh_correction_window_close();
        return -1;
    }
    eh_lock();
    // Closing with a draft is asked about first.
    if (eh_ui_close_asked(C.ui) && !C.busy && !eh_ui_asking(C.ui)) {
        if (dirty()) {
            eh_ui_ask(C.ui, "Discard your uncopied corrections? Your original transcript stays in History. Remembered spellings have already been saved.", "Discard draft", "Keep editing", true);
            C.asking = 1;
        } else {
            C.close_now = true;
        }
    }
    // Asked before the frame begins, which is when Escape takes the keyboard from a field.
    bool spelling_focused = gs_ui_focused(eh_ui_gs(C.ui), "spelling");
    gs_rect area = eh_ui_begin(C.ui);
    gs_ui *gs = eh_ui_gs(C.ui);
    eh_ui_inert(C.ui, C.busy, false);
    // Ctrl or Cmd with Enter copies, wherever the keyboard is.
    if (gs_ui_key(gs, SDLK_RETURN, SDL_KMOD_CTRL | SDL_KMOD_GUI) && !C.busy && !eh_ui_asking(C.ui)) copy();

    float x = 28, w = area.w - 56, y = 22;
    eh_w_line(C.ui, (gs_rect){ x, y, w, 26 }, "Correct transcript", 18, gs_rgba(EH_TEXT, 1), -1);
    y += 34;
    hint(x, &y, w, "Pick a word or phrase, then enter its correct spelling. Your original stays in History.");

    // From the bottom up, so the transcript takes the room that is left.
    float bottom = area.h - 20;
    gs_rect last_hint = { x, bottom - 34, w, 34 };
    eh_w_text(C.ui, last_hint, "Ctrl/Cmd+Enter copies the full prompt. Return to your agent and select the text you want to replace before pasting.", 12, gs_rgba(EH_TEXT_DIM, 1));
    bottom -= 34 + 12;
    float copy_w = eh_w_button_width(C.ui, "Copy corrected prompt & close") + 12, close_w = eh_w_button_width(C.ui, "Close");
    gs_rect copy_r = { x + w - copy_w, bottom - 36, copy_w, 36 }, close_r = { x + w - copy_w - 10 - close_w, bottom - 36, close_w, 36 };
    bottom -= 36 + 14;
    float status_h = C.status[0] ? eh_w_text_height(C.ui, C.status, 12, w) + 8 : 0, note_h = eh_w_text_height(C.ui, C.note, 12, w) + 8;
    float undo_hint_h = eh_w_text_height(C.ui, "Undo reverses replacements since your last direct edit. Already remembered spellings stay in your dictionary.", 12, w) + 8;
    float lower = 26 + 36 + 12 + 36 + 12 + undo_hint_h + note_h + status_h;  // spelling label and field, the actions, the notes
    float hint_h = 26;
    gs_rect box = { x, y + 26, w, SDL_max(120, bottom - lower - hint_h - (y + 26) - 10) };

    label(x, &y, w, "Transcript");
    float edit_w = eh_w_button_width(C.ui, C.editing ? "Done editing" : "Edit text");
    if (eh_w_button(C.ui, (gs_rect){ x + w - edit_w, y - 32, edit_w, 28 }, "edit", C.editing ? "Done editing" : "Edit text", EH_BTN_PLAIN)) {
        C.editing = !C.editing;
        find_words();
        unpick();
        if (!C.editing) eh_ui_focus(C.ui, "transcript");
    }
    if (C.editing) {
        // By hand: the replacements made before it can no longer be undone.
        if (eh_w_textarea(C.ui, box, "transcript-edit", C.text, TEXT_MAX, NULL)) clear_undo(), find_words();
    } else {
        gs_ui_fill(gs, box, gs_rgba(EH_FIELD, 1));
        int arrow = 0;
        bool shift = false, activated = false, focused = eh_ui_custom_stop(C.ui, "transcript", box, &arrow, &shift, &activated);
        gs_ui_frame(gs, box, gs_rgba(focused ? EH_TEXT : EH_FIELD_EDGE, 1));
        // With the keyboard: the arrows move the pick a word at a time, Shift grows it.
        if (focused && arrow && C.word_count) {
            if (!picked()) pick(0, 0), C.anchor = 0;
            else if (shift) {
                if (C.anchor < 0) C.anchor = C.first;
                int moving = C.anchor == C.first ? C.last : C.first;
                pick(C.anchor, SDL_clamp(moving + arrow, 0, C.word_count - 1));
            } else {
                int at = SDL_clamp((arrow > 0 ? C.last : C.first) + arrow, 0, C.word_count - 1);
                pick(at, at);
                C.anchor = at;
            }
        }
        gs_rect inner = gs_inset(box, 10);
        float top = eh_scroll_begin(C.ui, box, C.content_h);
        float h = words_view((gs_rect){ inner.x, top + 10, inner.w, 0 }, focused);
        if (SDL_fabsf(h + 20 - C.content_h) > 0.5f) C.content_h = h + 20, eh_ui_again(C.ui, 0);
        eh_scroll_end(C.ui, top + h + 20);
    }
    y = box.y + box.h + 8;
    char picked_hint[400];
    if (picked()) {
        int n = pick_end() - pick_start();
        SDL_snprintf(picked_hint, sizeof picked_hint, "Picked: “%.*s%s”. Press Tab to correct its spelling.", SDL_min(n, 120), C.text + pick_start(), n > 120 ? "…" : "");
    } else {
        SDL_strlcpy(picked_hint, C.editing ? "Change the transcript by hand, then choose Done editing to pick words again." : "Click a word, or drag across a phrase, then press Tab to correct it. The arrow keys pick too; Shift with them picks a phrase.", sizeof picked_hint);
    }
    eh_w_line(C.ui, (gs_rect){ x, y, w, 18 }, picked_hint, 12, gs_rgba(EH_TEXT_DIM, 1), -1);
    y += hint_h;

    label(x, &y, w, "Correct spelling");
    eh_w_field(C.ui, (gs_rect){ x, y, w, 36 }, "spelling", C.spelling, 121, NULL, 0);
    // Enter in the field replaces and remembers; Escape there gives up the pick.
    bool can = picked() && !C.busy && !C.editing;
    char *typed = eh_trim(C.spelling);
    can = can && *typed;
    SDL_free(typed);
    if (spelling_focused && gs_ui_key(gs, SDLK_RETURN, 0) && can) replace(true);
    if (spelling_focused && eh_ui_escaped(C.ui) && !eh_ui_asking(C.ui)) {
        unpick();
        SDL_strlcpy(C.status, "Pending replacement cancelled.", sizeof C.status);
        eh_ui_focus(C.ui, "transcript");
    }
    y += 36 + 12;
    float bx = x;
    eh_ui_inert(C.ui, !can, false);
    float bw = eh_w_button_width(C.ui, "Replace & remember");
    if (eh_w_button(C.ui, (gs_rect){ bx, y, bw, 36 }, "remember", "Replace & remember", EH_BTN_PRIMARY)) replace(true);
    bx += bw + 10;
    bw = eh_w_button_width(C.ui, "Replace only");
    if (eh_w_button(C.ui, (gs_rect){ bx, y, bw, 36 }, "replace", "Replace only", EH_BTN_PLAIN)) replace(false);
    bx += bw + 10;
    eh_ui_inert(C.ui, C.busy || !C.undo_count, false);
    bw = eh_w_button_width(C.ui, "Undo replacement");
    if (eh_w_button(C.ui, (gs_rect){ bx, y, bw, 36 }, "undo", "Undo replacement", EH_BTN_PLAIN)) undo();
    eh_ui_inert(C.ui, C.busy, false);
    y += 36 + 12;
    hint(x, &y, w, "Undo reverses replacements since your last direct edit. Already remembered spellings stay in your dictionary.");
    hint(x, &y, w, C.note);
    if (C.status[0]) eh_w_text(C.ui, (gs_rect){ x, y, w, 60 }, C.status, 12, gs_rgba(EH_TEXT, 1));

    bool has_text = false;
    for (const char *p = C.text; *p && !has_text; p++) has_text = *p != ' ' && *p != '\n';
    if (eh_w_button(C.ui, close_r, "cancel", "Close", EH_BTN_PLAIN)) {
        if (dirty()) {
            eh_ui_ask(C.ui, "Discard your uncopied corrections? Your original transcript stays in History. Remembered spellings have already been saved.", "Discard draft", "Keep editing", true);
            C.asking = 1;
        } else {
            C.close_now = true;
        }
    }
    eh_ui_inert(C.ui, C.busy || !has_text, false);
    if (eh_w_button(C.ui, copy_r, "copy", "Copy corrected prompt & close", EH_BTN_PRIMARY)) copy();
    eh_ui_inert(C.ui, false, false);

    int answer = eh_ui_modal(C.ui);
    if (answer && C.asking) {
        if (answer == 1) C.close_now = true;
        C.asking = 0;
    }
    if (C.busy) eh_ui_again(C.ui, 50);
    if (C.copied || C.close_now) eh_ui_again(C.ui, 0);
    int again = eh_ui_end(C.ui);
    eh_unlock();
    return again;
}
