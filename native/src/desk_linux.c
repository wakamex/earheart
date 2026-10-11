// Linux: hotkeys through an X connection of the app's own (libX11 is opened at run time, so the
// app builds and starts without it), and the paste keystroke through the tools the Electron build
// uses: wtype or ydotool on Wayland, xdotool on X11.
#define _DEFAULT_SOURCE
#include <SDL3/SDL.h>
#include <poll.h>
#include <unistd.h>
#include "desk.h"

/* ---------------- hotkeys ---------------- */

// The few Xlib types used, as Xlib lays them out, so no X header is needed to build.
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    void *display;
    unsigned long window, root, subwindow, time;
    int x, y, x_root, y_root;
    unsigned state, keycode;
    int same_screen;
} x_key_event;
typedef union { int type; x_key_event key; long pad[24]; } x_event;
typedef struct { int type; void *display; unsigned long resourceid, serial; unsigned char error_code, request_code, minor_code; } x_error_event;

#define X_KEY_PRESS 2
#define X_SHIFT 1u
#define X_LOCK 2u       // Caps Lock
#define X_CONTROL 4u
#define X_MOD1 8u       // Alt
#define X_MOD2 16u      // Num Lock
#define X_MOD4 64u      // Super
#define MAX_HOTKEYS 8

static struct {
    SDL_SharedObject *lib;
    void *display;
    unsigned long root;
    SDL_Mutex *lock;
    void (*fired)(int id);
    struct { int id; unsigned keycode, mods; } keys[MAX_HOTKEYS];
    SDL_AtomicInt refused;  // an X error arrived while grabbing
    void *(*open)(const char *);
    unsigned long (*default_root)(void *);
    unsigned char (*keysym_to_keycode)(void *, unsigned long);
    int (*grab)(void *, int, unsigned, unsigned long, int, int, int);
    int (*ungrab)(void *, int, unsigned, unsigned long);
    int (*sync)(void *, int);
    int (*pending)(void *);
    int (*next_event)(void *, x_event *);
    int (*connection)(void *);
    void *(*set_error_handler)(int (*)(void *, x_error_event *));
} X;

static int on_x_error(void *display, x_error_event *e) {
    SDL_SetAtomicInt(&X.refused, 1);
    return 0;
}

static int hotkey_thread(void *user) {
    struct pollfd fd = { X.connection(X.display), POLLIN, 0 };
    for (;;) {
        poll(&fd, 1, 500);
        int fired[MAX_HOTKEYS], count = 0;
        SDL_LockMutex(X.lock);
        while (X.pending(X.display)) {
            x_event e;
            X.next_event(X.display, &e);
            if (e.type != X_KEY_PRESS) continue;
            unsigned mods = e.key.state & (X_SHIFT | X_CONTROL | X_MOD1 | X_MOD4);
            for (int i = 0; i < MAX_HOTKEYS; i++)
                if (X.keys[i].id && X.keys[i].keycode == e.key.keycode && X.keys[i].mods == mods && count < MAX_HOTKEYS) fired[count++] = X.keys[i].id;
        }
        SDL_UnlockMutex(X.lock);
        for (int i = 0; i < count; i++) X.fired(fired[i]);
    }
    return 0;
}

bool eh_hotkeys_init(void (*fired)(int id), char *error, size_t error_size) {
    const char *why = "your desktop blocks global shortcuts";
    if (X.display) return true;
    X.lib = SDL_LoadObject("libX11.so.6");
    struct { void *fn; const char *name; } syms[] = {
        { &X.open, "XOpenDisplay" }, { &X.default_root, "XDefaultRootWindow" }, { &X.keysym_to_keycode, "XKeysymToKeycode" },
        { &X.grab, "XGrabKey" }, { &X.ungrab, "XUngrabKey" }, { &X.sync, "XSync" }, { &X.pending, "XPending" },
        { &X.next_event, "XNextEvent" }, { &X.connection, "XConnectionNumber" }, { &X.set_error_handler, "XSetErrorHandler" },
    };
    bool ok = X.lib != NULL;
    for (size_t i = 0; ok && i < SDL_arraysize(syms); i++) ok = (*(SDL_FunctionPointer *)syms[i].fn = SDL_LoadFunction(X.lib, syms[i].name)) != NULL;
    int (*init_threads)(void) = ok ? (int (*)(void))SDL_LoadFunction(X.lib, "XInitThreads") : NULL;
    if (init_threads) init_threads();
    // No X server to ask: a Wayland desktop without XWayland, or no desktop at all.
    X.display = ok ? X.open(NULL) : NULL;
    if (!X.display) { SDL_strlcpy(error, why, error_size); return false; }
    X.root = X.default_root(X.display);
    X.lock = SDL_CreateMutex();
    X.fired = fired;
    X.set_error_handler(on_x_error);
    SDL_DetachThread(SDL_CreateThread(hotkey_thread, "hotkeys", NULL));
    return true;
}

static unsigned long keysym_of(const char *key) {
    static const struct { const char *name; unsigned long sym; } named[] = {
        { "space", 0x20 }, { "tab", 0xff09 }, { "enter", 0xff0d }, { "return", 0xff0d }, { "escape", 0xff1b }, { "esc", 0xff1b },
        { "backspace", 0xff08 }, { "delete", 0xffff }, { "insert", 0xff63 }, { "home", 0xff50 }, { "end", 0xff57 },
        { "pageup", 0xff55 }, { "pagedown", 0xff56 }, { "up", 0xff52 }, { "down", 0xff54 }, { "left", 0xff51 }, { "right", 0xff53 },
        { "plus", 0x2b }, { "capslock", 0xffe5 }, { "numlock", 0xff7f }, { "scrolllock", 0xff14 }, { "printscreen", 0xff61 },
        { "numdec", 0xffae }, { "numadd", 0xffab }, { "numsub", 0xffad }, { "nummult", 0xffaa }, { "numdiv", 0xffaf },
    };
    size_t n = SDL_strlen(key);
    if (n == 1) return (unsigned char)key[0];  // letters (already lowercase), digits and punctuation are their own keysyms
    if (key[0] == 'f' && SDL_isdigit((unsigned char)key[1])) return 0xffbe + (unsigned long)SDL_atoi(key + 1) - 1;
    if (!SDL_strncmp(key, "num", 3) && SDL_isdigit((unsigned char)key[3]) && n == 4) return 0xffb0 + (unsigned long)(key[3] - '0');
    for (size_t i = 0; i < SDL_arraysize(named); i++)
        if (!SDL_strcmp(key, named[i].name)) return named[i].sym;
    return 0;
}

// The four ways Caps Lock and Num Lock can be set, each of which is a different grab to X.
static const unsigned LOCKS[] = { 0, X_LOCK, X_MOD2, X_LOCK | X_MOD2 };

static void ungrab(int id) {
    for (int i = 0; i < MAX_HOTKEYS; i++) {
        if (X.keys[i].id != id) continue;
        for (size_t k = 0; k < SDL_arraysize(LOCKS); k++) X.ungrab(X.display, (int)X.keys[i].keycode, X.keys[i].mods | LOCKS[k], X.root);
        X.keys[i].id = 0;
    }
    X.sync(X.display, 0);
}

void eh_hotkey_unregister(int id) {
    if (!X.display) return;
    SDL_LockMutex(X.lock);
    ungrab(id);
    SDL_UnlockMutex(X.lock);
}

bool eh_hotkey_register(int id, const eh_accelerator *a, char *error, size_t error_size) {
    if (!X.display) { SDL_strlcpy(error, "your desktop blocks global shortcuts", error_size); return false; }
    SDL_LockMutex(X.lock);
    ungrab(id);
    unsigned keycode = X.keysym_to_keycode(X.display, keysym_of(a->key));
    unsigned mods = (a->mods & EH_MOD_CTRL ? X_CONTROL : 0) | (a->mods & EH_MOD_SHIFT ? X_SHIFT : 0) | (a->mods & EH_MOD_ALT ? X_MOD1 : 0) | (a->mods & EH_MOD_SUPER ? X_MOD4 : 0);
    int slot = -1;
    for (int i = 0; i < MAX_HOTKEYS && slot < 0; i++)
        if (!X.keys[i].id) slot = i;
    bool ok = keycode && slot >= 0;
    if (ok) {
        // A grab another client already holds comes back as an error, some time later: the
        // errors are collected by a sync.
        X.sync(X.display, 0);
        SDL_SetAtomicInt(&X.refused, 0);
        for (size_t k = 0; k < SDL_arraysize(LOCKS); k++) X.grab(X.display, (int)keycode, mods | LOCKS[k], X.root, 0, 1, 1);
        X.sync(X.display, 0);
        ok = !SDL_GetAtomicInt(&X.refused);
        X.keys[slot].id = id;
        X.keys[slot].keycode = keycode;
        X.keys[slot].mods = mods;
        if (!ok) ungrab(id);
    }
    SDL_UnlockMutex(X.lock);
    if (!ok) SDL_strlcpy(error, keycode ? "already in use, or your desktop blocks global shortcuts" : "this keyboard has no such key", error_size);
    return ok;
}

/* ---------------- paste ---------------- */

static bool command_exists(const char *cmd) {
    const char *path = SDL_getenv("PATH");
    char candidate[1024];
    for (const char *dir = path; dir && *dir;) {
        const char *end = SDL_strchr(dir, ':');
        size_t n = end ? (size_t)(end - dir) : SDL_strlen(dir);
        SDL_snprintf(candidate, sizeof candidate, "%.*s/%s", (int)n, dir, cmd);
        if (!access(candidate, X_OK)) return true;
        dir = end ? end + 1 : NULL;
    }
    return false;
}

bool eh_paste_permitted(void) { return true; }

eh_paste_result eh_paste_keystroke(char *detail, size_t detail_size) {
    static const char *const wtype[] = { "wtype", "-M", "ctrl", "-k", "v", "-m", "ctrl", NULL };
    // ydotool works anywhere its daemon runs: 29 is the left Ctrl key and 47 is V.
    static const char *const ydotool[] = { "ydotool", "key", "29:1", "47:1", "47:0", "29:0", NULL };
    static const char *const xdotool[] = { "xdotool", "key", "--clearmodifiers", "ctrl+v", NULL };
    const char *session = SDL_getenv("XDG_SESSION_TYPE"), *display = SDL_getenv("WAYLAND_DISPLAY");
    bool wayland = (session && !SDL_strcmp(session, "wayland")) || (display && *display);
    const char *const *candidates[3];
    int count = 0;
    if (wayland) { candidates[count++] = wtype; candidates[count++] = ydotool; }
    candidates[count++] = xdotool;
    bool any = false;
    SDL_strlcpy(detail, "", detail_size);
    for (int i = 0; i < count; i++) {
        if (!command_exists(candidates[i][0])) continue;
        any = true;
        SDL_Process *p = SDL_CreateProcess(candidates[i], false);
        int code = -1;
        if (p && SDL_WaitProcess(p, true, &code) && code == 0) { SDL_DestroyProcess(p); return EH_PASTE_SENT; }
        SDL_snprintf(detail, detail_size, "%s exited with code %d", candidates[i][0], code);
        if (p) SDL_DestroyProcess(p);
    }
    if (!any) SDL_strlcpy(detail, "No keystroke tool found (install wtype, ydotool or xdotool)", detail_size);
    return any ? EH_PASTE_FAILED : EH_PASTE_NO_TOOL;
}
