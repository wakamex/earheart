#include "cleanup.h"
#include "llama/llama.h"

#define N_BATCH 512

static struct {
    SDL_SharedObject *ggml_base, *ggml, *lib;
    void (*load_backends)(const char *);
    size_t (*dev_count)(void);
    ggml_backend_dev_t (*dev_get)(size_t);
    const char *(*dev_name)(ggml_backend_dev_t);
    enum ggml_backend_dev_type (*dev_type)(ggml_backend_dev_t);
    void (*backend_init)(void);
    void (*log_set)(ggml_log_callback, void *);
    struct llama_model_params (*model_defaults)(void);
    struct llama_context_params (*context_defaults)(void);
    struct llama_model *(*model_load)(const char *, struct llama_model_params);
    void (*model_free)(struct llama_model *);
    struct llama_context *(*context_new)(struct llama_model *, struct llama_context_params);
    void (*context_free)(struct llama_context *);
    uint32_t (*n_ctx)(const struct llama_context *);
    const struct llama_vocab *(*vocab_of)(const struct llama_model *);
    const char *(*chat_template)(const struct llama_model *, const char *);
    int32_t (*meta_str)(const struct llama_model *, const char *, char *, size_t);
    int32_t (*apply_template)(const char *, const struct llama_chat_message *, size_t, bool, char *, int32_t);
    int32_t (*tokenize)(const struct llama_vocab *, const char *, int32_t, llama_token *, int32_t, bool, bool);
    int32_t (*piece)(const struct llama_vocab *, llama_token, char *, int32_t, int32_t, bool);
    bool (*is_eog)(const struct llama_vocab *, llama_token);
    llama_token (*bos)(const struct llama_vocab *);
    struct llama_batch (*batch_one)(llama_token *, int32_t);
    int32_t (*decode)(struct llama_context *, struct llama_batch);
    llama_memory_t (*memory)(const struct llama_context *);
    bool (*memory_rm)(llama_memory_t, llama_seq_id, llama_pos, llama_pos);
    struct llama_sampler_chain_params (*chain_defaults)(void);
    struct llama_sampler *(*chain_new)(struct llama_sampler_chain_params);
    void (*chain_add)(struct llama_sampler *, struct llama_sampler *);
    struct llama_sampler *(*s_top_k)(int32_t);
    struct llama_sampler *(*s_top_p)(float, size_t);
    struct llama_sampler *(*s_min_p)(float, size_t);
    struct llama_sampler *(*s_temp)(float);
    struct llama_sampler *(*s_dist)(uint32_t);
    struct llama_sampler *(*s_greedy)(void);
    llama_token (*sample)(struct llama_sampler *, struct llama_context *, int32_t);
    void (*sampler_free)(struct llama_sampler *);

    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    char *model_path;
    int context_size;
    enum { WRAP_TEMPLATE, WRAP_GEMMA, WRAP_CHATML, WRAP_QWEN } wrap;
    llama_token *held;  // the tokens now evaluated in the context, in order
    int held_count, held_cap;
    char device[64];
    char error[512];
} L;

const char *eh_cleanup_error(void) { return L.error; }
const char *eh_cleanup_device(void) { return L.device; }

#ifdef _WIN32
#define LIB(name) name ".dll"
#elif defined(__APPLE__)
#define LIB(name) "lib" name ".dylib"
#else
#define LIB(name) "lib" name ".so.0"
#endif

// llama.cpp logs every load in detail; only its errors are worth the log.
static void quiet_log(enum ggml_log_level level, const char *text, void *user) {
    if (level == GGML_LOG_LEVEL_ERROR) SDL_Log("llama.cpp: %s", text);
}

static SDL_SharedObject *open_lib(const char *dir, const char *name) {
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/%s", dir, name);
    SDL_SharedObject *lib = SDL_LoadObject(path);
    if (!lib) SDL_snprintf(L.error, sizeof L.error, "llama.cpp not available: %s", SDL_GetError());
    return lib;
}

static bool open_library(const char *dir) {
    if (L.lib) return true;
    // In dependency order, so each finds the ones before it already loaded.
    if (!(L.ggml_base = open_lib(dir, LIB("ggml-base"))) || !(L.ggml = open_lib(dir, LIB("ggml"))) || !(L.lib = open_lib(dir, LIB("llama")))) return false;
    struct { void *fn; SDL_SharedObject *lib; const char *name; } syms[] = {
        { &L.load_backends, L.ggml, "ggml_backend_load_all_from_path" }, { &L.dev_count, L.ggml, "ggml_backend_dev_count" },
        { &L.dev_get, L.ggml, "ggml_backend_dev_get" }, { &L.dev_name, L.ggml, "ggml_backend_dev_name" },
        { &L.dev_type, L.ggml, "ggml_backend_dev_type" },
        { &L.backend_init, L.lib, "llama_backend_init" }, { &L.log_set, L.lib, "llama_log_set" },
        { &L.model_defaults, L.lib, "llama_model_default_params" }, { &L.context_defaults, L.lib, "llama_context_default_params" },
        { &L.model_load, L.lib, "llama_model_load_from_file" }, { &L.model_free, L.lib, "llama_model_free" },
        { &L.context_new, L.lib, "llama_init_from_model" }, { &L.context_free, L.lib, "llama_free" },
        { &L.n_ctx, L.lib, "llama_n_ctx" }, { &L.vocab_of, L.lib, "llama_model_get_vocab" },
        { &L.chat_template, L.lib, "llama_model_chat_template" }, { &L.meta_str, L.lib, "llama_model_meta_val_str" },
        { &L.apply_template, L.lib, "llama_chat_apply_template" }, { &L.tokenize, L.lib, "llama_tokenize" },
        { &L.piece, L.lib, "llama_token_to_piece" }, { &L.is_eog, L.lib, "llama_vocab_is_eog" }, { &L.bos, L.lib, "llama_vocab_bos" },
        { &L.batch_one, L.lib, "llama_batch_get_one" }, { &L.decode, L.lib, "llama_decode" },
        { &L.memory, L.lib, "llama_get_memory" }, { &L.memory_rm, L.lib, "llama_memory_seq_rm" },
        { &L.chain_defaults, L.lib, "llama_sampler_chain_default_params" }, { &L.chain_new, L.lib, "llama_sampler_chain_init" },
        { &L.chain_add, L.lib, "llama_sampler_chain_add" }, { &L.s_top_k, L.lib, "llama_sampler_init_top_k" },
        { &L.s_top_p, L.lib, "llama_sampler_init_top_p" }, { &L.s_min_p, L.lib, "llama_sampler_init_min_p" },
        { &L.s_temp, L.lib, "llama_sampler_init_temp" }, { &L.s_dist, L.lib, "llama_sampler_init_dist" },
        { &L.s_greedy, L.lib, "llama_sampler_init_greedy" }, { &L.sample, L.lib, "llama_sampler_sample" },
        { &L.sampler_free, L.lib, "llama_sampler_free" },
    };
    for (size_t i = 0; i < SDL_arraysize(syms); i++) {
        SDL_FunctionPointer f = SDL_LoadFunction(syms[i].lib, syms[i].name);
        if (!f) {
            SDL_snprintf(L.error, sizeof L.error, "llama.cpp not available: %s is missing", syms[i].name);
            L.lib = NULL;
            return false;
        }
        *(SDL_FunctionPointer *)syms[i].fn = f;
    }
    L.log_set(quiet_log, NULL);
    L.load_backends(dir);
    L.backend_init();
    return true;
}

void eh_cleanup_unload(void) {
    if (L.ctx) L.context_free(L.ctx);
    if (L.model) L.model_free(L.model);
    SDL_free(L.model_path);
    L.ctx = NULL;
    L.model = NULL;
    L.model_path = NULL;
    L.context_size = 0;
    L.held_count = 0;
}

static bool contains_nocase(const char *hay, const char *needle) { return hay && SDL_strcasestr(hay, needle); }

// Which prompt the model gets. The catalogue's families are written out exactly as the Electron
// build's library renders them (tests/prompts.c holds its output); any other model goes through
// llama.cpp's own reading of the template stored in the file.
static void pick_wrapper(void) {
    const char *tmpl = L.chat_template(L.model, NULL);
    char name[256] = "";
    L.meta_str(L.model, "general.name", name, sizeof name);
    char base[256] = "";
    L.meta_str(L.model, "general.basename", base, sizeof base);
    if (tmpl && SDL_strstr(tmpl, "<start_of_turn>")) L.wrap = WRAP_GEMMA;
    else if (tmpl && SDL_strstr(tmpl, "<|im_start|>")) L.wrap = contains_nocase(name, "qwen") || contains_nocase(base, "qwen") ? WRAP_QWEN : WRAP_CHATML;
    else L.wrap = WRAP_TEMPLATE;
}

static bool try_load(const char *path, int context_size, bool gpu) {
    struct llama_model_params mp = L.model_defaults();
    mp.n_gpu_layers = gpu ? 999 : 0;
    L.model = L.model_load(path, mp);
    if (!L.model) { SDL_snprintf(L.error, sizeof L.error, "The cleanup model %s could not be loaded", path); return false; }
    struct llama_context_params cp = L.context_defaults();
    cp.n_ctx = (uint32_t)context_size;
    cp.n_batch = N_BATCH;
    // Physical cores do the arithmetic; SDL counts logical ones, and half of them is the usual ratio.
    cp.n_threads = cp.n_threads_batch = SDL_max(1, SDL_GetNumLogicalCPUCores() / 2);
    L.ctx = L.context_new(L.model, cp);
    if (!L.ctx) { SDL_snprintf(L.error, sizeof L.error, "No %d-token context could be made for %s", context_size, path); return false; }
    return true;
}

bool eh_cleanup_load(const char *engines_dir, const char *path, int context_size, bool cpu_only) {
    if (context_size <= 0) context_size = EH_CLEAN_CONTEXT_MIN;
    if (L.model && L.context_size == context_size && !SDL_strcmp(L.model_path, path)) return true;
    if (!open_library(engines_dir)) return false;
    eh_cleanup_unload();

    const char *env = SDL_getenv("EARHEART_LLAMA_GPU");
    const char *gpu_name = NULL;
    if (!cpu_only && !(env && !SDL_strcmp(env, "off")))
        for (size_t i = 0; i < L.dev_count() && !gpu_name; i++)
            if (L.dev_type(L.dev_get(i)) != GGML_BACKEND_DEVICE_TYPE_CPU) gpu_name = L.dev_name(L.dev_get(i));
    // A GPU that is present can still lack the free memory for the model and its context, which
    // only shows when they are made; the CPU then takes it, so cleanup is slower and not lost.
    bool ok = gpu_name && try_load(path, context_size, true);
    if (!ok && gpu_name) {
        SDL_Log("cleanup: loading on %s failed (%s); retrying on the CPU", gpu_name, L.error);
        eh_cleanup_unload();
    }
    if (ok) SDL_strlcpy(L.device, gpu_name, sizeof L.device);
    else if ((ok = try_load(path, context_size, false))) SDL_strlcpy(L.device, "CPU", sizeof L.device);
    if (!ok) { eh_cleanup_unload(); return false; }
    L.vocab = L.vocab_of(L.model);
    L.model_path = SDL_strdup(path);
    L.context_size = context_size;
    pick_wrapper();
    return true;
}

char *eh_cleanup_turn_prefix(const char *system_prompt, const char *transcript) {
    char *s = NULL;
    SDL_asprintf(&s, "%s\n\nTranscript:\n%s", system_prompt, transcript);
    return s;
}

char *eh_cleanup_turn(const char *system_prompt, const char *transcript) {
    char *s = NULL;
    SDL_asprintf(&s, "%s\n\nTranscript:\n%s\n\nCleaned transcript:", system_prompt, transcript);
    return s;
}

typedef struct { llama_token *v; int n, cap; } tokens;

static void push_text(tokens *t, const char *text, bool special) {
    int len = (int)SDL_strlen(text);
    if (!len) return;
    for (;;) {
        int room = t->cap - t->n;
        int got = L.tokenize(L.vocab, text, len, t->v + t->n, room, false, special);
        if (got >= 0) { t->n += got; return; }
        t->cap = t->n - got + 256;
        t->v = SDL_realloc(t->v, sizeof(llama_token) * (size_t)t->cap);
    }
}

static void push_token(tokens *t, llama_token tok) {
    if (t->n == t->cap) t->v = SDL_realloc(t->v, sizeof(llama_token) * (size_t)(t->cap = t->cap * 2 + 16));
    t->v[t->n++] = tok;
}

// The prompt around a user turn. `open` leaves the turn unfinished, for priming: the tokens are
// then a prefix of what any turn starting with this text gets, up to where the texts part.
static tokens build_prompt(const char *turn, bool open) {
    tokens t = { 0 };
    const char *head = NULL, *tail = NULL;
    switch (L.wrap) {
    case WRAP_GEMMA: push_token(&t, L.bos(L.vocab)); head = "<start_of_turn>user\n"; tail = "<end_of_turn>\n<start_of_turn>model\n"; break;
    case WRAP_CHATML: head = "<|im_start|>user\n"; tail = "<|im_end|>\n<|im_start|>assistant\n"; break;
    // An empty thought, so a model that can reason spends its tokens on the transcript.
    case WRAP_QWEN: head = "<|im_start|>user\n"; tail = "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"; break;
    default: break;
    }
    if (head) {
        push_text(&t, head, true);
        push_text(&t, turn, false);
        if (!open) push_text(&t, tail, true);
        return t;
    }
    struct llama_chat_message msg = { "user", turn };
    const char *tmpl = L.chat_template(L.model, NULL);
    int cap = (int)SDL_strlen(turn) * 2 + 1024;
    char *buf = SDL_malloc((size_t)cap);
    int n = L.apply_template(tmpl, &msg, 1, !open, buf, cap);
    if (n > cap) { buf = SDL_realloc(buf, (size_t)n + 1); n = L.apply_template(tmpl, &msg, 1, !open, buf, n + 1); }
    if (n < 0) { n = (int)SDL_strlcpy(buf, turn, (size_t)cap); }  // a template llama.cpp cannot read: the bare turn
    buf[n] = 0;
    // An open turn stops where the user's text ends; what the template puts after it is dropped.
    if (open) { char *at = SDL_strstr(buf, turn); if (at) at[SDL_strlen(turn)] = 0; }
    int room = n + 16;
    t.v = SDL_malloc(sizeof(llama_token) * (size_t)room);
    t.cap = room;
    int got = L.tokenize(L.vocab, buf, (int)SDL_strlen(buf), t.v, room, true, true);
    t.n = got > 0 ? got : 0;
    SDL_free(buf);
    return t;
}

int *eh_cleanup_prompt_tokens(const char *turn, int *count) {
    if (!L.model) { *count = 0; return NULL; }
    tokens t = build_prompt(turn, false);
    *count = t.n;
    return t.v;
}

int eh_cleanup_count_tokens(const char *text) {
    if (!L.model) return -1;
    tokens t = { 0 };
    push_text(&t, text, false);
    SDL_free(t.v);
    return t.n;
}

// Brings the context to hold exactly `want`: keeps the prefix it shares with what is there,
// drops the rest and evaluates what is new.
static bool evaluate(const tokens *want, SDL_AtomicInt *cancel) {
    int same = 0;
    while (same < L.held_count && same < want->n && L.held[same] == want->v[same]) same++;
    // The last token is always evaluated again, so its logits are there to sample from.
    if (same == want->n && same > 0) same--;
    if (same < L.held_count) L.memory_rm(L.memory(L.ctx), 0, same, -1);
    L.held_count = same;
    if (want->n > L.held_cap) L.held = SDL_realloc(L.held, sizeof(llama_token) * (size_t)(L.held_cap = want->n + 1024));
    for (int at = same; at < want->n; at += N_BATCH) {
        if (cancel && SDL_GetAtomicInt(cancel)) { SDL_strlcpy(L.error, "cleanup cancelled", sizeof L.error); return false; }
        int n = SDL_min(N_BATCH, want->n - at);
        if (L.decode(L.ctx, L.batch_one(want->v + at, n))) { SDL_strlcpy(L.error, "The cleanup model failed to read the prompt", sizeof L.error); return false; }
        SDL_memcpy(L.held + at, want->v + at, sizeof(llama_token) * (size_t)n);
        L.held_count = at + n;
    }
    return true;
}

bool eh_cleanup_prime(const char *text, SDL_AtomicInt *cancel) {
    if (!L.ctx) { SDL_strlcpy(L.error, "Cleanup model not loaded", sizeof L.error); return false; }
    tokens t = build_prompt(text, true);
    // The last token of an unfinished text can change once more text follows it, so it is left out.
    if (t.n) t.n--;
    bool ok = t.n < (int)L.n_ctx(L.ctx) && evaluate(&t, cancel);
    SDL_free(t.v);
    return ok;
}

static struct llama_sampler *make_sampler(const eh_sampling *s) {
    float temperature = s && s->temperature >= 0 ? s->temperature : 0.2f;
    struct llama_sampler *chain = L.chain_new(L.chain_defaults());
    if (temperature <= 0) { L.chain_add(chain, L.s_greedy()); return chain; }
    L.chain_add(chain, L.s_top_k(s && s->top_k > 0 ? s->top_k : 40));
    L.chain_add(chain, L.s_top_p(s && s->top_p >= 0 ? s->top_p : 0.95f, 1));
    if (s && s->min_p > 0) L.chain_add(chain, L.s_min_p(s->min_p, 1));
    L.chain_add(chain, L.s_temp(temperature));
    L.chain_add(chain, L.s_dist((uint32_t)SDL_GetTicksNS()));
    return chain;
}

static char *trimmed(const char *s, size_t n) {
    while (n && SDL_isspace((unsigned char)*s)) s++, n--;
    while (n && SDL_isspace((unsigned char)s[n - 1])) n--;
    return SDL_strndup(s, n);
}

char *eh_cleanup_clean(const char *system_prompt, const char *transcript, const eh_sampling *sampling,
                       eh_cleanup_progress *progress, void *user, SDL_AtomicInt *cancel) {
    if (!L.ctx) { SDL_strlcpy(L.error, "Cleanup model not loaded", sizeof L.error); return NULL; }
    char *turn = eh_cleanup_turn(system_prompt, transcript);
    tokens prompt = build_prompt(turn, false);
    int transcript_tokens = eh_cleanup_count_tokens(transcript);
    int turn_tokens = eh_cleanup_count_tokens(turn);
    SDL_free(turn);

    // A turn that cannot fit is refused: letting the context shift would silently drop the start
    // of the transcript, and a cleaned text that loses the user's words is worse than none.
    int needed = turn_tokens + transcript_tokens + EH_CLEAN_SLACK_TOKENS, available = (int)L.n_ctx(L.ctx);
    if (needed > available || prompt.n >= available) {
        SDL_snprintf(L.error, sizeof L.error, "This dictation is too long to clean up in one pass (it needs %d tokens of context and the model has %d)", needed, available);
        SDL_free(prompt.v);
        return NULL;
    }
    if (!evaluate(&prompt, cancel)) { SDL_free(prompt.v); return NULL; }
    SDL_free(prompt.v);

    int max_tokens = SDL_min(transcript_tokens + EH_CLEAN_SLACK_TOKENS, available - L.held_count);
    size_t total = SDL_max((size_t)1, SDL_strlen(transcript)), len = 0, cap = total + 256;
    char *out = SDL_malloc(cap);
    struct llama_sampler *smpl = make_sampler(sampling);
    const char *stops[] = { "<|im_end|>", "<end_of_turn>" };
    bool done = false, failed = false;
    for (int i = 0; !done && !failed; i++) {
        if (cancel && SDL_GetAtomicInt(cancel)) { SDL_strlcpy(L.error, "cleanup cancelled", sizeof L.error); failed = true; break; }
        llama_token tok = L.sample(smpl, L.ctx, -1);
        if (L.is_eog(L.vocab, tok)) break;
        // Reaching the limit means the model never finished: it reasoned, looped or wrote past the
        // transcript. Half a cleanup is not worth the user's words, so the caller keeps the raw text.
        if (i >= max_tokens) { SDL_strlcpy(L.error, "The cleanup model did not finish within the length of the transcript", sizeof L.error); failed = true; break; }
        char piece[256];
        int n = L.piece(L.vocab, tok, piece, sizeof piece, 0, false);
        if (n > 0) {
            if (len + (size_t)n + 1 > cap) out = SDL_realloc(out, cap = (cap + (size_t)n) * 2);
            SDL_memcpy(out + len, piece, (size_t)n);
            len += (size_t)n;
            out[len] = 0;
            for (size_t s = 0; s < SDL_arraysize(stops); s++) {
                char *at = SDL_strstr(out, stops[s]);
                if (at) { len = (size_t)(at - out); done = true; }
            }
            if (progress) progress(user, SDL_min(0.99f, (float)len / (float)total));
        }
        if (done) break;
        if (L.decode(L.ctx, L.batch_one(&tok, 1))) { SDL_strlcpy(L.error, "The cleanup model failed while writing", sizeof L.error); failed = true; break; }
        if (L.held_count == L.held_cap) L.held = SDL_realloc(L.held, sizeof(llama_token) * (size_t)(L.held_cap += 1024));
        L.held[L.held_count++] = tok;
    }
    L.sampler_free(smpl);
    char *text = failed ? NULL : trimmed(out, len);
    SDL_free(out);
    return text;
}
