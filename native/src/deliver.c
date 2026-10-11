// Delivery: the text goes on the clipboard and, in the paste modes, a paste keystroke goes to the
// app that has focus; in the plain paste mode the clipboard then gets its old contents back. If
// the keystroke cannot be sent the text is still on the clipboard, and the result says so.
// Ported from main/output/deliver.js. Called by a pipeline task holding the loop's lock; the
// clipboard itself is touched on the main thread, which SDL requires.
#include "deliver.h"
#include <SDL3/SDL.h>
#include "desk.h"
#include "loop.h"

#define RESTORE_WINDOW_MS 1000  // how long after the keystroke the old clipboard is put back

// Everything the clipboard held, in every format it offered: a screenshot, a file copy or rich
// text read as "" when asked for text, and writing that back would destroy them.
typedef struct {
    char **types;
    void **data;
    size_t *sizes;
    int count;
} snapshot;

static struct {
    unsigned timer;
    snapshot *snap;  // the contents a pending restore will put back
    char *text;      // and the transcript that replaced them
} D;

static void free_snapshot(snapshot *s) {
    if (!s) return;
    for (int i = 0; i < s->count; i++) { SDL_free(s->types[i]); SDL_free(s->data[i]); }
    SDL_free(s->types);
    SDL_free(s->data);
    SDL_free(s->sizes);
    SDL_free(s);
}

/* ---------------- on the main thread ---------------- */

typedef struct { const char *text; snapshot *snap; bool take, restore, ok; char *read; } clipboard_job;

static const void *offer(void *user, const char *mime, size_t *size) {
    snapshot *s = user;
    for (int i = 0; i < s->count; i++)
        if (!SDL_strcmp(s->types[i], mime)) { *size = s->sizes[i]; return s->data[i]; }
    *size = 0;
    return NULL;
}

static void offer_done(void *user) { free_snapshot(user); }

static void clipboard_main(void *user) {
    clipboard_job *j = user;
    if (j->take) {
        // NULL when there is nothing to capture, so the caller skips the restore and never
        // overwrites the user's contents with nothing.
        size_t n = 0;
        char **types = SDL_GetClipboardMimeTypes(&n);
        snapshot *s = SDL_calloc(1, sizeof *s);
        s->types = SDL_calloc(n + 1, sizeof *s->types);
        s->data = SDL_calloc(n + 1, sizeof *s->data);
        s->sizes = SDL_calloc(n + 1, sizeof *s->sizes);
        for (size_t i = 0; types && i < n; i++) {
            size_t size = 0;
            void *data = SDL_GetClipboardData(types[i], &size);
            if (!data || !size) { SDL_free(data); continue; }
            s->types[s->count] = SDL_strdup(types[i]);
            s->data[s->count] = data;
            s->sizes[s->count++] = size;
        }
        SDL_free(types);
        if (!s->count) { free_snapshot(s); s = NULL; }
        j->snap = s;
    }
    if (j->restore) {
        // Only when the clipboard still holds the transcript: if the user copied something else
        // meanwhile, that is theirs.
        char *now = SDL_GetClipboardText();
        bool unchanged = now && !SDL_strcmp(now, j->text);
        SDL_free(now);
        if (unchanged) {
            snapshot *s = j->snap;
            j->snap = NULL;
            if (!SDL_SetClipboardData(offer, offer_done, s, (const char **)s->types, (size_t)s->count)) SDL_Log("clipboard restore failed: %s", SDL_GetError());
        }
        return;
    }
    if (j->text) j->ok = SDL_SetClipboardText(j->text);
}

static void on_main(clipboard_job *j) { EH_AWAIT(SDL_RunOnMainThread(clipboard_main, j, true)); }

static void restore_if_unchanged(snapshot *snap, const char *text) {
    clipboard_job j = { .text = text, .snap = snap, .restore = true };
    on_main(&j);
    free_snapshot(j.snap);  // not restored: the copy is dropped
}

static void restore_task(void *user) {
    D.timer = 0;
    snapshot *snap = D.snap;
    char *text = D.text;
    D.snap = NULL;
    D.text = NULL;
    if (snap) restore_if_unchanged(snap, text);
    SDL_free(text);
}

bool eh_deliver(const char *text, const eh_jv *output, eh_signal *signal, eh_delivery *result, char *error, size_t error_size) {
    *result = (eh_delivery){ 0 };
    if (eh_signal_aborted(signal)) { SDL_strlcpy(result->method, "cancelled", sizeof result->method); return true; }
    // A restore still pending from the dictation before runs now, so this one's snapshot is of
    // the user's contents and not of the last transcript.
    if (D.snap) {
        eh_cancel(D.timer);
        restore_task(NULL);
    }
    const char *mode = eh_jv_string(eh_jv_get(output, "mode"), "paste");
    bool paste_only = !SDL_strcmp(mode, "paste"), pasting = paste_only || !SDL_strcmp(mode, "paste-copy");
    clipboard_job j = { .text = text, .take = paste_only && eh_jv_boolean(eh_jv_get(output, "restoreClipboard"), true) };
    on_main(&j);
    snapshot *previous = j.snap;
    if (!j.ok) {
        SDL_snprintf(error, error_size, "The clipboard could not be written: %s", SDL_GetError());
        free_snapshot(previous);
        return false;
    }
    SDL_strlcpy(result->method, "clipboard", sizeof result->method);
    if (!pasting) return true;

    double delay = eh_jv_number(eh_jv_get(output, "pasteDelayMs"), 150);
    EH_AWAIT(SDL_Delay((Uint32)SDL_max(0, delay)));
    if (eh_signal_aborted(signal)) {
        if (previous) restore_if_unchanged(previous, text);
        SDL_strlcpy(result->method, "cancelled", sizeof result->method);
        return true;
    }
    char detail[256] = "";
    eh_paste_result sent;
    EH_AWAIT(sent = eh_paste_keystroke(detail, sizeof detail));
    if (sent != EH_PASTE_SENT) {
        // The text stays on the clipboard, so the snapshot is not put back over it.
        free_snapshot(previous);
        if (eh_signal_aborted(signal)) { SDL_strlcpy(result->method, "cancelled", sizeof result->method); return true; }
        const char *note, *hint;
        if (sent == EH_PASTE_NOT_PERMITTED) {
            note = "Accessibility permission is off";
            hint = "Accessibility is off for Earheart (an update can reset it) — Settings ▸ General ▸ Fix auto-paste permission";
        } else if (sent == EH_PASTE_NO_TOOL) {
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
        free_snapshot(previous);
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
