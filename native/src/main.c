// Earheart: hotkey-driven voice dictation, fully local.
//   earheart transcribe [--models DIR] [--model ID] [--json] FILE.wav...   print each file's transcript
//   earheart clean [--models DIR] [--model ID] [--cpu] [--system FILE] [--temperature T] [--tokens] TEXT
//                                             print TEXT cleaned up ("-" reads it from standard input);
//                                             --tokens prints the prompt's tokens instead, for tests
#include <SDL3/SDL.h>
#include <stdio.h>
#include "catalog.h"
#include "cleanup.h"
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
    fprintf(stderr, "usage: earheart transcribe [--models DIR] [--model ID] [--json] FILE.wav...\n"
                    "       earheart clean [--models DIR] [--model ID] [--cpu] [--system FILE] [--temperature T] [--tokens] TEXT\n");
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

static char *read_stdin(void) {
    size_t len = 0, cap = 4096;
    char *buf = SDL_malloc(cap);
    for (size_t n; (n = fread(buf + len, 1, cap - len - 1, stdin)) > 0;)
        if ((len += n) + 1 == cap) buf = SDL_realloc(buf, cap *= 2);
    buf[len] = 0;
    return buf;
}

static int cmd_clean(int argc, char **argv) {
    const char *models = SDL_getenv("EARHEART_MODELS"), *id = NULL, *system_file = NULL;
    eh_sampling sampling = { .temperature = -1, .top_p = -1 };
    bool cpu = false, tokens = false;
    int i = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!SDL_strcmp(argv[i], "--models") && i + 1 < argc) models = argv[++i];
        else if (!SDL_strcmp(argv[i], "--model") && i + 1 < argc) id = argv[++i];
        else if (!SDL_strcmp(argv[i], "--system") && i + 1 < argc) system_file = argv[++i];
        else if (!SDL_strcmp(argv[i], "--temperature") && i + 1 < argc) sampling.temperature = (float)SDL_atof(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--cpu")) cpu = true;
        else if (!SDL_strcmp(argv[i], "--tokens")) tokens = true;
        else return usage();
    }
    if (i != argc - 1 || !models) return usage();
    const eh_model *m = eh_catalog_find(EH_MODEL_CLEANUP, id);
    if (!m) { fprintf(stderr, "earheart: no cleanup model \"%s\"\n", id); return 2; }
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/cleanup/%s/%s", models, m->id, m->gguf);
    if (!eh_cleanup_load(engines_dir(), path, 0, cpu)) { fprintf(stderr, "earheart: %s\n", eh_cleanup_error()); return 1; }

    char *text = SDL_strcmp(argv[i], "-") ? SDL_strdup(argv[i]) : read_stdin();
    char *system_prompt = system_file ? SDL_LoadFile(system_file, NULL) : SDL_strdup("SYSTEM PROMPT");
    if (!system_prompt) { fprintf(stderr, "earheart: %s: %s\n", system_file, SDL_GetError()); return 1; }
    int status = 0;
    if (tokens) {
        char *turn = eh_cleanup_turn(system_prompt, text);
        int n;
        int *t = eh_cleanup_prompt_tokens(turn, &n);
        for (int k = 0; k < n; k++) printf(k ? " %d" : "%d", t[k]);
        putchar('\n');
        SDL_free(t);
        SDL_free(turn);
    } else {
        Uint64 t0 = SDL_GetTicks();
        char *cleaned = eh_cleanup_clean(system_prompt, text, &sampling, NULL, NULL, NULL);
        if (cleaned) puts(cleaned);
        else { fprintf(stderr, "earheart: %s\n", eh_cleanup_error()); status = 1; }
        fprintf(stderr, "%s, %d ms\n", eh_cleanup_device(), (int)(SDL_GetTicks() - t0));
        SDL_free(cleaned);
    }
    SDL_free(system_prompt);
    SDL_free(text);
    eh_cleanup_unload();
    return status;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !SDL_strcmp(argv[1], "clean")) return cmd_clean(argc - 2, argv + 2);
    if (argc >= 2 && !SDL_strcmp(argv[1], "transcribe")) return cmd_transcribe(argc - 2, argv + 2);
    return usage();
}
