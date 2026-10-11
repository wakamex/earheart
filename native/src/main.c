// Earheart: hotkey-driven voice dictation, fully local.
//   earheart transcribe [--models DIR] [--model ID] [--json] FILE.wav...   print each file's transcript
//   earheart clean [--models DIR] [--model ID] [--cpu] [--system FILE] [--temperature T] [--tokens] TEXT
//                                             print TEXT cleaned up ("-" reads it from standard input);
//                                             --tokens prints the prompt's tokens instead, for tests
//   earheart --engine stt|cleanup             an engine worker, started by the app (see engine.h)
// Both commands go through the engine workers, as dictation does.
#include <SDL3/SDL.h>
#include <stdio.h>
#include "catalog.h"
#include "cleanup.h"
#include "engine.h"
#include "paths.h"

static int usage(void) {
    fprintf(stderr, "usage: earheart transcribe [--models DIR] [--model ID] [--json] FILE.wav...\n"
                    "       earheart clean [--models DIR] [--model ID] [--cpu] [--system FILE] [--temperature T] [--tokens] TEXT\n");
    return 2;
}

static int fail(eh_engine_msg *m) {
    fprintf(stderr, "earheart: %s\n", gs_json_str(gs_json_get(m->v, "error"), "the engine gave no reason"));
    gs_json_free(m->doc);
    return 1;
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

    eh_engine *e = eh_engine_new("stt");
    eh_engine_msg msg;
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", "load-stt");
    eh_jw_str(&w, "modelId", m->id);
    eh_jw_str(&w, "dir", dir);
    eh_jw_str(&w, "encoder", m->sherpa.encoder);
    eh_jw_str(&w, "decoder", m->sherpa.decoder);
    eh_jw_str(&w, "joiner", m->sherpa.joiner);
    eh_jw_str(&w, "tokens", m->sherpa.tokens);
    eh_jw_str(&w, "modelType", m->sherpa.model_type);
    if (!eh_engine_wait(e, eh_engine_send(e, &w, NULL, 0), &msg, NULL, NULL)) return fail(&msg);
    int threads = (int)gs_json_num(gs_json_get(msg.v, "numThreads"), 0);
    gs_json_free(msg.doc);

    int failed = 0;
    for (; i < argc; i++) {
        size_t len;
        void *wav = SDL_LoadFile(argv[i], &len);
        if (!wav) { fprintf(stderr, "earheart: %s: %s\n", argv[i], SDL_GetError()); failed = 1; continue; }
        eh_jw_begin(&w);
        eh_jw_str(&w, "type", "transcribe");
        bool ok = eh_engine_wait(e, eh_engine_send(e, &w, wav, len), &msg, NULL, NULL);
        SDL_free(wav);
        if (!ok) { fprintf(stderr, "earheart: %s: ", argv[i]); failed = fail(&msg); continue; }
        const char *text = gs_json_str(gs_json_get(msg.v, "text"), "");
        if (json) {
            eh_jw_begin(&w);
            eh_jw_str(&w, "file", argv[i]);
            eh_jw_str(&w, "text", text);
            eh_jw_num(&w, "decodeMs", gs_json_num(gs_json_get(msg.v, "decodeMs"), 0));
            eh_jw_num(&w, "threads", threads);
            char *line = eh_jw_end(&w);
            puts(line);
            SDL_free(line);
        } else {
            puts(text);
        }
        fflush(stdout);
        gs_json_free(msg.doc);
    }
    eh_engine_free(e);
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
    double temperature = -1;
    bool cpu = false, tokens = false;
    int i = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!SDL_strcmp(argv[i], "--models") && i + 1 < argc) models = argv[++i];
        else if (!SDL_strcmp(argv[i], "--model") && i + 1 < argc) id = argv[++i];
        else if (!SDL_strcmp(argv[i], "--system") && i + 1 < argc) system_file = argv[++i];
        else if (!SDL_strcmp(argv[i], "--temperature") && i + 1 < argc) temperature = SDL_atof(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--cpu")) cpu = true;
        else if (!SDL_strcmp(argv[i], "--tokens")) tokens = true;
        else return usage();
    }
    if (i != argc - 1 || !models) return usage();
    const eh_model *m = eh_catalog_find(EH_MODEL_CLEANUP, id);
    if (!m) { fprintf(stderr, "earheart: no cleanup model \"%s\"\n", id); return 2; }
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/cleanup/%s/%s", models, m->id, m->gguf);
    char *text = SDL_strcmp(argv[i], "-") ? SDL_strdup(argv[i]) : read_stdin();
    char *system_prompt = system_file ? SDL_LoadFile(system_file, NULL) : SDL_strdup("SYSTEM PROMPT");
    if (!system_prompt) { fprintf(stderr, "earheart: %s: %s\n", system_file, SDL_GetError()); return 1; }

    if (tokens) {
        if (!eh_cleanup_load(eh_engines_dir(), path, 0, true)) { fprintf(stderr, "earheart: %s\n", eh_cleanup_error()); return 1; }
        char *turn = eh_cleanup_turn(system_prompt, text);
        int n;
        int *t = eh_cleanup_prompt_tokens(turn, &n);
        for (int k = 0; k < n; k++) printf(k ? " %d" : "%d", t[k]);
        putchar('\n');
        return 0;
    }

    eh_engine *e = eh_engine_new("cleanup");
    eh_engine_msg msg;
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", "clean");
    eh_jw_str(&w, "modelPath", path);
    eh_jw_bool(&w, "cpuOnly", cpu);
    eh_jw_str(&w, "systemPrompt", system_prompt);
    eh_jw_str(&w, "transcript", text);
    if (temperature >= 0) eh_jw_num(&w, "temperature", temperature);
    Uint64 t0 = SDL_GetTicks();
    if (!eh_engine_wait(e, eh_engine_send(e, &w, NULL, 0), &msg, NULL, NULL)) return fail(&msg);
    puts(gs_json_str(gs_json_get(msg.v, "text"), ""));
    fprintf(stderr, "%d ms\n", (int)(SDL_GetTicks() - t0));
    gs_json_free(msg.doc);
    eh_engine_free(e);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !SDL_strcmp(argv[1], "--engine")) return eh_worker_main(argv[2]);
    if (argc >= 2 && !SDL_strcmp(argv[1], "transcribe")) return cmd_transcribe(argc - 2, argv + 2);
    if (argc >= 2 && !SDL_strcmp(argv[1], "clean")) return cmd_clean(argc - 2, argv + 2);
    return usage();
}
