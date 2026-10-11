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

int eh_wav_frames(const uint8_t *buf, size_t len);  // sample frames in a PCM16 WAV; 0 when it is not one

// A mono PCM16 WAV file of the samples (SDL_free it); *len is its size.
uint8_t *eh_wav_encode(const int16_t *samples, int count, int rate, size_t *len);
// Floats to PCM16 as the pipeline does it: scaled by 32768, rounded and clamped, with `pad` frames
// of silence on each side (SDL_free the result, which holds count + 2 * pad samples).
int16_t *eh_wav_pcm16(const float *samples, int count, int pad);
// Floats to a WAV as the recorder does it: clamped to [-1, 1], scaled by 32768 below zero and
// 32767 above, and truncated.
uint8_t *eh_wav_encode_recorded(const float *samples, int count, int rate, size_t *len);
// The frames [from, to) of a mono PCM16 WAV as a new WAV, clamped to the recording. NULL when the
// file is not mono PCM16.
uint8_t *eh_wav_slice(const uint8_t *buf, size_t len, long long from, long long to, size_t *out_len);
