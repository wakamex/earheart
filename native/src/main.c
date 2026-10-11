// Earheart: hotkey-driven voice dictation, fully local.
//   earheart transcribe [--models DIR] [--model ID] [--json] FILE.wav...   print each file's transcript
#include <SDL3/SDL.h>
#include <stdio.h>
#include "catalog.h"
#include "stt.h"
#include "wav.h"

static const char *engines_dir(void) {
    static char dir[1024];
    const char *env = SDL_getenv("EARHEART_ENGINES");
    if (env && *env) return env;
    SDL_snprintf(dir, sizeof dir, "%sengines", SDL_GetBasePath());
    return dir;
}

static void print_json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 0x20) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static int usage(void) {
    fprintf(stderr, "usage: earheart transcribe [--models DIR] [--model ID] [--json] FILE.wav...\n");
    return 2;
}

static int cmd_transcribe(int argc, char **argv) {
    const char *models = SDL_getenv("EARHEART_MODELS"), *id = NULL;
    bool json = false;
    int i = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!SDL_strcmp(argv[i], "--models") && i + 1 < argc) models = argv[++i];
        else if (!SDL_strcmp(argv[i], "--model") && i + 1 < argc) id = argv[++i];
        else if (!SDL_strcmp(argv[i], "--json")) json = true;
        else return usage();
    }
    if (i == argc || !models) return usage();
    const eh_model *m = eh_catalog_find(EH_MODEL_STT, id);
    if (!m) { fprintf(stderr, "earheart: no speech model \"%s\"\n", id); return 2; }
    char dir[1024];
    SDL_snprintf(dir, sizeof dir, "%s/stt/%s", models, m->id);
    if (!eh_stt_load(engines_dir(), dir, &m->sherpa)) { fprintf(stderr, "earheart: %s\n", eh_stt_error()); return 1; }

    int failed = 0;
    for (; i < argc; i++) {
        size_t len;
        uint8_t *buf = SDL_LoadFile(argv[i], &len);
        const char *error = buf ? NULL : SDL_GetError();
        int frames = 0, rate = 0, ms = 0;
        float *samples = buf ? eh_wav_floats(buf, len, &frames, &rate, &error) : NULL;
        char *text = samples ? eh_stt_transcribe(samples, frames, rate, &ms) : NULL;
        if (!text) {
            fprintf(stderr, "earheart: %s: %s\n", argv[i], error ? error : eh_stt_error());
            failed = 1;
        } else if (json) {
            printf("{\"file\":");
            print_json_string(argv[i]);
            printf(",\"text\":");
            print_json_string(text);
            printf(",\"decodeMs\":%d,\"threads\":%d}\n", ms, eh_stt_threads());
        } else {
            puts(text);
        }
        fflush(stdout);
        SDL_free(text);
        SDL_free(samples);
        SDL_free(buf);
    }
    eh_stt_unload();
    return failed;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !SDL_strcmp(argv[1], "transcribe")) return cmd_transcribe(argc - 2, argv + 2);
    return usage();
}
