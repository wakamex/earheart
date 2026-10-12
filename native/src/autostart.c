#include "autostart.h"
#include <SDL3/SDL.h>
#include "files.h"
#include "paths.h"

#if defined(_WIN32)
#include <windows.h>

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE L"Earheart"

const char *eh_autostart_where(void) { return "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\Earheart"; }

bool eh_autostart_enabled(void) {
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, NULL, &size) == ERROR_SUCCESS;
}

bool eh_autostart_set(bool enabled, char *error, size_t error_size) {
    HKEY key;
    LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (s == ERROR_SUCCESS) {
        if (enabled) {
            // The program in quotes, as a path with spaces needs, and the flag that keeps it in the tray.
            wchar_t exe[1024], command[1100];
            MultiByteToWideChar(CP_UTF8, 0, eh_exe_path(), -1, exe, 1024);
            int n = swprintf(command, 1100, L"\"%ls\" --hidden", exe);
            s = RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)command, (DWORD)((size_t)(n + 1) * sizeof(wchar_t)));
        } else {
            s = RegDeleteValueW(key, RUN_VALUE);
            if (s == ERROR_FILE_NOT_FOUND) s = ERROR_SUCCESS;
        }
        RegCloseKey(key);
    }
    if (s != ERROR_SUCCESS) SDL_snprintf(error, error_size, "The login entry could not be changed (Windows error %ld)", (long)s);
    return s == ERROR_SUCCESS;
}

#else

static const char *entry_path(void) {
    static char path[1200];
    const char *home = SDL_getenv("HOME");
#if defined(__APPLE__)
    SDL_snprintf(path, sizeof path, "%s/Library/LaunchAgents/dev.wakamex.earheart.plist", home ? home : "");
#else
    const char *config = SDL_getenv("XDG_CONFIG_HOME");
    if (config && *config) SDL_snprintf(path, sizeof path, "%s/autostart/earheart.desktop", config);
    else SDL_snprintf(path, sizeof path, "%s/.config/autostart/earheart.desktop", home ? home : "");
#endif
    return path;
}

const char *eh_autostart_where(void) { return entry_path(); }
bool eh_autostart_enabled(void) { return SDL_GetPathInfo(entry_path(), NULL); }

#if defined(__APPLE__)
// The program's path as text inside XML.
static void xml_text(const char *s, char *out, size_t size) {
    size_t at = 0;
    for (; *s && at + 7 < size; s++) {
        const char *as = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : NULL;
        if (as) at += (size_t)SDL_snprintf(out + at, size - at, "%s", as);
        else out[at++] = *s;
    }
    out[at] = 0;
}
#else
// One argument of a desktop entry's Exec line, quoted as the Desktop Entry Specification asks: in
// double quotes, with a backslash before each of " ` $ and \, and % doubled.
static void exec_arg(const char *s, char *out, size_t size) {
    size_t at = 0;
    out[at++] = '"';
    for (; *s && at + 4 < size; s++) {
        if (*s == '%') out[at++] = '%';
        else if (*s == '"' || *s == '`' || *s == '$' || *s == '\\') out[at++] = '\\';
        out[at++] = *s;
    }
    out[at++] = '"';
    out[at] = 0;
}
#endif

bool eh_autostart_set(bool enabled, char *error, size_t error_size) {
    const char *path = entry_path();
    if (!enabled) {
        if (SDL_GetPathInfo(path, NULL) && !SDL_RemovePath(path)) {
            SDL_snprintf(error, error_size, "The login entry could not be removed: %s", SDL_GetError());
            return false;
        }
        return true;
    }
    char program[2400], text[4096];
#if defined(__APPLE__)
    xml_text(eh_exe_path(), program, sizeof program);
    int n = SDL_snprintf(text, sizeof text,
                         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                         "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                         "<plist version=\"1.0\">\n<dict>\n"
                         "    <key>Label</key><string>dev.wakamex.earheart</string>\n"
                         "    <key>ProgramArguments</key>\n    <array>\n        <string>%s</string>\n        <string>--hidden</string>\n    </array>\n"
                         "    <key>RunAtLoad</key><true/>\n"
                         "    <key>ProcessType</key><string>Interactive</string>\n"
                         "</dict>\n</plist>\n",
                         program);
#else
    exec_arg(eh_exe_path(), program, sizeof program);
    int n = SDL_snprintf(text, sizeof text,
                         "[Desktop Entry]\nType=Application\nName=Earheart\nComment=Private, hotkey-driven voice dictation\n"
                         "Exec=%s --hidden\nIcon=earheart\nTerminal=false\nX-GNOME-Autostart-enabled=true\n",
                         program);
#endif
    char why[256];
    if (!eh_write_file_atomic(path, text, (size_t)n, why, sizeof why)) {
        SDL_snprintf(error, error_size, "The login entry could not be written: %s", why);
        return false;
    }
    return true;
}

#endif
