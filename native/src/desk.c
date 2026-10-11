#include "desk.h"
#include <SDL3/SDL.h>

static const struct { const char *name; int mod; const char *mac, *windows, *linux_; } MODS[] = {
    { "commandorcontrol", 0, "Cmd", "Ctrl", "Ctrl" }, { "cmdorctrl", 0, "Cmd", "Ctrl", "Ctrl" },
    { "command", EH_MOD_SUPER, "Cmd", "Cmd", "Cmd" }, { "cmd", EH_MOD_SUPER, "Cmd", "Cmd", "Cmd" },
    { "control", EH_MOD_CTRL, "Control", "Ctrl", "Ctrl" }, { "ctrl", EH_MOD_CTRL, "Control", "Ctrl", "Ctrl" },
    { "alt", EH_MOD_ALT, "Option", "Alt", "Alt" }, { "option", EH_MOD_ALT, "Option", "Alt", "Alt" },
    { "altgr", EH_MOD_ALT | EH_MOD_CTRL, "AltGr", "AltGr", "AltGr" },
    { "shift", EH_MOD_SHIFT, "Shift", "Shift", "Shift" },
    { "super", EH_MOD_SUPER, "Cmd", "Win", "Super" }, { "meta", EH_MOD_SUPER, "Cmd", "Win", "Super" },
};

// Keys that have a name. Letters, digits, F1 to F24 and punctuation marks stand for themselves.
static const char *const NAMED[] = {
    "space", "tab", "enter", "return", "escape", "esc", "backspace", "delete", "insert", "home", "end", "pageup", "pagedown",
    "up", "down", "left", "right", "plus", "capslock", "numlock", "scrolllock", "printscreen",
    "num0", "num1", "num2", "num3", "num4", "num5", "num6", "num7", "num8", "num9", "numdec", "numadd", "numsub", "nummult", "numdiv",
};

static void lower_trim(const char *from, size_t n, char *out, size_t size) {
    while (n && SDL_isspace((unsigned char)*from)) from++, n--;
    while (n && SDL_isspace((unsigned char)from[n - 1])) n--;
    size_t i = 0;
    for (; i < n && i + 1 < size; i++) out[i] = (char)SDL_tolower((unsigned char)from[i]);
    out[i] = 0;
}

static bool known_key(const char *key) {
    size_t n = SDL_strlen(key);
    if (n == 1) return (unsigned char)key[0] > 0x20 && (unsigned char)key[0] < 0x7f;
    if (key[0] == 'f' && n <= 3) {
        int f = SDL_atoi(key + 1);
        return f >= 1 && f <= 24;
    }
    for (size_t i = 0; i < SDL_arraysize(NAMED); i++)
        if (!SDL_strcmp(key, NAMED[i])) return true;
    return false;
}

bool eh_accelerator_parse(const char *text, eh_accelerator *out, char *error, size_t error_size) {
    *out = (eh_accelerator){ 0 };
    if (!text || !*text) { SDL_strlcpy(error, "no hotkey is set", error_size); return false; }
    for (const char *part = text; part;) {
        // "Ctrl++" is Ctrl and the plus key: a separator is a plus that is not the last character.
        const char *plus = SDL_strchr(part[0] == '+' ? part + 1 : part, '+');
        char token[32];
        lower_trim(part, plus ? (size_t)(plus - part) : SDL_strlen(part), token, sizeof token);
        part = plus ? plus + 1 : NULL;
        bool modifier = false;
        for (size_t i = 0; i < SDL_arraysize(MODS) && !modifier; i++) {
            if (SDL_strcmp(token, MODS[i].name)) continue;
            modifier = true;
#ifdef __APPLE__
            out->mods |= MODS[i].mod ? MODS[i].mod : EH_MOD_SUPER;
#else
            out->mods |= MODS[i].mod ? MODS[i].mod : EH_MOD_CTRL;
#endif
        }
        if (modifier) continue;
        if (!SDL_strcmp(token, "+")) SDL_strlcpy(token, "plus", sizeof token);
        if (!known_key(token) || out->key[0]) {
            SDL_snprintf(error, error_size, "\"%s\" is not a key combination Earheart can register", text);
            return false;
        }
        SDL_strlcpy(out->key, token, sizeof out->key);
    }
    if (!out->key[0]) { SDL_snprintf(error, error_size, "\"%s\" has no key besides its modifiers", text); return false; }
    return true;
}

void eh_accelerator_label(const char *text, char *out, size_t size) {
    out[0] = 0;
    if (!text) return;
    static const struct { const char *key, *label; } KEYS[] = {
        { "num0", "Numpad 0" }, { "num1", "Numpad 1" }, { "num2", "Numpad 2" }, { "num3", "Numpad 3" }, { "num4", "Numpad 4" },
        { "num5", "Numpad 5" }, { "num6", "Numpad 6" }, { "num7", "Numpad 7" }, { "num8", "Numpad 8" }, { "num9", "Numpad 9" },
        { "numdec", "Numpad Decimal" }, { "numadd", "Numpad +" }, { "numsub", "Numpad -" }, { "nummult", "Numpad *" }, { "numdiv", "Numpad /" },
    };
    for (const char *part = text; part && *text;) {
        const char *plus = SDL_strchr(part, '+');
        size_t n = plus ? (size_t)(plus - part) : SDL_strlen(part);
        char token[32], shown[32];
        lower_trim(part, n, token, sizeof token);
        // A part that is no modifier and no numpad key is shown as written, trimmed.
        size_t from = 0;
        while (from < n && SDL_isspace((unsigned char)part[from])) from++;
        size_t to = n;
        while (to > from && SDL_isspace((unsigned char)part[to - 1])) to--;
        SDL_strlcpy(shown, part + from, SDL_min(sizeof shown, to - from + 1));
        const char *label = shown;
        for (size_t i = 0; i < SDL_arraysize(MODS); i++) {
            if (SDL_strcmp(token, MODS[i].name)) continue;
#ifdef __APPLE__
            label = MODS[i].mac;
#elif defined(_WIN32)
            label = MODS[i].windows;
#else
            label = MODS[i].linux_;
#endif
        }
        for (size_t i = 0; i < SDL_arraysize(KEYS); i++)
            if (!SDL_strcmp(token, KEYS[i].key)) label = KEYS[i].label;
        if (out[0]) SDL_strlcat(out, "+", size);
        SDL_strlcat(out, label, size);
        part = plus ? plus + 1 : NULL;
    }
}
