#define _DEFAULT_SOURCE  // readlink, realpath
#include "paths.h"
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdlib.h>
#else
#include <unistd.h>
#endif

const char *eh_exe_path(void) {
    static char path[2048];
    if (path[0]) return path;
#ifdef _WIN32
    WCHAR wide[1024];
    DWORD n = GetModuleFileNameW(NULL, wide, SDL_arraysize(wide));
    if (n && n < SDL_arraysize(wide)) WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, sizeof path, NULL, NULL);
#elif defined(__APPLE__)
    char raw[2048];
    uint32_t size = sizeof raw;
    if (!_NSGetExecutablePath(raw, &size) && !realpath(raw, path)) SDL_strlcpy(path, raw, sizeof path);
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    path[n > 0 ? n : 0] = 0;
#endif
    if (!path[0]) SDL_snprintf(path, sizeof path, "%searheart", SDL_GetBasePath());
    return path;
}

const char *eh_engines_dir(void) {
    static char dir[2048];
    const char *env = SDL_getenv("EARHEART_ENGINES");
    if (env && *env) return env;
    if (dir[0]) return dir;
    SDL_strlcpy(dir, eh_exe_path(), sizeof dir);
    char *slash = SDL_strrchr(dir, '/');
#ifdef _WIN32
    char *back = SDL_strrchr(dir, '\\');
    if (back > slash) slash = back;
#endif
    SDL_strlcpy(slash ? slash + 1 : dir, "engines", sizeof dir - (size_t)(slash ? slash + 1 - dir : 0));
    return dir;
}

void eh_engines_prepare(const char *dir) {
#ifdef _WIN32
    // LoadLibrary looks for a library's own imports in the program's folder and along PATH, not
    // beside the library. Without this, llama.dll's search for ggml.dll walks PATH and fails on
    // the first folder it may not traverse.
    WCHAR wide[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, wide, SDL_arraysize(wide))) SetDllDirectoryW(wide);
#else
    (void)dir;
#endif
}
