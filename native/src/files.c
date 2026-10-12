#include "files.h"
#include <stdlib.h>
#include <SDL3/SDL.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

bool eh_make_dirs(const char *path) { return SDL_CreateDirectory(path); }

bool eh_write_file_atomic(const char *path, const void *data, size_t len, char *error, size_t error_size) {
    char folder[1200], tmp[1200];
    SDL_strlcpy(folder, path, sizeof folder);
    char *slash = SDL_strrchr(folder, '/');
#ifdef _WIN32
    char *back = SDL_strrchr(folder, '\\');
    if (back > slash) slash = back;
#endif
    if (slash) { *slash = 0; eh_make_dirs(folder); }
    SDL_snprintf(tmp, sizeof tmp, "%s.%llu.tmp", path, (unsigned long long)SDL_GetCurrentThreadID());
    SDL_RemovePath(tmp);
    SDL_IOStream *io = SDL_IOFromFile(tmp, "wb");
    bool ok = io != NULL;
#ifndef _WIN32
    if (ok) chmod(tmp, 0600);  // before any content is in it
#endif
    ok = ok && SDL_WriteIO(io, data, len) == len && SDL_FlushIO(io);
    if (io) ok = SDL_CloseIO(io) && ok;
    ok = ok && SDL_RenamePath(tmp, path);
    if (!ok) {
        SDL_strlcpy(error, SDL_GetError(), error_size);
        SDL_RemovePath(tmp);
    }
    return ok;
}

char *eh_json_text(const gs_jval *v, bool pretty) {
    char *text = gs_jval_write(v, pretty), *copy = SDL_strdup(text ? text : "null");
    free(text);
    return copy;
}
