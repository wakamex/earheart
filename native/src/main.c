// Earheart: hotkey-driven voice dictation, fully local.
//   earheart [--hidden | --toggle | --pause | --discard | --smoke-test [--engine-check]]
//                                             the app, in the tray (run.c)
//   earheart transcribe [--models DIR] [--model ID] [--json] [--list FILE] FILE.wav...
//                                             print each file's transcript; --list names a file of
//                                             paths, one a line; --chunked decodes each as a dictation
//                                             is decoded: in pieces cut at the pauses (chunked.h)
//   earheart clean [--models DIR] [--model ID] [--cpu] [--system FILE] [--temperature T] [--tokens] TEXT
//                                             print TEXT cleaned up ("-" reads it from standard input);
//                                             --tokens prints the prompt's tokens instead, for tests
//   earheart clean [--models DIR] [--model ID] [--cpu] --batch
//                                             one clean per line of standard input, each a JSON object
//                                             {"systemPrompt", "transcript", "temperature", "topP", "topK",
//                                             "minP", "seed", "fresh"}, answered by a line {"text", "ms"}
//                                             or {"error", "ms"}; for the benchmark
//   earheart models [--models DIR] [list | download KIND/ID... | remove KIND/ID...]
//                                             the catalogue, and downloads into DIR
//   earheart dictate --wav FILE ...          a whole dictation with the file as the microphone (dictate.c)
//   earheart --engine stt|cleanup             an engine worker, started by the app (see engine.h)
// Both commands go through the engine workers, as dictation does.
#include <SDL3/SDL.h>
#include <stdio.h>
#include "catalog.h"
#include "chunked.h"
#include "jv.h"
#include "wav.h"
#include "cleanup.h"
#include "cores.h"
#include "download.h"
#include "hf.h"
#include "gs_rand.h"
#include "overlay.h"
#include "engine.h"
#include "paths.h"

static int usage(void);

static void print_progress(void *user, long long received, long long total, const char *file) {
    static Uint64 last;
    if (SDL_GetTicks() - last < 1000 && received < total) return;
    last = SDL_GetTicks();
    fprintf(stderr, "%s: %lld of %lld MB\n", file, received >> 20, total >> 20);
}

// earheart models [--models DIR] [list | download KIND/ID... | remove KIND/ID...]
static int cmd_models(int argc, char **argv) {
    const char *dir = SDL_getenv("EARHEART_MODELS");
    int i = 0;
    if (i + 1 < argc && !SDL_strcmp(argv[i], "--models")) { dir = argv[i + 1]; i += 2; }
    char own[1200];
    SDL_snprintf(own, sizeof own, "%s" EH_SEP "models", eh_data_dir());
    const char *dirs[] = { dir ? dir : own };
    eh_models_set_dirs(dirs, 1);
    // The user's own models, from the settings beside the models.
    char settings_path[1300];
    SDL_snprintf(settings_path, sizeof settings_path, "%s" EH_SEP "settings.json", eh_data_dir());
    size_t settings_len = 0;
    char *settings_text = SDL_LoadFile(settings_path, &settings_len);
    eh_jv *saved = settings_text ? eh_jv_parse(settings_text, settings_len) : NULL;
    eh_models_set_custom(eh_jv_get(saved, "customModels"));
    eh_jv_free(saved);
    SDL_free(settings_text);
    const char *verb = i < argc ? argv[i++] : "list";
    if (!SDL_strcmp(verb, "list")) {
        const eh_model *all[64];
        int total = eh_models_list(EH_MODEL_STT, all, 64);
        total += eh_models_list(EH_MODEL_CLEANUP, all + total, 64 - total);
        for (int k = 0; k < total; k++) {
            const eh_model *m = all[k];
            long long bytes = 0;
            for (int f = 0; f < m->file_count; f++) bytes += m->files[f].bytes;
            printf("%s/%s\t%s\t%lld MB\t%s%s\n", eh_model_kind_dir(m->kind), m->id, eh_model_installed(m, NULL, 0) ? "installed" : "-", bytes >> 20, m->label, m->is_default ? " (default)" : "");
        }
        return 0;
    }
    bool download = !SDL_strcmp(verb, "download");
    if (!SDL_strcmp(verb, "find")) {
        // models find stt|cleanup ADDRESS: what Hugging Face holds there that the app can run.
        if (i + 2 > argc) return usage();
        eh_model_kind kind = !SDL_strcmp(argv[i], "stt") ? EH_MODEL_STT : EH_MODEL_CLEANUP;
        char error[1400];
        eh_jv *listing = eh_hf_discover(kind, argv[i + 1], error, sizeof error);
        if (!listing) { fprintf(stderr, "earheart: %s\n", error); return 1; }
        const eh_jv *variants = eh_jv_get(listing, "variants");
        // With a version named: that version's model definition, as settings keep it.
        for (int k = 0; i + 2 < argc && k < eh_jv_count(variants); k++) {
            const eh_jv *v = eh_jv_at(variants, k);
            if (SDL_strcmp(eh_jv_string(eh_jv_get(v, "label"), ""), argv[i + 2])) continue;
            eh_jv *model = eh_hf_model(kind, eh_jv_string(eh_jv_get(listing, "repo"), ""), v);
            char *json = eh_jv_write(model, false);
            printf("%s\n", json);
            SDL_free(json);
            eh_jv_free(model);
            eh_jv_free(listing);
            return 0;
        }
        if (i + 2 < argc) { fprintf(stderr, "earheart: no version %s there\n", argv[i + 2]); return 1; }
        printf("%s at %s\n", eh_jv_string(eh_jv_get(listing, "repo"), ""), eh_jv_string(eh_jv_get(listing, "commit"), ""));
        for (int k = 0; k < eh_jv_count(variants); k++) {
            const eh_jv *v = eh_jv_at(variants, k);
            eh_jv *model = eh_hf_model(kind, eh_jv_string(eh_jv_get(listing, "repo"), ""), v);
            printf("  %-12s %8.0f MB  %d file%s  %s\n", eh_jv_string(eh_jv_get(v, "label"), ""), eh_jv_number(eh_jv_get(v, "totalBytes"), 0) / 1e6, eh_jv_count(eh_jv_get(v, "files")),
                   eh_jv_count(eh_jv_get(v, "files")) == 1 ? "" : "s", eh_jv_string(eh_jv_get(model, "id"), ""));
            eh_jv_free(model);
        }
        eh_jv_free(listing);
        return 0;
    }
    if (!download && SDL_strcmp(verb, "remove")) return usage();
    for (; i < argc; i++) {
        const char *slash = SDL_strchr(argv[i], '/');
        const eh_model *m = slash ? eh_catalog_find(!SDL_strncmp(argv[i], "stt/", 4) ? EH_MODEL_STT : EH_MODEL_CLEANUP, slash + 1) : NULL;
        char error[256];
        SDL_AtomicInt stop = { 0 };
        if (!m) { fprintf(stderr, "earheart: no model %s in the catalogue\n", argv[i]); return 2; }
        if (!(download ? eh_model_download(m, &stop, print_progress, NULL, error, sizeof error) : eh_model_remove(m, error, sizeof error))) {
            fprintf(stderr, "earheart: %s\n", error);
            return 1;
        }
        printf("%s: %s\n", argv[i], download ? "installed" : "removed");
    }
    return 0;
}

// earheart overlay-shot OUT.png STATUS [TITLE [DETAIL [CLEAN [RAW]]]]: one frame of the overlay as a
// picture, for tests and the README.
static int cmd_overlay_shot(int argc, char **argv) {
    if (argc < 2) return usage();
    eh_overlay_model m = { 0 };
    SDL_strlcpy(m.status, argv[1], sizeof m.status);
    SDL_strlcpy(m.title, argc > 2 ? argv[2] : "Listening…", sizeof m.title);
    SDL_strlcpy(m.detail, argc > 3 ? argv[3] : "", sizeof m.detail);
    m.clean = argc > 4 ? argv[4] : NULL;
    m.raw = argc > 5 ? argv[5] : NULL;
    SDL_strlcpy(m.timer, "0:07", sizeof m.timer);
    // A voice for the waveform: a few words' worth of levels.
    gs_mulberry rng = { 7 };
    for (int i = 0; i < 160; i++) m.levels[i] = (float)(0.01 + 0.12 * gs_mulberry_next(&rng) * (0.5 + 0.5 * SDL_sin(i * 0.21)));
    m.level_count = 160;
    m.progress = !SDL_strcmp(m.status, "transcribing") || !SDL_strcmp(m.status, "cleaning");
    m.fraction = 0.62f;
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "earheart: %s\n", SDL_GetError()); return 1; }
    bool ok = eh_overlay_shot(&m, argv[0], 2);
    SDL_Quit();
    return ok ? 0 : 1;
}

int eh_cmd_dictate(int argc, char **argv);  // dictate.c
int eh_cmd_settings_smoke(int argc, char **argv);  // window_smoke.c
int eh_cmd_wizard_smoke(int argc, char **argv);
int eh_cmd_correction_smoke(int argc, char **argv);
int eh_run(int argc, char **argv);          // run.c

static int usage(void) {
    fprintf(stderr, "usage: earheart [--hidden | --toggle | --pause | --discard]\n"
                    "       earheart transcribe [--models DIR] [--model ID] [--json] [--chunked] [--list FILE] FILE.wav...\n"
                    "       earheart clean [--models DIR] [--model ID] [--cpu] [--system FILE] [--temperature T] [--tokens] TEXT\n"
                    "       earheart clean [--models DIR] [--model ID] [--cpu] --batch < requests.jsonl\n");
    return 2;
}

static int fail(eh_engine_reply *r) {
    fprintf(stderr, "earheart: %s\n", r->error);
    eh_engine_reply_free(r);
    return 1;
}

typedef struct { eh_engine *e; int inputs, decode_ms; double max_seconds; } chunked_run;

static eh_decode_code chunked_decode(void *user, const uint8_t *wav, size_t len, char **text, int *decode_ms, char *error, size_t error_size) {
    chunked_run *c = user;
    c->inputs++;
    c->max_seconds = SDL_max(c->max_seconds, eh_wav_seconds(wav, len));
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", "transcribe");
    eh_engine_reply reply;
    eh_engine_status s = eh_engine_call(c->e, &w, wav, len, 0, NULL, NULL, &reply);
    eh_decode_code code = s == EH_ENGINE_OK ? EH_DECODE_OK : s == EH_ENGINE_EXITED ? EH_DECODE_EXITED : s == EH_ENGINE_TIMEOUT ? EH_DECODE_TIMEOUT : EH_DECODE_FAILED;
    if (code == EH_DECODE_OK) {
        *text = SDL_strdup(gs_json_str(gs_json_get(reply.v, "text"), ""));
        *decode_ms = (int)gs_json_num(gs_json_get(reply.v, "decodeMs"), -1);
        if (*decode_ms > 0) c->decode_ms += *decode_ms;
    } else {
        SDL_strlcpy(error, reply.error, error_size);
    }
    eh_engine_reply_free(&reply);
    return code;
}

// One recording decoded as the app decodes a dictation, with what the measurement scripts want to
// know about the pieces.
static int transcribe_chunked(eh_engine *e, const char *file, const uint8_t *wav, size_t len, bool json) {
    chunked_run run = { e, 0, 0, 0 };
    eh_chunked o = { .run = chunked_decode, .user = &run, .min_sound_sec = -1, .min_utterance_sec = -1 };
    eh_chunked_result r;
    bool ok = eh_transcribe_chunked(wav, len, &o, &r);
    if (!ok) fprintf(stderr, "earheart: %s: %s\n", file, r.error);
    if (json) {
        eh_jv *out = eh_jv_object(), *pieces = eh_jv_array();
        eh_jv_set(out, "file", eh_jv_str(file));
        eh_jv_set(out, "text", eh_jv_str(ok ? r.text : ""));
        eh_jv_set(out, "partial", eh_jv_bool(r.partial));
        if (!ok) eh_jv_set(out, "error", eh_jv_str(r.error));
        eh_jv_set(out, "decodeMs", eh_jv_num(run.decode_ms));
        eh_jv_set(out, "workerInputs", eh_jv_num(run.inputs));
        eh_jv_set(out, "maxPieceSec", eh_jv_num(run.max_seconds));
        for (int i = 0; i < r.piece_count; i++) {
            eh_jv *p = eh_jv_object();
            eh_jv_set(p, "seconds", eh_jv_num((double)(r.pieces[i].to - r.pieces[i].from) / EH_SAMPLE_RATE));
            eh_jv_set(p, "ok", eh_jv_bool(r.pieces[i].ok));
            eh_jv_set(p, "noise", eh_jv_bool(r.pieces[i].noise));
            eh_jv_set(p, "unconfirmed", eh_jv_bool(r.pieces[i].unconfirmed));
            eh_jv_push(pieces, p);
        }
        eh_jv_set(out, "pieces", pieces);
        char *line = eh_jv_write(out, false);
        puts(line);
        SDL_free(line);
        eh_jv_free(out);
    } else if (ok) {
        puts(r.text);
    }
    fflush(stdout);
    eh_chunked_result_free(&r);
    return ok ? 0 : 1;
}

static int cmd_transcribe(int argc, char **argv) {
    const char *models = SDL_getenv("EARHEART_MODELS"), *id = NULL, *list = NULL;
    bool json = false, chunked = false;
    int i = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!SDL_strcmp(argv[i], "--models") && i + 1 < argc) models = argv[++i];
        else if (!SDL_strcmp(argv[i], "--model") && i + 1 < argc) id = argv[++i];
        else if (!SDL_strcmp(argv[i], "--list") && i + 1 < argc) list = argv[++i];
        else if (!SDL_strcmp(argv[i], "--json")) json = true;
        else if (!SDL_strcmp(argv[i], "--chunked")) chunked = true;
        else return usage();
    }
    if (list) {
        // The listed paths take the place of the arguments.
        char *text = SDL_LoadFile(list, NULL), *save = NULL;
        if (!text) { fprintf(stderr, "earheart: %s: %s\n", list, SDL_GetError()); return 1; }
        int cap = 16;
        argv = SDL_malloc(sizeof(char *) * (size_t)cap);
        argc = i = 0;
        for (char *line = SDL_strtok_r(text, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
            if (argc == cap) argv = SDL_realloc(argv, sizeof(char *) * (size_t)(cap *= 2));
            argv[argc++] = line;
        }
    }
    if (i == argc || !models) return usage();
    const eh_model *m = eh_catalog_find(EH_MODEL_STT, id);
    if (!m) { fprintf(stderr, "earheart: no speech model \"%s\"\n", id); return 2; }
    char dir[1024];
    SDL_snprintf(dir, sizeof dir, "%s/stt/%s", models, m->id);

    eh_engine *e = eh_engine_new("stt");
    eh_engine_reply msg;
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
    if (eh_engine_call(e, &w, NULL, 0, 0, NULL, NULL, &msg)) return fail(&msg);
    int threads = (int)gs_json_num(gs_json_get(msg.v, "numThreads"), 0);
    eh_engine_reply_free(&msg);

    int failed = 0;
    for (; i < argc; i++) {
        size_t len;
        void *wav = SDL_LoadFile(argv[i], &len);
        if (!wav) { fprintf(stderr, "earheart: %s: %s\n", argv[i], SDL_GetError()); failed = 1; continue; }
        if (chunked) {
            failed |= transcribe_chunked(e, argv[i], wav, len, json);
            SDL_free(wav);
            continue;
        }
        eh_jw_begin(&w);
        eh_jw_str(&w, "type", "transcribe");
        bool ok = !eh_engine_call(e, &w, wav, len, 0, NULL, NULL, &msg);
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
        eh_engine_reply_free(&msg);
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

static int clean_batch(const char *path, bool cpu) {
    eh_engine *e = eh_engine_new("cleanup");
    char *line = NULL;
    size_t cap = 1 << 20;
    line = SDL_malloc(cap);
    while (fgets(line, (int)cap, stdin)) {
        gs_json *doc = gs_json_parse(line, SDL_strlen(line));
        gs_jv v = gs_json_root(doc);
        if (!doc) continue;
        eh_jw w;
        eh_jw_begin(&w);
        eh_jw_str(&w, "type", "clean");
        eh_jw_str(&w, "modelPath", path);
        eh_jw_bool(&w, "cpuOnly", cpu);
        eh_jw_str(&w, "systemPrompt", gs_json_str(gs_json_get(v, "systemPrompt"), ""));
        eh_jw_str(&w, "transcript", gs_json_str(gs_json_get(v, "transcript"), ""));
        static const char *const numbers[] = { "temperature", "topP", "topK", "minP", "seed" };
        for (size_t k = 0; k < SDL_arraysize(numbers); k++)
            if (gs_json_get(v, numbers[k])) eh_jw_num(&w, numbers[k], gs_json_num(gs_json_get(v, numbers[k]), 0));
        eh_jw_bool(&w, "fresh", gs_json_bool(gs_json_get(v, "fresh"), false));
        eh_engine_reply msg;
        Uint64 t0 = SDL_GetTicks();
        bool ok = !eh_engine_call(e, &w, NULL, 0, 0, NULL, NULL, &msg);
        eh_jw_begin(&w);
        eh_jw_str(&w, ok ? "text" : "error", ok ? gs_json_str(gs_json_get(msg.v, "text"), "") : msg.error);
        eh_jw_num(&w, "ms", (double)(SDL_GetTicks() - t0));
        char *out = eh_jw_end(&w);
        puts(out);
        fflush(stdout);
        SDL_free(out);
        eh_engine_reply_free(&msg);
        gs_json_free(doc);
    }
    eh_engine_free(e);
    return 0;
}

static int cmd_clean(int argc, char **argv) {
    const char *models = SDL_getenv("EARHEART_MODELS"), *id = NULL, *system_file = NULL;
    double temperature = -1;
    bool cpu = false, tokens = false, batch = false;
    int i = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!SDL_strcmp(argv[i], "--models") && i + 1 < argc) models = argv[++i];
        else if (!SDL_strcmp(argv[i], "--model") && i + 1 < argc) id = argv[++i];
        else if (!SDL_strcmp(argv[i], "--system") && i + 1 < argc) system_file = argv[++i];
        else if (!SDL_strcmp(argv[i], "--temperature") && i + 1 < argc) temperature = SDL_atof(argv[++i]);
        else if (!SDL_strcmp(argv[i], "--cpu")) cpu = true;
        else if (!SDL_strcmp(argv[i], "--tokens")) tokens = true;
        else if (!SDL_strcmp(argv[i], "--batch")) batch = true;
        else return usage();
    }
    if (i != argc - (batch ? 0 : 1) || !models) return usage();
    const eh_model *m = eh_catalog_find(EH_MODEL_CLEANUP, id);
    if (!m) { fprintf(stderr, "earheart: no cleanup model \"%s\"\n", id); return 2; }
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/cleanup/%s/%s", models, m->id, m->gguf);
    if (batch) return clean_batch(path, cpu);
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
    eh_engine_reply msg;
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", "clean");
    eh_jw_str(&w, "modelPath", path);
    eh_jw_bool(&w, "cpuOnly", cpu);
    eh_jw_str(&w, "systemPrompt", system_prompt);
    eh_jw_str(&w, "transcript", text);
    if (temperature >= 0) eh_jw_num(&w, "temperature", temperature);
    Uint64 t0 = SDL_GetTicks();
    if (eh_engine_call(e, &w, NULL, 0, 0, NULL, NULL, &msg)) return fail(&msg);
    puts(gs_json_str(gs_json_get(msg.v, "text"), ""));
    fprintf(stderr, "%d ms\n", (int)(SDL_GetTicks() - t0));
    eh_engine_reply_free(&msg);
    eh_engine_free(e);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !SDL_strcmp(argv[1], "--cores")) { printf("%d\n", eh_math_cores()); return 0; }
    if (argc >= 3 && !SDL_strcmp(argv[1], "--engine")) return eh_worker_main(argv[2]);
    if (argc >= 2 && !SDL_strcmp(argv[1], "transcribe")) return cmd_transcribe(argc - 2, argv + 2);
    if (argc >= 2 && !SDL_strcmp(argv[1], "clean")) return cmd_clean(argc - 2, argv + 2);
    if (argc >= 2 && !SDL_strcmp(argv[1], "dictate")) return eh_cmd_dictate(argc - 2, argv + 2);
    if (argc >= 2 && !SDL_strcmp(argv[1], "models")) return cmd_models(argc - 2, argv + 2);
    if (argc >= 2 && !SDL_strcmp(argv[1], "overlay-shot")) return cmd_overlay_shot(argc - 2, argv + 2);
    if (argc >= 2 && !SDL_strcmp(argv[1], "settings-smoke")) return eh_cmd_settings_smoke(argc - 2, argv + 2);
    if (argc >= 2 && !SDL_strcmp(argv[1], "wizard-smoke")) return eh_cmd_wizard_smoke(argc - 2, argv + 2);
    if (argc >= 2 && !SDL_strcmp(argv[1], "correction-smoke")) return eh_cmd_correction_smoke(argc - 2, argv + 2);
    if (argc >= 2 && (!SDL_strcmp(argv[1], "--help") || !SDL_strcmp(argv[1], "-h"))) { usage(); return 0; }
    // Anything else is the app itself: in the tray, or handing its arguments to the instance there.
    return eh_run(argc, argv);
}
