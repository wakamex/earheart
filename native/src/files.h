// Files the app keeps for the user.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// Writes the file whole: to a temporary file beside it, readable by its owner only, flushed to the
// disk, then renamed over the old one, so a crash leaves the old content or the new. The folder is
// made when it is missing. False with `error` set on failure.
bool eh_write_file_atomic(const char *path, const void *data, size_t len, char *error, size_t error_size);
bool eh_make_dirs(const char *path);  // the folder and its parents
