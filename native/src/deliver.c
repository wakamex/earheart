// Delivery: the text goes on the clipboard and, in the paste modes, a paste keystroke goes to the
// app that has focus; in the plain paste mode the clipboard then gets its old contents back. If
// the keystroke cannot be sent the text is still on the clipboard, and the result says so.
// Ported from main/output/deliver.js. Called by a pipeline task holding the loop's lock; the
// clipboard itself is touched on the main thread, which SDL requires.
#include "deliver.h"
#include <SDL3/SDL.h>
#include "activate.h"
#include "gs_desk.h"
#include "loop.h"

#define RESTORE_WINDOW_MS 1000  // how long after the keystroke the old clipboard is put back

static struct {
    unsigned timer;
    gs_clip *snap;  // the contents a pending restore will put back
    char *text;     // and the transcript that replaced them
} D;

/* ---------------- on the main thread ---------------- */

typedef struct { const char *text; gs_clip *snap; bool take, restore, ok; } clipboard_job;

static void clipboard_main(void *user) {
    clipboard_job *j = user;
    // NULL when there is nothing to capture, so the caller skips the restore and never overwrites
    // the user's contents with nothing.
    if (j->take) j->snap = gs_clipboard_snapshot();
    if (j->restore) {
        // Only when the clipboard still holds the transcript: if the user copied something else
        // meanwhile, that is theirs.
        char *now = SDL_GetClipboardText();
        bool unchanged = now && !SDL_strcmp(now, j->text);
        SDL_free(now);
        if (unchanged) {
            gs_clip *s = j->snap;
            j->snap = NULL;
            if (!gs_clipboard_restore(s)) SDL_Log("clipboard restore failed: %s", SDL_GetError());
        }
        return;
    }
    if (j->text) j->ok = SDL_SetClipboardText(j->text);
}

static void on_main(clipboard_job *j) { EH_AWAIT(SDL_RunOnMainThread(clipboard_main, j, true)); }

static void restore_if_unchanged(gs_clip *snap, const char *text) {
    clipboard_job j = { .text = text, .snap = snap, .restore = true };
    on_main(&j);
    gs_clipboard_free(j.snap);  // not restored: the copy is dropped
}

static void restore_task(void *user) {
    D.timer = 0;
    gs_clip *snap = D.snap;
    char *text = D.text;
    D.snap = NULL;
    D.text = NULL;
    if (snap) restore_if_unchanged(snap, text);
    SDL_free(text);
}

bool eh_deliver(const char *text, const gs_jval *output, eh_signal *signal, eh_delivery *result, char *error, size_t error_size) {
    *result = (eh_delivery){ 0 };
    if (eh_signal_aborted(signal)) { SDL_strlcpy(result->method, "cancelled", sizeof result->method); return true; }
    // A restore still pending from the dictation before runs now, so this one's snapshot is of
    // the user's contents and not of the last transcript.
    if (D.snap) {
        eh_cancel(D.timer);
        restore_task(NULL);
    }
    const char *mode = gs_jval_string(gs_jval_get(output, "mode"), "paste");
    bool paste_only = !SDL_strcmp(mode, "paste"), pasting = paste_only || !SDL_strcmp(mode, "paste-copy");
    clipboard_job j = { .text = text, .take = paste_only && gs_jval_boolean(gs_jval_get(output, "restoreClipboard"), true) };
    on_main(&j);
    gs_clip *previous = j.snap;
    if (!j.ok) {
        SDL_snprintf(error, error_size, "The clipboard could not be written: %s", SDL_GetError());
        gs_clipboard_free(previous);
        return false;
    }
    SDL_strlcpy(result->method, "clipboard", sizeof result->method);
    if (!pasting) return true;

    double delay = gs_jval_number(gs_jval_get(output, "pasteDelayMs"), 150);
    EH_AWAIT(SDL_Delay((Uint32)SDL_max(0, delay)));
    if (eh_signal_aborted(signal)) {
        if (previous) restore_if_unchanged(previous, text);
        SDL_strlcpy(result->method, "cancelled", sizeof result->method);
        return true;
    }
    char detail[256] = "";
    gs_paste_result sent;
    EH_AWAIT(sent = gs_paste_keystroke(detail, sizeof detail));
    if (sent != GS_PASTE_SENT) {
        // The text stays on the clipboard, so the snapshot is not put back over it.
        gs_clipboard_free(previous);
        if (eh_signal_aborted(signal)) { SDL_strlcpy(result->method, "cancelled", sizeof result->method); return true; }
        const char *note, *hint;
        if (sent == GS_PASTE_NOT_PERMITTED) {
            // Asked once a run: the system's prompt leads to the switch in System Settings.
            static bool asked;
            if (!asked) { asked = true; gs_paste_ask_permission(); }
            note = "Accessibility permission is off";
            hint = "Accessibility is off for Earheart (an update can reset it) — Settings ▸ General ▸ Fix auto-paste permission";
        } else if (sent == GS_PASTE_NO_TOOL) {
            note = "No keystroke tool found";
            hint = "Install wtype, ydotool or xdotool for auto-paste; until then, paste with Ctrl+V";
        } else {
#ifdef __APPLE__
            note = "Paste it with ⌘V";
            hint = "Paste it with ⌘V. What the paste tool reported is in Settings ▸ Advanced ▸ Open error log";
#else
            note = "Paste it with Ctrl+V";
            hint = "Paste it with Ctrl+V. What the paste tool reported is in Settings ▸ Advanced ▸ Open error log";
#endif
        }
        SDL_Log("auto-paste failed: %s; tool output: %s", hint, detail);
        SDL_strlcpy(result->note, note, sizeof result->note);
        SDL_strlcpy(result->hint, hint, sizeof result->hint);
        return true;
    }
    if (eh_signal_aborted(signal)) {
        gs_clipboard_free(previous);
        SDL_strlcpy(result->method, "cancelled", sizeof result->method);
        return true;
    }
    if (previous) {
        // Put back a moment later, once the target app has read the transcript.
        D.snap = previous;
        D.text = SDL_strdup(text);
        D.timer = eh_after(RESTORE_WINDOW_MS, restore_task, NULL);
    }
    SDL_strlcpy(result->method, mode, sizeof result->method);
    return true;
}
