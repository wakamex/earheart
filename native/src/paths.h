// Where the app's own files are.
#pragma once

const char *eh_exe_path(void);     // this executable, for starting the engine workers
const char *eh_engines_dir(void);
// Call before opening an engine library in `dir`, so the libraries it needs are found beside it.
void eh_engines_prepare(const char *dir);  // the engine libraries: $EARHEART_ENGINES, or `engines` beside the executable

// The separator the system's loader wants: LoadLibrary does not take forward slashes.
#ifdef _WIN32
#define EH_SEP "\\"
#else
#define EH_SEP "/"
#endif
