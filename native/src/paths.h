// Where the app's own files are.
#pragma once

const char *eh_exe_path(void);     // this executable, for starting the engine workers
const char *eh_engines_dir(void);
const char *eh_data_dir(void);      // this build's own folder for settings, history and models: $EARHEART_DATA, or the system's place for it
const char *eh_electron_dir(void);  // the Electron build's data folder, whether or not it exists
// Call before opening an engine library in `dir`, so the libraries it needs are found beside it.
void eh_engines_prepare(const char *dir);  // the engine libraries: $EARHEART_ENGINES, or `engines` beside the executable

// The separator the system's loader wants: LoadLibrary does not take forward slashes.
#ifdef _WIN32
#define EH_SEP "\\"
#else
#define EH_SEP "/"
#endif
