// Speech-to-text through sherpa-onnx's C interface, loaded at run time from the engines folder so
// the app starts without it. One recognizer at a time, as in the Electron build's worker.
#pragma once
#include <stdbool.h>

typedef struct {
    const char *encoder, *decoder, *joiner;  // file names inside the model's folder; joiner NULL for Whisper
    const char *tokens;
    const char *model_type;                  // "nemo_transducer", "transducer", "whisper", or NULL
} eh_stt_files;

// Loads the model in `dir`. False with eh_stt_error() set when the library or the model cannot load.
bool eh_stt_load(const char *engines_dir, const char *dir, const eh_stt_files *files);
int eh_stt_threads(void);  // the thread count the recognizer was given: min(8, cores - 1), at least 1
// The transcript of mono floats at `rate`, trimmed (SDL_free it), and the decode's milliseconds. NULL on failure.
char *eh_stt_transcribe(const float *samples, int frames, int rate, int *decode_ms);
void eh_stt_unload(void);
const char *eh_stt_error(void);
