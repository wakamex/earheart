// The correction editor: a saved transcript in which a word or phrase is picked, given its
// correct spelling, and replaced, with the spelling remembered in the dictionary for future
// cleanup. The corrected text is copied to the clipboard; the original stays in History. Ported
// from renderer/correction.js and the correction handlers of main/ipc.js and main/windows.js.
//
// The transcript is a text area: what is selected in it is what gets replaced, and it can be changed
// by hand. Everything here runs on the main thread, with the loop's lock held for the length of a
// frame.
#include "correction_window.h"
#include "commit.h"
#include "deliver.h"
#include "gs_desk.h"
#include "gs_draw.h"
#include "loop.h"
#include "pipeline.h"
#include "settings.h"
#include "text.h"
#include "ui.h"

#define TEXT_MAX (256 * 1024)
#define AREA "transcript"

typedef struct { char *before, *after; int start, end; } replacement;

static struct {
    eh_ui *ui;
    char *text, *original;
    int sel_start, sel_end;  // what is selected, without the blanks at its ends; -1 when nothing is
    int place_start, place_end;  // a selection to give the text area when it is next drawn, or -1
    char spelling[128];
    char status[400], note[200];
    replacement *undo;
    int undo_count;
    bool busy, copied, close_now;
    int asking;  // the open question is about closing with a draft
} C;

static bool picked(void) { return C.sel_start >= 0 && C.sel_end > C.sel_start; }
static int pick_start(void) { return C.sel_start; }
static int pick_end(void) { return C.sel_end; }

static void clear_undo(void) {
    for (int i = 0; i < C.undo_count; i++) SDL_free(C.undo[i].before), SDL_free(C.undo[i].after);
    SDL_free(C.undo);
    C.undo = NULL;
    C.undo_count = 0;
}

static void unpick(void) {
    C.sel_start = C.sel_end = -1;
    C.spelling[0] = 0;
}

// The range becomes what is to be replaced, and the spelling field is filled with it.
static void pick(int start, int end) {
    C.sel_start = start, C.sel_end = end;
    int n = SDL_min(end - start, (int)sizeof C.spelling - 1);
    SDL_memcpy(C.spelling, C.text + start, (size_t)n);
    C.spelling[n] = 0;
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
        unpick();
        // The cursor goes after what was put in.
        C.place_start = C.place_end = start + (int)add;
        eh_ui_focus(C.ui, AREA);
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
    // What the replacement had taken the place of is selected again.
    pick(r.start, r.end);
    C.place_start = r.start, C.place_end = r.end;
    eh_ui_focus(C.ui, AREA);
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

/* ---------------- what is selected ---------------- */

static bool blank(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

// Follows the text area's selection while it has the keyboard: the selection without the blanks at
// its ends is what will be replaced, and the spelling field starts as a copy of it. A selection
// given up while a spelling for it is typed and not applied is kept, so a stray click does not
// throw the spelling away.
static void capture(void) {
    int start = 0, end = 0;
    gs_ui_textarea_selection(eh_ui_gs(C.ui), AREA, &start, &end);
    while (start < end && blank(C.text[start])) start++;
    while (end > start && blank(C.text[end - 1])) end--;
    if (picked() && start == C.sel_start && end == C.sel_end) return;
    if (end <= start && pending()) return;
    if (end > start) pick(start, end);
    else unpick();
}

/* ---------------- the window ---------------- */

bool eh_correction_window_open(const char *text, char *error, size_t error_size) {
    if (C.ui) {
        // One draft at a time: another transcript does not replace it.
        if (text && *text && SDL_strcmp(text, C.original)) SDL_strlcpy(C.status, "Your current correction is still open. Copy or close it before opening another transcript.", sizeof C.status);
        SDL_ShowWindow(eh_ui_window(C.ui));
        gs_window_activate(eh_ui_window(C.ui));  // to the front with the keyboard, which on macOS takes making the app active
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
    unpick();
    C.place_start = C.place_end = 0;
    C.busy = C.copied = C.close_now = false;
    C.asking = 0;
    SDL_strlcpy(C.note, cleanup ? "Remember spellings to guide future cleanup. No automatic replacements or model retraining." : "Cleanup is off. Remembered spellings take effect when cleanup is enabled.", sizeof C.note);
    SDL_strlcpy(C.status, *C.text ? "" : "No saved transcript yet. Enable History and finish a dictation first.", sizeof C.status);
    eh_ui_focus(C.ui, AREA);
    SDL_ShowWindow(eh_ui_window(C.ui));
    gs_window_activate(eh_ui_window(C.ui));  // to the front with the keyboard, which on macOS takes making the app active
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
    hint(x, &y, w, "Select a word or phrase, then enter its correct spelling. Your original stays in History.");

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
    if (C.place_start >= 0) {
        gs_ui_textarea_select(gs, AREA, C.place_start, C.place_end);
        C.place_start = C.place_end = -1;
    }
    if (eh_w_textarea(C.ui, box, AREA, C.text, TEXT_MAX, NULL)) {
        // Changed by hand: nothing is selected for replacing, and the replacements made before it
        // can no longer be undone.
        unpick();
        clear_undo();
    } else if (gs_ui_focused(gs, AREA) && !C.busy) {
        capture();
    }
    y = box.y + box.h + 8;
    char picked_hint[400];
    if (picked()) {
        int n = pick_end() - pick_start();
        SDL_snprintf(picked_hint, sizeof picked_hint, "Selected: “%.*s%s”. Press Tab to correct its spelling.", SDL_min(n, 120), C.text + pick_start(), n > 120 ? "…" : "");
    } else {
        SDL_strlcpy(picked_hint, "Select text, then press Tab to correct it. You can also edit the transcript directly.", sizeof picked_hint);
    }
    eh_w_line(C.ui, (gs_rect){ x, y, w, 18 }, picked_hint, 12, gs_rgba(EH_TEXT_DIM, 1), -1);
    y += hint_h;

    label(x, &y, w, "Correct spelling");
    eh_w_field(C.ui, (gs_rect){ x, y, w, 36 }, "spelling", C.spelling, 121, NULL, 0);
    // Enter in the field replaces and remembers; Escape there gives up the pick.
    bool can = picked() && !C.busy;
    char *typed = eh_trim(C.spelling);
    can = can && *typed;
    SDL_free(typed);
    if (spelling_focused && gs_ui_key(gs, SDLK_RETURN, 0) && can) replace(true);
    if (spelling_focused && eh_ui_escaped(C.ui) && !eh_ui_asking(C.ui)) {
        // The cursor goes back to where the selection began.
        C.place_start = C.place_end = picked() ? C.sel_start : 0;
        unpick();
        SDL_strlcpy(C.status, "Pending replacement cancelled.", sizeof C.status);
        eh_ui_focus(C.ui, AREA);
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
