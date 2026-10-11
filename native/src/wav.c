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

int eh_wav_frames(const uint8_t *buf, size_t len) {
    if (len < 12 || memcmp(buf, "RIFF", 4)) return 0;
    eh_wav_info w = eh_wav_parse(buf, len);
    return (int)(w.data_size / 2 / (size_t)(w.channels > 1 ? w.channels : 1));
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// The canonical 44-byte mono PCM16 header.
static uint8_t *header(uint32_t data_size, int rate, size_t *len) {
    uint8_t *buf = SDL_malloc(44 + (size_t)data_size);
    memcpy(buf, "RIFF", 4); put32(buf + 4, 36 + data_size);
    memcpy(buf + 8, "WAVEfmt ", 8); put32(buf + 16, 16);
    put16(buf + 20, 1); put16(buf + 22, 1);
    put32(buf + 24, (uint32_t)rate); put32(buf + 28, (uint32_t)rate * 2);
    put16(buf + 32, 2); put16(buf + 34, 16);
    memcpy(buf + 36, "data", 4); put32(buf + 40, data_size);
    *len = 44 + (size_t)data_size;
    return buf;
}

uint8_t *eh_wav_encode(const int16_t *samples, int count, int rate, size_t *len) {
    uint8_t *buf = header((uint32_t)count * 2, rate, len);
    for (int i = 0; i < count; i++) put16(buf + 44 + i * 2, (uint16_t)samples[i]);
    return buf;
}

int16_t *eh_wav_pcm16(const float *samples, int count, int pad) {
    int16_t *pcm = SDL_calloc((size_t)(count + 2 * pad) + 1, sizeof(int16_t));
    for (int i = 0; i < count; i++) {
        double x = SDL_floor((double)samples[i] * 32768.0 + 0.5);
        pcm[pad + i] = (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
    }
    return pcm;
}

uint8_t *eh_wav_encode_recorded(const float *samples, int count, int rate, size_t *len) {
    uint8_t *buf = header((uint32_t)count * 2, rate, len);
    for (int i = 0; i < count; i++) {
        double s = samples[i] < -1 ? -1 : samples[i] > 1 ? 1 : (double)samples[i];
        put16(buf + 44 + i * 2, (uint16_t)(int16_t)(s < 0 ? s * 32768.0 : s * 32767.0));
    }
    return buf;
}

uint8_t *eh_wav_slice(const uint8_t *buf, size_t len, long long from, long long to, size_t *out_len) {
    eh_wav_info w = eh_wav_parse(buf, len);
    if (!w.data_offset || w.format != 1 || w.bits != 16 || w.channels != 1) return NULL;
    long long frames = (long long)(w.data_size / 2);
    from = from < 0 ? 0 : from > frames ? frames : from;
    to = to < from ? from : to > frames ? frames : to;
    uint8_t *out = header((uint32_t)(to - from) * 2, w.rate, out_len);
    memcpy(out + 44, buf + w.data_offset + from * 2, (size_t)(to - from) * 2);
    return out;
}
