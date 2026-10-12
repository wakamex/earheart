// Starting the app when the user logs in, hidden in the tray: an autostart entry on Linux, a value
// under the user's Run key on Windows, a launch agent on macOS. Each names this program where it is
// now, so it is written again whenever it is switched on.
#pragma once
#include <stdbool.h>
#include <stddef.h>

bool eh_autostart_enabled(void);
// False with the reason when the entry could not be written or removed.
bool eh_autostart_set(bool enabled, char *error, size_t error_size);
// Where the entry is kept, for saying so: a file's path, or the registry value's.
const char *eh_autostart_where(void);
