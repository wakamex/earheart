// macOS: Carbon's hotkeys, which need no permission; a posted Cmd+V for the paste keystroke,
// which needs Accessibility. The one-instance socket is in desk_unix.c.
#include <SDL3/SDL.h>
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include "desk.h"

/* ---------------- hotkeys ---------------- */

#define MAX_HOTKEYS 8

static struct {
    void (*fired)(int id);
    bool installed;
    struct { int id; EventHotKeyRef ref; } keys[MAX_HOTKEYS];
} M;

static OSStatus on_hotkey(EventHandlerCallRef next, EventRef event, void *user) {
    EventHotKeyID hk;
    if (!GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, NULL, sizeof hk, NULL, &hk)) M.fired((int)hk.id);
    return noErr;
}

bool eh_hotkeys_init(void (*fired)(int id), char *error, size_t error_size) {
    M.fired = fired;
    return true;
}

// Virtual key codes are positions on an ANSI keyboard, in no order worth computing.
static int keycode_of(const char *key) {
    static const struct { const char *name; int code; } table[] = {
        { "a", 0 }, { "s", 1 }, { "d", 2 }, { "f", 3 }, { "h", 4 }, { "g", 5 }, { "z", 6 }, { "x", 7 }, { "c", 8 }, { "v", 9 }, { "b", 11 }, { "q", 12 },
        { "w", 13 }, { "e", 14 }, { "r", 15 }, { "y", 16 }, { "t", 17 }, { "1", 18 }, { "2", 19 }, { "3", 20 }, { "4", 21 }, { "6", 22 }, { "5", 23 },
        { "=", 24 }, { "9", 25 }, { "7", 26 }, { "-", 27 }, { "8", 28 }, { "0", 29 }, { "]", 30 }, { "o", 31 }, { "u", 32 }, { "[", 33 }, { "i", 34 },
        { "p", 35 }, { "l", 37 }, { "j", 38 }, { "'", 39 }, { "k", 40 }, { ";", 41 }, { "\\", 42 }, { ",", 43 }, { "/", 44 }, { "n", 45 }, { "m", 46 },
        { ".", 47 }, { "`", 50 }, { "plus", 24 },
        { "enter", 36 }, { "return", 36 }, { "tab", 48 }, { "space", 49 }, { "backspace", 51 }, { "escape", 53 }, { "esc", 53 }, { "capslock", 57 },
        { "numdec", 65 }, { "nummult", 67 }, { "numadd", 69 }, { "numdiv", 75 }, { "numsub", 78 },
        { "num0", 82 }, { "num1", 83 }, { "num2", 84 }, { "num3", 85 }, { "num4", 86 }, { "num5", 87 }, { "num6", 88 }, { "num7", 89 }, { "num8", 91 }, { "num9", 92 },
        { "f5", 96 }, { "f6", 97 }, { "f7", 98 }, { "f3", 99 }, { "f8", 100 }, { "f9", 101 }, { "f11", 103 }, { "f13", 105 }, { "f16", 106 }, { "f14", 107 },
        { "f10", 109 }, { "f12", 111 }, { "f15", 113 }, { "insert", 114 }, { "home", 115 }, { "pageup", 116 }, { "delete", 117 }, { "f4", 118 }, { "end", 119 },
        { "f2", 120 }, { "pagedown", 121 }, { "f1", 122 }, { "left", 123 }, { "right", 124 }, { "down", 125 }, { "up", 126 },
        { "f17", 64 }, { "f18", 79 }, { "f19", 80 }, { "f20", 90 },
    };
    for (size_t i = 0; i < SDL_arraysize(table); i++)
        if (!SDL_strcmp(key, table[i].name)) return table[i].code;
    return -1;
}

typedef struct { int id; const eh_accelerator *a; bool ok, unregister; } hotkey_request;

// On the main thread, whose event loop delivers the hotkey events.
static void apply(void *user) {
    hotkey_request *r = user;
    if (!M.installed) {
        const EventTypeSpec spec = { kEventClassKeyboard, kEventHotKeyPressed };
        InstallApplicationEventHandler(on_hotkey, 1, &spec, NULL, NULL);
        M.installed = true;
    }
    int slot = -1;
    for (int i = 0; i < MAX_HOTKEYS; i++) {
        if (M.keys[i].id != r->id) continue;
        UnregisterEventHotKey(M.keys[i].ref);
        M.keys[i].id = 0;
    }
    for (int i = 0; i < MAX_HOTKEYS && slot < 0; i++)
        if (!M.keys[i].id) slot = i;
    if (r->unregister) { r->ok = true; return; }
    int code = keycode_of(r->a->key);
    UInt32 mods = (r->a->mods & EH_MOD_SUPER ? cmdKey : 0) | (r->a->mods & EH_MOD_SHIFT ? shiftKey : 0) | (r->a->mods & EH_MOD_ALT ? optionKey : 0) | (r->a->mods & EH_MOD_CTRL ? controlKey : 0);
    EventHotKeyID hk = { 'EHRT', (UInt32)r->id };
    r->ok = code >= 0 && slot >= 0 && RegisterEventHotKey((UInt32)code, mods, hk, GetApplicationEventTarget(), 0, &M.keys[slot].ref) == noErr;
    if (r->ok) M.keys[slot].id = r->id;
}

bool eh_hotkey_register(int id, const eh_accelerator *a, char *error, size_t error_size) {
    hotkey_request r = { id, a, false, false };
    if (!SDL_RunOnMainThread(apply, &r, true) || !r.ok) {
        SDL_strlcpy(error, keycode_of(a->key) < 0 ? "this keyboard has no such key" : "already in use by another app", error_size);
        return false;
    }
    return true;
}

void eh_hotkey_unregister(int id) {
    hotkey_request r = { id, NULL, false, true };
    SDL_RunOnMainThread(apply, &r, true);
}

/* ---------------- paste ---------------- */

bool eh_paste_permitted(void) { return AXIsProcessTrusted(); }

eh_paste_result eh_paste_keystroke(char *detail, size_t detail_size) {
    // Without Accessibility the events are dropped without a word, so it is asked first.
    if (!AXIsProcessTrusted()) return EH_PASTE_NOT_PERMITTED;
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
    CGEventRef down = CGEventCreateKeyboardEvent(source, kVK_ANSI_V, true), up = CGEventCreateKeyboardEvent(source, kVK_ANSI_V, false);
    bool ok = down && up;
    if (ok) {
        CGEventSetFlags(down, kCGEventFlagMaskCommand);
        CGEventSetFlags(up, kCGEventFlagMaskCommand);
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
    } else {
        SDL_strlcpy(detail, "the keyboard events could not be made", detail_size);
    }
    if (down) CFRelease(down);
    if (up) CFRelease(up);
    if (source) CFRelease(source);
    return ok ? EH_PASTE_SENT : EH_PASTE_FAILED;
}
