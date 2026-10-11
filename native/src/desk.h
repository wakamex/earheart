// What the app does to the desktop around it and no library here does: global hotkeys, a paste
// keystroke into whichever app has focus, and being the one running instance. One implementation
// per system: desk_linux.c, desk_windows.c, desk_macos.c. These are requested of gesso
// (gesso-requests.txt); they live here until it has them.
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* ---------------- accelerators ---------------- */

// A key combination as Electron writes it ("CommandOrControl+Shift+Space"), taken apart.
enum { EH_MOD_CTRL = 1, EH_MOD_SHIFT = 2, EH_MOD_ALT = 4, EH_MOD_SUPER = 8 };  // SUPER: Cmd, the Windows key
typedef struct {
    int mods;
    char key[16];  // lowercase: a letter, a digit, "space", "f5", "enter", "up", "num3", a punctuation mark
} eh_accelerator;

// False with `error` set for an empty string, an unknown key, or no key at all.
bool eh_accelerator_parse(const char *text, eh_accelerator *out, char *error, size_t error_size);
// How the combination reads to a user on this system: "Ctrl+Shift+Space". "" for an unset hotkey.
void eh_accelerator_label(const char *text, char *out, size_t size);

/* ---------------- global hotkeys ---------------- */

// `fired` is called with the id of a hotkey when it is pressed, on a thread of the hotkeys' own.
// False with `error` set where the desktop offers no way to grab a key (Wayland without X).
bool eh_hotkeys_init(void (*fired)(int id), char *error, size_t error_size);
// Takes the combination for `id`, replacing what that id held. False with `error` set when the
// system refuses it, most often because another app holds it; the id then holds nothing.
bool eh_hotkey_register(int id, const eh_accelerator *accelerator, char *error, size_t error_size);
void eh_hotkey_unregister(int id);

/* ---------------- paste ---------------- */

typedef enum {
    EH_PASTE_SENT,
    EH_PASTE_NOT_PERMITTED,  // macOS: Accessibility is off for this app
    EH_PASTE_NO_TOOL,        // Linux: none of wtype, ydotool and xdotool is installed
    EH_PASTE_FAILED,
} eh_paste_result;

// Sends the paste chord (Ctrl+V; Cmd+V on macOS) to the app that has focus. Waits for it; call
// without the loop's lock. `detail` gets what a failing tool reported, for the log.
eh_paste_result eh_paste_keystroke(char *detail, size_t detail_size);
bool eh_paste_permitted(void);  // false only on macOS without Accessibility
// macOS: has the system ask the user for Accessibility, which also lists the app in System
// Settings so the user has something to switch on. Does nothing elsewhere.
void eh_paste_ask_permission(void);

/* ---------------- one instance ---------------- */

// Becomes the running instance. Arguments a later instance forwards arrive at `on_args`, on
// another thread. False when an instance is already running.
bool eh_instance_acquire(const char *data_dir, void (*on_args)(int argc, char **argv));
// Hands the arguments to the running instance. False when there is none.
bool eh_instance_forward(const char *data_dir, int argc, char **argv);
