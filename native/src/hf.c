// Models from Hugging Face: a pasted address read as a repository, the repository's files sorted
// into the versions that can be downloaded (quantisations of a language model, precisions of a
// speech model), and the model definition made from the one chosen. Ported from
// main/services/hf-models.js, whose rules, order and messages these are.
#include "hf.h"
#include <SDL3/SDL.h>
#include <string.h>
#include "gs_http.h"

typedef struct {
    const char *path, *name;  // name: the part of the path after its last slash
    double bytes;
    char sha256[65];
} entry;

static void fail(char *error, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(error, size, fmt, ap);
    va_end(ap);
}

static bool word(char c) { return SDL_isalnum((unsigned char)c) || c == '_'; }
static char lower(char c) { return (char)SDL_tolower((unsigned char)c); }

static bool ends_ci(const char *s, const char *suffix) {
    size_t n = SDL_strlen(s), m = SDL_strlen(suffix);
    return n >= m && !SDL_strcasecmp(s + n - m, suffix);
}

// The first place `needle` is in `hay`, whatever the case of either.
static const char *find_ci(const char *hay, const char *needle) {
    size_t m = SDL_strlen(needle);
    for (; *hay; hay++)
        if (!SDL_strncasecmp(hay, needle, m)) return hay;
    return NULL;
}

/* ---------------- the address ---------------- */

const char *eh_hf_search_url(eh_model_kind kind) {
    return kind == EH_MODEL_CLEANUP ? "https://huggingface.co/models?library=gguf&other=conversational&num_parameters=min:0,max:12B&sort=trending"
                                    : "https://huggingface.co/models?search=sherpa-onnx-nemo-parakeet-tdt&sort=trending";
}

static bool name_chars(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!word(s[i]) && s[i] != '.' && s[i] != '-') return false;
    return n > 0;
}

static int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// decodeURIComponent: false for a broken escape or bytes that are not UTF-8.
static bool percent_decode(const char *from, size_t n, char *out, size_t size) {
    size_t at = 0;
    for (size_t i = 0; i < n; i++) {
        char c = from[i];
        if (c == '%') {
            if (i + 2 >= n) return false;
            int hi = hex(from[i + 1]), lo = hex(from[i + 2]);
            if (hi < 0 || lo < 0) return false;
            c = (char)(hi * 16 + lo);
            i += 2;
        }
        if (at + 1 >= size) return false;
        out[at++] = c;
    }
    out[at] = 0;
    // Well-formed UTF-8, as the JavaScript function demands of what it decodes.
    for (const unsigned char *p = (const unsigned char *)out; *p;) {
        int more = *p < 0x80 ? 0 : (*p & 0xe0) == 0xc0 ? 1 : (*p & 0xf0) == 0xe0 ? 2 : (*p & 0xf8) == 0xf0 ? 3 : -1;
        if (more < 0) return false;
        for (int k = 1; k <= more; k++)
            if ((p[k] & 0xc0) != 0x80) return false;
        p += more + 1;
    }
    return true;
}

bool eh_hf_parse(const char *input, eh_hf_repo *out, char *error, size_t error_size) {
    *out = (eh_hf_repo){ 0 };
    const char *raw = input ? input : "";
    while (SDL_isspace((unsigned char)*raw)) raw++;
    size_t len = SDL_strlen(raw);
    while (len && SDL_isspace((unsigned char)raw[len - 1])) len--;
    if (!len) { fail(error, error_size, "Paste a Hugging Face URL or owner/model"); return false; }
    char typed[512];
    SDL_snprintf(typed, sizeof typed, "%.*s", (int)len, raw);
    // owner/model, bare.
    const char *slash = SDL_strchr(typed, '/');
    if (slash && !SDL_strchr(slash + 1, '/') && name_chars(typed, (size_t)(slash - typed)) && name_chars(slash + 1, SDL_strlen(slash + 1))) {
        SDL_snprintf(out->owner, sizeof out->owner, "%.*s", (int)(slash - typed), typed);
        SDL_strlcpy(out->repo, slash + 1, sizeof out->repo);
        return true;
    }
    // An address: its scheme, host and path, read the way a browser reads them.
    char url[600];
    const char *colon = SDL_strstr(typed, "://");
    bool has_scheme = colon && colon > typed && SDL_isalpha((unsigned char)typed[0]);
    for (const char *p = typed; has_scheme && p < colon; p++) has_scheme = SDL_isalnum((unsigned char)*p) || *p == '+' || *p == '.' || *p == '-';
    SDL_snprintf(url, sizeof url, "%s%s", has_scheme ? "" : "https://", typed);
    char *rest = SDL_strstr(url, "://");
    char scheme[32];
    SDL_snprintf(scheme, sizeof scheme, "%.*s", (int)(rest - url), url);
    for (char *p = scheme; *p; p++) *p = lower(*p);
    rest += 3;
    while (*rest == '/' || *rest == '\\') rest++;
    size_t host_len = strcspn(rest, "/\\?#");
    char host[256];
    SDL_snprintf(host, sizeof host, "%.*s", (int)host_len, rest);
    char *at = SDL_strrchr(host, '@');
    char *name = at ? at + 1 : host;
    char *port = SDL_strchr(name, ':');
    if (port) *port = 0;
    bool bad_host = !*name;
    for (char *p = name; *p; p++) {
        *p = lower(*p);
        bad_host = bad_host || SDL_strchr(" #%/:<>?@[\\]^|", *p) != NULL || (unsigned char)*p < 0x20;
    }
    if (bad_host) { fail(error, error_size, "Not a Hugging Face URL or owner/model: %s", typed); return false; }
    if (SDL_strcmp(scheme, "https") && SDL_strcmp(scheme, "http")) { fail(error, error_size, "URL must start with https://"); return false; }
    if (!SDL_strncmp(name, "www.", 4)) name += 4;
    if (SDL_strcmp(name, "huggingface.co") && SDL_strcmp(name, "hf.co")) { fail(error, error_size, "Only huggingface.co URLs (or a bare owner/model) are supported"); return false; }
    // The path's parts, without the empty ones.
    const char *path = rest + host_len;
    size_t path_len = strcspn(path, "?#");
    const char *parts[8];
    size_t part_len[8];
    int n = 0;
    for (size_t i = 0; i < path_len && n < 8;) {
        while (i < path_len && path[i] == '/') i++;
        size_t start = i;
        while (i < path_len && path[i] != '/') i++;
        if (i > start) parts[n] = path + start, part_len[n++] = i - start;
    }
    int first = n && part_len[0] == 6 && !SDL_strncmp(parts[0], "models", 6) ? 1 : 0;
    if (n - first < 2) { fail(error, error_size, "URL must point to a model repo, e.g. huggingface.co/owner/model"); return false; }
    SDL_snprintf(out->owner, sizeof out->owner, "%.*s", (int)part_len[first], parts[first]);
    SDL_snprintf(out->repo, sizeof out->repo, "%.*s", (int)part_len[first + 1], parts[first + 1]);
    if (n - first >= 4) {
        const char *verb = parts[first + 2];
        size_t vl = part_len[first + 2];
        bool reference = (vl == 4 && (!SDL_strncmp(verb, "tree", 4) || !SDL_strncmp(verb, "blob", 4))) || (vl == 7 && !SDL_strncmp(verb, "resolve", 7));
        if (reference && !percent_decode(parts[first + 3], part_len[first + 3], out->ref, sizeof out->ref)) {
            fail(error, error_size, "Not a Hugging Face URL or owner/model: %s", typed);
            return false;
        }
    }
    return true;
}

// encodeURIComponent, appended to `out`.
static void encode(char *out, size_t size, const char *s, size_t n) {
    size_t at = SDL_strlen(out);
    for (size_t i = 0; i < n && at + 4 < size; i++) {
        unsigned char c = (unsigned char)s[i];
        if (SDL_isalnum(c) || SDL_strchr("-_.!~*'()", c)) out[at++] = (char)c;
        else at += (size_t)SDL_snprintf(out + at, size - at, "%%%02X", c);
    }
    out[at] = 0;
}

static char *resolve_url(const char *owner, const char *repo, const char *commit, const char *path) {
    char url[2048];
    SDL_snprintf(url, sizeof url, "https://huggingface.co/%s/%s/resolve/", owner, repo);
    encode(url, sizeof url, commit, SDL_strlen(commit));
    for (const char *p = path;;) {
        const char *slash = SDL_strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : SDL_strlen(p);
        SDL_strlcat(url, "/", sizeof url);
        encode(url, sizeof url, p, n);
        if (!slash) break;
        p = slash + 1;
    }
    return SDL_strdup(url);
}

/* ---------------- a repository's files ---------------- */

static bool status_ok(int status, char *error, size_t error_size) {
    if (status == 401 || status == 403) fail(error, error_size, "This repository is gated or private — Earheart can only download public models");
    else if (status == 404) fail(error, error_size, "Model repository not found on Hugging Face");
    else if (status < 200 || status > 299) fail(error, error_size, "Hugging Face returned HTTP %d", status);
    else return true;
    return false;
}

static bool truthy(const gs_jval *v) {
    if (!v) return false;
    switch (gs_jval_kind(v)) {
    case GS_JSON_NULL: return false;
    case GS_JSON_BOOL: return gs_jval_boolean(v, false);
    case GS_JSON_NUMBER: return gs_jval_number(v, 0) != 0;
    case GS_JSON_STRING: return gs_jval_string(v, "")[0] != 0;
    default: return true;
    }
}

static int read_files(const gs_jval *tree, entry **out) {
    int n = 0, count = tree && gs_jval_kind(tree) == GS_JSON_ARRAY ? gs_jval_count(tree) : 0;
    entry *list = SDL_calloc((size_t)count + 1, sizeof *list);
    for (int i = 0; i < count; i++) {
        const gs_jval *e = gs_jval_at(tree, i);
        if (!e || gs_jval_kind(e) != GS_JSON_OBJECT || SDL_strcmp(gs_jval_string(gs_jval_get(e, "type"), ""), "file")) continue;
        entry *f = &list[n++];
        f->path = gs_jval_string(gs_jval_get(e, "path"), "");
        const char *slash = SDL_strrchr(f->path, '/');
        f->name = slash ? slash + 1 : f->path;
        const gs_jval *lfs = gs_jval_get(e, "lfs");
        f->bytes = gs_jval_number(gs_jval_get(e, "size"), 0);
        if (f->bytes == 0) f->bytes = gs_jval_number(gs_jval_get(lfs, "size"), 0);
        const char *oid = gs_jval_string(gs_jval_get(lfs, "oid"), "");
        bool is_hex = SDL_strlen(oid) == 64;
        for (int k = 0; is_hex && k < 64; k++) is_hex = hex(oid[k]) >= 0;
        for (int k = 0; is_hex && k < 64; k++) f->sha256[k] = lower(oid[k]);
    }
    *out = list;
    return n;
}

static gs_jval *file_json(const eh_hf_repo *r, const char *commit, const entry *f) {
    gs_jval *o = gs_jval_object();
    gs_jval_set(o, "name", gs_jval_str(f->name));
    if (f->bytes != 0) gs_jval_set(o, "bytes", gs_jval_num(f->bytes));
    if (f->sha256[0]) gs_jval_set(o, "sha256", gs_jval_str(f->sha256));
    char *url = resolve_url(r->owner, r->repo, commit, f->path);
    gs_jval_set(o, "url", gs_jval_str(url));
    SDL_free(url);
    return o;
}

typedef struct { gs_jval *json; int priority; double bytes; } ranked;

// The versions in order: by the rank of their label, then smallest first, otherwise as found.
static gs_jval *ranked_result(const eh_hf_repo *r, const char *commit, ranked *v, int n) {
    for (int i = 1; i < n; i++) {
        ranked moving = v[i];
        int j = i;
        while (j > 0 && (v[j - 1].priority > moving.priority || (v[j - 1].priority == moving.priority && v[j - 1].bytes > moving.bytes))) v[j] = v[j - 1], j--;
        v[j] = moving;
    }
    gs_jval *out = gs_jval_object(), *list = gs_jval_array();
    char full[300];
    SDL_snprintf(full, sizeof full, "%s/%s", r->owner, r->repo);
    gs_jval_set(out, "repo", gs_jval_str(full));
    gs_jval_set(out, "commit", gs_jval_str(commit));
    gs_jval_set(out, "recommended", n ? gs_jval_str(gs_jval_string(gs_jval_get(v[0].json, "label"), "")) : gs_jval_null());
    for (int i = 0; i < n; i++) gs_jval_push(list, v[i].json);
    gs_jval_set(out, "variants", list);
    return out;
}

/* ---------------- language models ---------------- */

// The quantisation in a file's name ("Q4_K_M", "IQ3_M", "F16"), in capitals; without one, the
// name less its ".gguf" and any "-00001-of-00003" before it.
static void quant_of(const char *name, char *out, size_t size) {
    size_t len = SDL_strlen(name);
    for (size_t i = 0; i < len; i++) {
        if (i && word(name[i - 1])) continue;  // only at the start of a word
        const char *p = name + i;
        size_t n = 0;
        char c0 = (char)SDL_toupper((unsigned char)p[0]), c1 = (char)SDL_toupper((unsigned char)p[1]);
        if (c0 == 'I' && c1 == 'Q' && SDL_isdigit((unsigned char)p[2])) {
            n = 2;
            while (SDL_isdigit((unsigned char)p[n])) n++;
            while (word(p[n])) n++;
        } else if (c0 == 'Q' && SDL_isdigit((unsigned char)p[1])) {
            n = 1;
            while (SDL_isdigit((unsigned char)p[n])) n++;
            while (p[n] == '_' && SDL_isalnum((unsigned char)p[n + 1])) {
                n++;
                while (SDL_isalnum((unsigned char)p[n])) n++;
            }
        } else if (!SDL_strncasecmp(p, "BF16", 4)) n = 4;
        else if (!SDL_strncasecmp(p, "FP16", 4) || !SDL_strncasecmp(p, "FP32", 4)) n = 4;
        else if (!SDL_strncasecmp(p, "F16", 3) || !SDL_strncasecmp(p, "F32", 3)) n = 3;
        else if (!SDL_strncasecmp(p, "MXFP4", 5)) n = 5;
        if (!n || word(p[n])) continue;  // and only when a word ends there
        SDL_snprintf(out, size, "%.*s", (int)n, p);
        for (char *c = out; *c; c++) *c = (char)SDL_toupper((unsigned char)*c);
        return;
    }
    SDL_strlcpy(out, name, size);
    size_t n = SDL_strlen(out);
    // "-ddddd-of-ddddd.gguf" at the end, then ".gguf".
    if (n >= 20 && ends_ci(out, ".gguf")) {
        const char *s = out + n - 20;
        bool shard = s[0] == '-' && s[6] == '-' && lower(s[7]) == 'o' && lower(s[8]) == 'f' && s[9] == '-';
        for (int k = 1; shard && k <= 5; k++) shard = SDL_isdigit((unsigned char)s[k]) && SDL_isdigit((unsigned char)s[9 + k]);
        if (shard) out[n - 20] = 0, n -= 20;
    }
    if (ends_ci(out, ".gguf")) out[SDL_strlen(out) - 5] = 0;
}

static int quant_priority(const char *label) {
    static const char *const rank[] = { "Q4_K_M", "Q4_K_S", "Q4_0", "Q4_1", "Q4_K", "Q5_K_M", "Q5_K_S", "Q5_0", "Q3_K_M", "Q6_K", "Q8_0" };
    for (int i = 0; i < (int)SDL_arraysize(rank); i++)
        if (!SDL_strcasecmp(label, rank[i])) return i;
    return !SDL_strncasecmp(label, "Q4", 2) ? (int)SDL_arraysize(rank) : (int)SDL_arraysize(rank) + 1;
}

// A file that goes with a language model and is not one: a vision projector, an importance matrix.
static bool companion(const char *name) {
    static const char *const marks[] = { "mmproj", "projector", "imatrix" };
    for (int m = 0; m < 3; m++) {
        size_t n = SDL_strlen(marks[m]);
        for (const char *p = name; (p = find_ci(p, marks[m])); p++) {
            bool before = p == name || SDL_strchr("-_.", p[-1]), after = !p[n] || SDL_strchr("-_.", p[n]);
            if (before && after) return true;
        }
    }
    return false;
}

static int by_name(const void *a, const void *b) {
    const entry *x = *(const entry *const *)a, *y = *(const entry *const *)b;
    int c = SDL_strcasecmp(x->name, y->name);
    return c ? c : SDL_strcmp(x->name, y->name);
}

static gs_jval *list_ggufs(const eh_hf_repo *r, const char *commit, const entry *files, int count, char *error, size_t error_size) {
    char (*labels)[160] = SDL_calloc((size_t)count + 1, sizeof *labels);
    const entry **group = SDL_calloc((size_t)count + 1, sizeof *group);
    bool *model = SDL_calloc((size_t)count + 1, 1), *done = SDL_calloc((size_t)count + 1, 1);
    ranked *variants = SDL_calloc((size_t)count + 1, sizeof *variants);
    int found = 0, n = 0;
    for (int i = 0; i < count; i++) {
        model[i] = ends_ci(files[i].path, ".gguf") && !companion(files[i].name);
        if (model[i]) quant_of(files[i].name, labels[i], sizeof labels[i]), found++;
    }
    for (int i = 0; i < count; i++) {
        if (!model[i] || done[i]) continue;
        int members = 0;
        double total = 0;
        for (int j = i; j < count; j++)
            if (model[j] && !done[j] && !SDL_strcmp(labels[j], labels[i])) group[members++] = &files[j], done[j] = true, total += files[j].bytes;
        SDL_qsort(group, (size_t)members, sizeof *group, by_name);
        gs_jval *v = gs_jval_object(), *list = gs_jval_array();
        gs_jval_set(v, "label", gs_jval_str(labels[i]));
        gs_jval_set(v, "totalBytes", gs_jval_num(total));
        for (int k = 0; k < members; k++) gs_jval_push(list, file_json(r, commit, group[k]));
        gs_jval_set(v, "files", list);
        variants[n++] = (ranked){ v, quant_priority(labels[i]), total };
    }
    gs_jval *out = found ? ranked_result(r, commit, variants, n) : NULL;
    if (!found) fail(error, error_size, "No language-model GGUF files found in this repository");
    SDL_free(labels);
    SDL_free(group);
    SDL_free(model);
    SDL_free(done);
    SDL_free(variants);
    return out;
}

/* ---------------- speech models ---------------- */

enum { ENCODER, DECODER, JOINER, NO_PART };
static const char *const PART[3] = { "encoder", "decoder", "joiner" };

// Where in a name its role is written, and which role: the first of the three words found.
static const char *part_at(const char *name, int *which) {
    const char *best = NULL;
    for (int p = 0; p < 3; p++) {
        const char *at = find_ci(name, PART[p]);
        if (at && (!best || at < best)) best = at, *which = p;
    }
    return best;
}

static int part_of(const char *name) {
    int which = NO_PART;
    return ends_ci(name, ".onnx") && part_at(name, &which) ? which : NO_PART;
}

// The precision written in a path ("int8", "fp16"): the last of its dot, dash, underscore or
// slash separated pieces that names one, and full precision when none does.
static void precision_of(const char *path, char *out, size_t size) {
    static const char *const tokens[] = { "int8", "uint8", "uint8f16", "q4", "q4f16", "q8", "q8f16", "bnb4", "quantized", "fp16", "half", "fp32" };
    char stem[1024];
    SDL_strlcpy(stem, path, sizeof stem);
    if (ends_ci(stem, ".onnx")) stem[SDL_strlen(stem) - 5] = 0;
    for (char *p = stem; *p; p++) *p = lower(*p);
    SDL_strlcpy(out, "fp32", size);
    for (char *end = stem + SDL_strlen(stem); end > stem;) {
        char *start = end;
        while (start > stem && !SDL_strchr("/.-_", start[-1])) start--;
        for (int t = 0; t < (int)SDL_arraysize(tokens); t++) {
            if ((size_t)(end - start) == SDL_strlen(tokens[t]) && !SDL_strncmp(start, tokens[t], (size_t)(end - start))) {
                SDL_strlcpy(out, !SDL_strcmp(tokens[t], "half") ? "fp16" : tokens[t], size);
                return;
            }
        }
        end = start;
        while (end > stem && SDL_strchr("/.-_", end[-1])) end--;
    }
}

static int precision_priority(const char *label) {
    static const char *const rank[] = { "int8", "quantized", "q8", "uint8", "fp16", "fp32" };
    for (int i = 0; i < (int)SDL_arraysize(rank); i++)
        if (!SDL_strcmp(label, rank[i])) return i;
    return (int)SDL_arraysize(rank);
}

// What comes before the role in a bundle's file name ("turbo" in "turbo-encoder.onnx"), lowercase.
static void bundle_prefix(const char *name, char *out, size_t size) {
    int which;
    const char *at = part_at(name, &which);
    out[0] = 0;
    if (!at) return;
    size_t n = (size_t)(at - name);
    if (n && SDL_strchr("-_.", name[n - 1])) n--;
    SDL_snprintf(out, size, "%.*s", (int)n, name);
    for (char *p = out; *p; p++) *p = lower(*p);
}

static bool is_tokens(const char *name) {
    size_t n = SDL_strlen(name);
    return ends_ci(name, "tokens.txt") && (n == 10 || SDL_strchr("-_.", name[n - 11]));
}

static void tokens_prefix(const char *name, char *out, size_t size) {
    size_t n = SDL_strlen(name) - 10;
    if (n && SDL_strchr("-_.", name[n - 1])) n--;
    SDL_snprintf(out, size, "%.*s", (int)n, name);
    for (char *p = out; *p; p++) *p = lower(*p);
}

// `mark` as a word of its own in `text`: nothing alphanumeric on either side.
static bool marked(const char *text, const char *mark) {
    size_t n = SDL_strlen(mark);
    for (const char *p = text; (p = find_ci(p, mark)); p++)
        if ((p == text || !SDL_isalnum((unsigned char)p[-1])) && !SDL_isalnum((unsigned char)p[n])) return true;
    return false;
}

// A Whisper size as sherpa-onnx's exports are prefixed: "tiny", "small.en", "distil-large-v3".
static bool whisper_prefix(const char *prefix) {
    const char *p = prefix;
    if (!SDL_strncmp(p, "distil-", 7)) p += 7;
    static const char *const sizes[] = { "tiny", "base", "small", "medium", "large", "turbo" };
    size_t n = 0;
    for (int i = 0; i < 6 && !n; i++)
        if (!SDL_strncmp(p, sizes[i], SDL_strlen(sizes[i]))) n = SDL_strlen(sizes[i]);
    if (!n) return false;
    bool large = !SDL_strncmp(p, "large", 5);
    p += n;
    if (large && p[0] == '-' && p[1] == 'v' && SDL_isdigit((unsigned char)p[2])) p += 3;
    if (!SDL_strcmp(p, ".en")) p += 3;
    return !*p;
}

static void sample_paths(const entry **files, int n, char *out, size_t size) {
    out[0] = 0;
    for (int i = 0; i < n && i < 3; i++) SDL_snprintf(out + SDL_strlen(out), size - SDL_strlen(out), "%s%s", i ? ", " : "", files[i]->path);
    if (n > 3) SDL_strlcat(out, ", …", size);
}

static int depth(const char *path) {
    int d = 1;
    for (; *path; path++) d += *path == '/';
    return d;
}

static bool same_dir(const char *a, const char *b) {
    const char *sa = SDL_strrchr(a, '/'), *sb = SDL_strrchr(b, '/');
    size_t na = sa ? (size_t)(sa - a) + 1 : 0, nb = sb ? (size_t)(sb - b) + 1 : 0;
    return na == nb && !SDL_strncmp(a, b, na);
}

#define SUPPORTED_STT "Earheart supports sherpa-onnx transducer bundles (encoder, decoder, joiner + tokens.txt), such as Parakeet, and sherpa-onnx Whisper exports (encoder, decoder + tokens.txt)"

static gs_jval *list_stt(const eh_hf_repo *r, const char *commit, const entry *files, int count, char *error, size_t error_size) {
    // Shallowest first, and otherwise as found: a bundle at the top of a repository comes before
    // the copies kept in its folders.
    const entry **by_depth = SDL_calloc((size_t)count + 1, sizeof *by_depth), **onnx = SDL_calloc((size_t)count + 1, sizeof *onnx);
    const entry **tokens = SDL_calloc((size_t)count + 1, sizeof *tokens), **all = SDL_calloc((size_t)count + 1, sizeof *all);
    ranked *variants = SDL_calloc((size_t)count + 1, sizeof *variants);
    char (*precisions)[16] = SDL_calloc((size_t)count + 1, sizeof *precisions);
    gs_jval *out = NULL;
    for (int i = 0; i < count; i++) {
        int j = i;
        while (j > 0 && depth(by_depth[j - 1]->path) > depth(files[i].path)) by_depth[j] = by_depth[j - 1], j--;
        by_depth[j] = &files[i];
    }
    int onnx_all = 0, n_onnx = 0, n_tokens = 0, n_all = 0;
    bool has[3] = { false, false, false };
    for (int i = 0; i < count; i++) {
        const entry *f = by_depth[i];
        if (ends_ci(f->name, ".onnx")) all[n_all++] = f, onnx_all++;
        int part = part_of(f->name);
        if (part != NO_PART) onnx[n_onnx++] = f, has[part] = true;
        if (is_tokens(f->name)) tokens[n_tokens++] = f;
    }
    char sample[900], full[300];
    SDL_snprintf(full, sizeof full, "%s/%s", r->owner, r->repo);
    if (!onnx_all) {
        fail(error, error_size, "No .onnx files found in this repository — Earheart needs a sherpa-onnx transducer bundle (encoder, decoder, joiner) or Whisper export (encoder, decoder), plus tokens.txt");
        goto done;
    }
    if (!n_onnx) {
        sample_paths(all, onnx_all, sample, sizeof sample);
        fail(error, error_size, "Found %d .onnx file%s (%s), but none of them is an encoder, decoder or joiner — Earheart can only run sherpa-onnx transducer bundles and Whisper exports for speech-to-text, not single-file ONNX models",
             onnx_all, onnx_all == 1 ? "" : "s", sample);
        goto done;
    }
    sample_paths(onnx, n_onnx, sample, sizeof sample);
    if (!has[ENCODER] || !has[DECODER]) {
        fail(error, error_size, "Found %d .onnx file%s (%s), but no %s — Earheart can only run sherpa-onnx transducer bundles (encoder, decoder, joiner) and Whisper exports (encoder, decoder), plus tokens.txt",
             n_onnx, n_onnx == 1 ? "" : "s", sample, !has[ENCODER] && !has[DECODER] ? "encoder or decoder" : !has[ENCODER] ? "encoder" : "decoder");
        goto done;
    }
    // Everything that could say what family this is: the repository's name and its files' paths.
    size_t text_size = SDL_strlen(full) + 2;
    for (int i = 0; i < n_onnx; i++) text_size += SDL_strlen(onnx[i]->path) + 1;
    for (int i = 0; i < n_tokens; i++) text_size += SDL_strlen(tokens[i]->path) + 1;
    char *marker_text = SDL_calloc(1, text_size);
    SDL_strlcpy(marker_text, full, text_size);
    for (int i = 0; i < n_onnx; i++) SDL_strlcat(marker_text, " ", text_size), SDL_strlcat(marker_text, onnx[i]->path, text_size);
    for (int i = 0; i < n_tokens; i++) SDL_strlcat(marker_text, " ", text_size), SDL_strlcat(marker_text, tokens[i]->path, text_size);
    bool canary = !has[JOINER] && marked(marker_text, "canary"), whisper_named = marked(marker_text, "whisper");
    SDL_free(marker_text);
    if (canary) {
        fail(error, error_size, "This looks like a NeMo Canary model, which Earheart can't run yet. " SUPPORTED_STT ".");
        goto done;
    }
    if (!n_tokens) {
        fail(error, error_size, "Found the %s .onnx files, but no tokens.txt — sherpa-onnx needs the bundle's symbol table. Hugging Face / optimum ONNX exports keep the vocabulary in tokenizer.json instead; look for a sherpa-onnx conversion of the same model (csukuangfj/sherpa-onnx-…)",
             has[JOINER] ? "encoder, decoder and joiner" : "encoder and decoder");
        goto done;
    }
    bool whisper_prefixed = false;
    for (int i = 0; i < n_onnx && !whisper_prefixed; i++) {
        char prefix[256], tp[256];
        bundle_prefix(onnx[i]->name, prefix, sizeof prefix);
        if (!whisper_prefix(prefix)) continue;
        for (int t = 0; t < n_tokens && !whisper_prefixed; t++) {
            tokens_prefix(tokens[t]->name, tp, sizeof tp);
            whisper_prefixed = !SDL_strcmp(tp, prefix);
        }
    }
    bool transducer = has[JOINER], whisper = !transducer && (whisper_named || whisper_prefixed);
    if (!transducer && !whisper) {
        fail(error, error_size, "Couldn't tell which kind of speech model this is (found %s). Encoder and decoder files alone don't identify a model family, and Earheart won't guess one. " SUPPORTED_STT ".", sample);
        goto done;
    }
    const char *model_type = whisper ? "whisper" : find_ci(full, "nemo") || find_ci(full, "parakeet") ? "nemo_transducer" : "transducer";

    // One version per precision that has an encoder, the other parts in that precision or, failing
    // that, in full precision.
    int n_prec = 0, n = 0;
    for (int i = 0; i < n_onnx; i++) {
        char p[16];
        precision_of(onnx[i]->path, p, sizeof p);
        bool seen = false;
        for (int k = 0; k < n_prec; k++) seen = seen || !SDL_strcmp(precisions[k], p);
        if (!seen) SDL_strlcpy(precisions[n_prec++], p, 16);
    }
    for (int k = 0; k < n_prec; k++) {
        const entry *parts[3] = { NULL, NULL, NULL }, *fallback[3] = { NULL, NULL, NULL };
        for (int i = 0; i < n_onnx; i++) {
            char p[16];
            precision_of(onnx[i]->path, p, sizeof p);
            int part = part_of(onnx[i]->name);
            if (!SDL_strcmp(p, precisions[k]) && !parts[part]) parts[part] = onnx[i];
            if (!SDL_strcmp(p, "fp32") && !fallback[part]) fallback[part] = onnx[i];
        }
        if (!parts[ENCODER]) continue;
        if (!parts[DECODER]) parts[DECODER] = fallback[DECODER];
        if (!parts[DECODER]) continue;
        if (transducer && !parts[JOINER]) parts[JOINER] = fallback[JOINER];
        if (transducer && !parts[JOINER]) continue;
        int n_parts = transducer ? 3 : 2;
        gs_jval *v = gs_jval_object(), *list = gs_jval_array(), *sherpa = gs_jval_object();
        double total = 0;
        for (int p = 0; p < n_parts; p++) gs_jval_push(list, file_json(r, commit, parts[p])), total += parts[p]->bytes;
        // Weights kept beside a part, in its folder and under its name.
        for (int i = 0; i < count; i++) {
            const entry *f = by_depth[i];
            if (!ends_ci(f->name, ".weights") && !ends_ci(f->name, ".data") && !ends_ci(f->name, ".onnx_data")) continue;
            bool beside = false;
            for (int p = 0; p < n_parts && !beside; p++) {
                size_t stem = SDL_strlen(parts[p]->name) - 5;
                beside = same_dir(f->path, parts[p]->path) && !SDL_strncmp(f->name, parts[p]->name, stem);
            }
            if (beside) gs_jval_push(list, file_json(r, commit, f)), total += f->bytes;
        }
        // The symbol table named like the bundle, or the plain one, or the first there is.
        char prefix[256], tp[256];
        bundle_prefix(parts[ENCODER]->name, prefix, sizeof prefix);
        const entry *table = NULL;
        for (int t = 0; t < n_tokens && !table; t++) {
            tokens_prefix(tokens[t]->name, tp, sizeof tp);
            if (!SDL_strcmp(tp, prefix)) table = tokens[t];
        }
        for (int t = 0; t < n_tokens && !table; t++) {
            tokens_prefix(tokens[t]->name, tp, sizeof tp);
            if (!tp[0]) table = tokens[t];
        }
        if (!table) table = tokens[0];
        gs_jval_push(list, file_json(r, commit, table));
        total += table->bytes;
        gs_jval_set(v, "label", gs_jval_str(precisions[k]));
        gs_jval_set(v, "totalBytes", gs_jval_num(total));
        gs_jval_set(v, "files", list);
        gs_jval_set(sherpa, "encoder", gs_jval_str(parts[ENCODER]->name));
        gs_jval_set(sherpa, "decoder", gs_jval_str(parts[DECODER]->name));
        if (transducer) gs_jval_set(sherpa, "joiner", gs_jval_str(parts[JOINER]->name));
        gs_jval_set(sherpa, "tokens", gs_jval_str(table->name));
        gs_jval_set(sherpa, "modelType", gs_jval_str(model_type));
        gs_jval_set(v, "sherpa", sherpa);
        variants[n++] = (ranked){ v, precision_priority(precisions[k]), total };
    }
    if (!n) {
        fail(error, error_size, "Could not assemble a complete model from this repository — %s never share a precision (%s)", whisper ? "encoder and decoder" : "encoder, decoder and joiner", sample);
        goto done;
    }
    out = ranked_result(r, commit, variants, n);
done:
    SDL_free(by_depth);
    SDL_free(onnx);
    SDL_free(tokens);
    SDL_free(all);
    SDL_free(variants);
    SDL_free(precisions);
    return out;
}

gs_jval *eh_hf_listing(eh_model_kind kind, const eh_hf_repo *r, int info_status, const gs_jval *info, int tree_status, const gs_jval *tree, char *error, size_t error_size) {
    if (!status_ok(info_status, error, error_size)) return NULL;
    if (truthy(gs_jval_get(info, "gated"))) { fail(error, error_size, "This repository is gated — Earheart can only download public models"); return NULL; }
    if (!status_ok(tree_status, error, error_size)) return NULL;
    char commit[256];
    eh_hf_commit(r, info, commit, sizeof commit);
    entry *files;
    int count = read_files(tree, &files);
    gs_jval *out = kind == EH_MODEL_CLEANUP ? list_ggufs(r, commit, files, count, error, error_size) : list_stt(r, commit, files, count, error, error_size);
    SDL_free(files);
    return out;
}

void eh_hf_commit(const eh_hf_repo *r, const gs_jval *info, char *out, size_t size) {
    const char *sha = gs_jval_string(gs_jval_get(info, "sha"), "");
    SDL_strlcpy(out, *sha ? sha : r->ref[0] ? r->ref : "main", size);
}

/* ---------------- the model made from a version ---------------- */

static void slug(const char *s, char *out, size_t size) {
    size_t at = 0;
    bool gap = false;
    for (; *s && at + 2 < size; s++) {
        char c = lower(*s);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (gap && at) out[at++] = '-';
            out[at++] = c;
            gap = false;
        } else {
            gap = true;
        }
    }
    out[at] = 0;
}

gs_jval *eh_hf_model(eh_model_kind kind, const char *repo_full, const gs_jval *variant) {
    const char *label = gs_jval_string(gs_jval_get(variant, "label"), ""), *slash = SDL_strchr(repo_full, '/');
    const char *repo_name = slash && slash[1] ? slash + 1 : repo_full;
    const gs_jval *files = gs_jval_get(variant, "files");
    double total = gs_jval_number(gs_jval_get(variant, "totalBytes"), 0);
    char a[300], b[200], text[900], size_note[48] = "";
    gs_jval *m = gs_jval_object(), *source = gs_jval_object(), *list = gs_jval_array();
    slug(repo_full, a, sizeof a);
    slug(label, b, sizeof b);
    SDL_snprintf(text, sizeof text, "custom-%s-%s", a, b);
    gs_jval_set(m, "id", gs_jval_str(text));
    gs_jval_set(m, "kind", gs_jval_str(kind == EH_MODEL_CLEANUP ? "cleanup" : "stt"));
    SDL_snprintf(text, sizeof text, "%s · %s", repo_name, label);
    gs_jval_set(m, "label", gs_jval_str(text));
    int with_sum = 0, count = gs_jval_count(files);
    for (int i = 0; i < count; i++) with_sum += gs_jval_get(gs_jval_at(files, i), "sha256") != NULL;
    if (total != 0) SDL_snprintf(size_note, sizeof size_note, " · ~%.*f GB", total < 1e9 ? 2 : 1, total / 1e9);
    SDL_snprintf(text, sizeof text, "Runs on this computer · Hugging Face · %s · %s%s%s", repo_full, label, size_note,
                 !with_sum ? " · not checksum-verified" : with_sum == count ? " · checksum-verified" : " · partially checksum-verified");
    gs_jval_set(m, "note", gs_jval_str(text));
    gs_jval_set(m, "custom", gs_jval_bool(true));
    gs_jval_set(source, "repo", gs_jval_str(repo_full));
    gs_jval_set(source, kind == EH_MODEL_CLEANUP ? "quant" : "variant", gs_jval_str(label));
    gs_jval_set(m, "source", source);
    for (int i = 0; i < count; i++) {
        const gs_jval *f = gs_jval_at(files, i);
        gs_jval *o = gs_jval_object();
        gs_jval_set(o, "name", gs_jval_clone(gs_jval_get(f, "name")));
        gs_jval_set(o, "url", gs_jval_clone(gs_jval_get(f, "url")));
        if (gs_jval_get(f, "bytes")) gs_jval_set(o, "bytes", gs_jval_clone(gs_jval_get(f, "bytes")));
        if (gs_jval_get(f, "sha256")) gs_jval_set(o, "sha256", gs_jval_clone(gs_jval_get(f, "sha256")));
        gs_jval_push(list, o);
    }
    gs_jval_set(m, "files", list);
    if (kind == EH_MODEL_CLEANUP) {
        gs_jval *gguf = gs_jval_object();
        gs_jval_set(gguf, "file", gs_jval_clone(gs_jval_get(gs_jval_at(files, 0), "name")));
        gs_jval_set(m, "gguf", gguf);
    } else {
        gs_jval_set(m, "sherpa", gs_jval_clone(gs_jval_get(variant, "sherpa")));
    }
    return m;
}

/* ---------------- asking Hugging Face ---------------- */

static gs_jval *get_json(const char *url, int *status, char *error, size_t error_size) {
    static const char *const headers[] = { "Accept: application/json", NULL };
    gs_http_request req = { .url = url, .headers = headers, .follow = true, .timeout_ms = 15000 };
    char *body = NULL;
    size_t len = 0;
    *status = gs_http_fetch(&req, &body, &len);
    if (*status <= 0) {
        fail(error, error_size, "Could not reach Hugging Face");
        SDL_free(body);
        return NULL;
    }
    gs_jval *v = *status >= 200 && *status < 300 ? gs_jval_parse(body ? body : "", len) : gs_jval_null();
    if (!v) fail(error, error_size, "Unexpected response from Hugging Face");
    SDL_free(body);
    return v;
}

gs_jval *eh_hf_discover(eh_model_kind kind, const char *input, char *error, size_t error_size) {
    eh_hf_repo r;
    if (!eh_hf_parse(input, &r, error, error_size)) return NULL;
    char url[1024], commit[256], ref[400] = "";
    encode(ref, sizeof ref, r.ref, SDL_strlen(r.ref));
    SDL_snprintf(url, sizeof url, "https://huggingface.co/api/models/%s/%s%s%s", r.owner, r.repo, r.ref[0] ? "/revision/" : "", ref);
    int info_status = 0, tree_status = 200;
    gs_jval *info = get_json(url, &info_status, error, error_size), *tree = NULL, *out = NULL;
    if (!info) return NULL;
    // The first answer may already settle it; the files are asked for only when it does not.
    if (info_status >= 200 && info_status < 300 && !truthy(gs_jval_get(info, "gated"))) {
        eh_hf_commit(&r, info, commit, sizeof commit);
        char c[400] = "";
        encode(c, sizeof c, commit, SDL_strlen(commit));
        SDL_snprintf(url, sizeof url, "https://huggingface.co/api/models/%s/%s/tree/%s?recursive=true", r.owner, r.repo, c);
        tree = get_json(url, &tree_status, error, error_size);
        if (!tree) { gs_jval_free(info); return NULL; }
    }
    out = eh_hf_listing(kind, &r, info_status, info, tree_status, tree, error, error_size);
    gs_jval_free(info);
    gs_jval_free(tree);
    return out;
}
