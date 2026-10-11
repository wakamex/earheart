#include "stt.h"
#include <SDL3/SDL.h>
#include "paths.h"
#include "sherpa-onnx/c-api/c-api.h"

#define FEATURE_DIM 80  // Parakeet's mel-feature dimension, fixed by the model

static struct {
    SDL_SharedObject *lib;
    const SherpaOnnxOfflineRecognizer *(*create)(const SherpaOnnxOfflineRecognizerConfig *);
    void (*destroy)(const SherpaOnnxOfflineRecognizer *);
    const SherpaOnnxOfflineStream *(*stream)(const SherpaOnnxOfflineRecognizer *);
    void (*stream_free)(const SherpaOnnxOfflineStream *);
    void (*accept)(const SherpaOnnxOfflineStream *, int32_t, const float *, int32_t);
    void (*decode)(const SherpaOnnxOfflineRecognizer *, const SherpaOnnxOfflineStream *);
    const SherpaOnnxOfflineRecognizerResult *(*result)(const SherpaOnnxOfflineStream *);
    void (*result_free)(const SherpaOnnxOfflineRecognizerResult *);
    const SherpaOnnxOfflineRecognizer *recognizer;
    int threads;
    char error[512];
} S;

const char *eh_stt_error(void) { return S.error; }
int eh_stt_threads(void) { return S.threads; }

#ifdef _WIN32
#define SHERPA_LIB "sherpa-onnx-c-api.dll"
#elif defined(__APPLE__)
#define SHERPA_LIB "libsherpa-onnx-c-api.dylib"
#else
#define SHERPA_LIB "libsherpa-onnx-c-api.so"
#endif

static bool open_library(const char *engines_dir) {
    if (S.lib) return true;
    eh_engines_prepare(engines_dir);
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s" EH_SEP "%s", engines_dir, SHERPA_LIB);
    S.lib = SDL_LoadObject(path);
    if (!S.lib) { SDL_snprintf(S.error, sizeof S.error, "sherpa-onnx not available: %s", SDL_GetError()); return false; }
    struct { void *fn; const char *name; } syms[] = {
        { &S.create, "SherpaOnnxCreateOfflineRecognizer" }, { &S.destroy, "SherpaOnnxDestroyOfflineRecognizer" },
        { &S.stream, "SherpaOnnxCreateOfflineStream" }, { &S.stream_free, "SherpaOnnxDestroyOfflineStream" },
        { &S.accept, "SherpaOnnxAcceptWaveformOffline" }, { &S.decode, "SherpaOnnxDecodeOfflineStream" },
        { &S.result, "SherpaOnnxGetOfflineStreamResult" }, { &S.result_free, "SherpaOnnxDestroyOfflineRecognizerResult" },
    };
    for (size_t i = 0; i < SDL_arraysize(syms); i++) {
        SDL_FunctionPointer f = SDL_LoadFunction(S.lib, syms[i].name);
        if (!f) {
            SDL_snprintf(S.error, sizeof S.error, "sherpa-onnx not available: %s is missing", syms[i].name);
            SDL_UnloadObject(S.lib);
            S.lib = NULL;
            return false;
        }
        *(SDL_FunctionPointer *)syms[i].fn = f;
    }
    return true;
}

bool eh_stt_load(const char *engines_dir, const char *dir, const eh_stt_files *f) {
    // A joiner makes a transducer; Whisper has to say so. Anything else would load into the wrong
    // configuration, so it is refused before the current recognizer is dropped.
    const char *type = f->model_type;
    bool transducer = f->joiner && (!type || !SDL_strcmp(type, "transducer") || !SDL_strcmp(type, "nemo_transducer"));
    bool whisper = !f->joiner && type && !SDL_strcmp(type, "whisper");
    if (!transducer && !whisper) {
        SDL_snprintf(S.error, sizeof S.error, "Unsupported speech model configuration (%s%s%s, %s a joiner): Earheart runs transducer and Whisper models",
                     type ? "type \"" : "no model type", type ? type : "", type ? "\"" : "", f->joiner ? "with" : "without");
        return false;
    }
    if (!open_library(engines_dir)) return false;
    eh_stt_unload();

    char encoder[1024], decoder[1024], joiner[1024], tokens[1024];
    SDL_snprintf(encoder, sizeof encoder, "%s/%s", dir, f->encoder);
    SDL_snprintf(decoder, sizeof decoder, "%s/%s", dir, f->decoder);
    SDL_snprintf(joiner, sizeof joiner, "%s/%s", dir, f->joiner ? f->joiner : "");
    SDL_snprintf(tokens, sizeof tokens, "%s/%s", dir, f->tokens);

    // Capped at 8: decoding is memory-bound and stops scaling there.
    int cores = SDL_GetNumLogicalCPUCores();
    S.threads = SDL_clamp(cores - 1, 1, 8);

    SherpaOnnxOfflineRecognizerConfig c = { 0 };
    c.feat_config.sample_rate = 16000;
    c.feat_config.feature_dim = FEATURE_DIM;
    if (transducer) {
        c.model_config.transducer.encoder = encoder;
        c.model_config.transducer.decoder = decoder;
        c.model_config.transducer.joiner = joiner;
    } else {
        c.model_config.whisper.encoder = encoder;
        c.model_config.whisper.decoder = decoder;
    }
    c.model_config.tokens = tokens;
    c.model_config.num_threads = S.threads;
    c.model_config.provider = "cpu";
    c.model_config.model_type = transducer ? (type ? type : "nemo_transducer") : "whisper";
    S.recognizer = S.create(&c);
    if (!S.recognizer) { SDL_snprintf(S.error, sizeof S.error, "The speech model in %s could not be loaded", dir); return false; }
    return true;
}

char *eh_stt_transcribe(const float *samples, int frames, int rate, int *decode_ms) {
    if (!S.recognizer) { SDL_strlcpy(S.error, "STT model not loaded", sizeof S.error); return NULL; }
    const SherpaOnnxOfflineStream *st = S.stream(S.recognizer);
    S.accept(st, rate, samples, frames);
    // Timed here, where a model load or a queued request cannot leak in: this figure paces the
    // overlay's transcribing bar.
    Uint64 t0 = SDL_GetTicks();
    S.decode(S.recognizer, st);
    if (decode_ms) *decode_ms = (int)(SDL_GetTicks() - t0);
    const SherpaOnnxOfflineRecognizerResult *r = S.result(st);
    const char *text = r && r->text ? r->text : "";
    while (SDL_isspace((unsigned char)*text)) text++;
    size_t n = SDL_strlen(text);
    while (n && SDL_isspace((unsigned char)text[n - 1])) n--;
    char *out = SDL_strndup(text, n);
    if (r) S.result_free(r);
    S.stream_free(st);
    return out;
}

void eh_stt_unload(void) {
    if (S.recognizer) S.destroy(S.recognizer);
    S.recognizer = NULL;
}
