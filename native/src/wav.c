#include "wav.h"
#include <SDL3/SDL.h>
#include <string.h>

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

eh_wav_info eh_wav_parse(const uint8_t *buf, size_t len) {
    eh_wav_info w = { .format = 1, .channels = 1, .rate = EH_SAMPLE_RATE, .bits = 16 };
    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *id = buf + pos;
        size_t size = u32(buf + pos + 4), body = pos + 8;
        if (!memcmp(id, "fmt ", 4) && body + 16 <= len) {
            w.format = u16(buf + body);
            w.channels = u16(buf + body + 2);
            w.rate = (int)u32(buf + body + 4);
            w.bits = u16(buf + body + 14);
        } else if (!memcmp(id, "data", 4)) {
            w.data_offset = body;
            w.data_size = size < len - body ? size : len - body;
        }
        pos = body + size + (size & 1);  // chunks are word-aligned
    }
    return w;
}

float *eh_wav_floats(const uint8_t *buf, size_t len, int *frames, int *rate, const char **error) {
    if (len < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { *error = "Not a RIFF/WAVE file"; return NULL; }
    eh_wav_info w = eh_wav_parse(buf, len);
    if (!w.data_offset) { *error = "WAV has no data chunk"; return NULL; }
    if (w.format != 1 || w.bits != 16 || w.channels < 1) { *error = "Unsupported WAV format (need 16-bit PCM)"; return NULL; }
    int n = (int)(w.data_size / 2 / (size_t)w.channels);
    float *out = SDL_malloc(sizeof(float) * (size_t)(n ? n : 1));
    const uint8_t *p = buf + w.data_offset;
    for (int i = 0; i < n; i++) {
        int acc = 0;
        for (int c = 0; c < w.channels; c++, p += 2) acc += (int16_t)u16(p);
        // The same arithmetic as the Electron build, so both hand the recognizer the same floats.
        out[i] = w.channels == 1 ? (float)acc / 32768.0f : (float)((double)acc / w.channels / 32768.0);
    }
    *frames = n;
    *rate = w.rate;
    return out;
}

double eh_wav_seconds(const uint8_t *buf, size_t len) {
    eh_wav_info w = eh_wav_parse(buf, len);
    double s = (double)w.data_size / 2 / (w.channels > 1 ? w.channels : 1) / (w.rate > 1 ? w.rate : 1);
    return s > 0.01 ? s : 0.01;
}
