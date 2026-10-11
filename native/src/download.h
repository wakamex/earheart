#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include "catalog.h"

typedef void eh_download_progress(void *user, long long received, long long total, const char *file);

// Downloads the model's files into this build's own models folder and marks it installed. Files
// already there and sound are kept, and a download cut short is resumed. Setting *stop gives it
// up. False with `error` set ("aborted" after a stop).
bool eh_model_download(const eh_model *m, SDL_AtomicInt *stop, eh_download_progress *progress, void *user, char *error, size_t error_size);
// Frees the model's disk space in this build's own folder; a copy in the Electron build's folder is left alone.
bool eh_model_remove(const eh_model *m, char *error, size_t error_size);
