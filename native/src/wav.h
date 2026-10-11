// WAV files as the app records them: RIFF, PCM, 16 bits. Other chunks are skipped.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EH_SAMPLE_RATE 16000

typedef struct {
    int format, channels, rate, bits;
    size_t data_offset, data_size;  // data_offset is 0 when there is no data chunk
} eh_wav_info;

// Walks the chunk list. A missing fmt chunk leaves 16 kHz mono PCM16, for the caller to judge.
eh_wav_info eh_wav_parse(const uint8_t *buf, size_t len);

// The samples as floats in [-1, 1], channels averaged to mono (SDL_free them). NULL with *error set
// when the file is not RIFF/WAVE, has no data, or is not 16-bit PCM.
float *eh_wav_floats(const uint8_t *buf, size_t len, int *frames, int *rate, const char **error);

double eh_wav_seconds(const uint8_t *buf, size_t len);  // at least 0.01
