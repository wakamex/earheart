#include "catalog.h"
#include <SDL3/SDL.h>
#include <string.h>
#include "gs_json.h"
#include "gs_sha256.h"
#include "jv.h"
#include "paths.h"

const char *eh_model_kind_dir(eh_model_kind kind) { return kind == EH_MODEL_STT ? "stt" : "cleanup"; }

/* ---------------- the user's own models ---------------- */

typedef struct { eh_model model; char *definition; } custom;
static custom **customs;  // the ones in force
static int custom_count;

bool eh_path_segment(const char *v) {
    size_t n = v ? SDL_strlen(v) : 0;
    if (!n || !SDL_strcmp(v, ".") || !SDL_strcmp(v, "..") || v[n - 1] == '.' || v[n - 1] == ' ') return false;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++)
        if (*p < 0x20 || SDL_strchr("\\/:*?\"<>|", *p)) return false;
    // The names Windows keeps for devices, with or without an extension.
    static const char *const reserved[] = { "con", "prn", "aux", "nul" };
    size_t stem = strcspn(v, ".");
    for (int i = 0; i < 4; i++)
        if (stem == 3 && !SDL_strncasecmp(v, reserved[i], 3)) return false;
    if (stem == 4 && (!SDL_strncasecmp(v, "com", 3) || !SDL_strncasecmp(v, "lpt", 3)) && v[3] >= '1' && v[3] <= '9') return false;
    return true;
}

static const char *kept(const eh_jv *v) {
    const char *s = eh_jv_string(v, NULL);
    return s ? SDL_strdup(s) : NULL;
}

static custom *make_custom(const eh_jv *def, const char *text) {
    const char *id = eh_jv_string(eh_jv_get(def, "id"), ""), *kind = eh_jv_string(eh_jv_get(def, "kind"), "");
    const eh_jv *files = eh_jv_get(def, "files"), *sherpa = eh_jv_get(def, "sherpa");
    if (!*id || (SDL_strcmp(kind, "stt") && SDL_strcmp(kind, "cleanup"))) return NULL;
    // Everything of it that becomes part of a path.
    bool safe = eh_path_segment(id);
    for (int i = 0; i < eh_jv_count(files); i++) safe = safe && eh_path_segment(eh_jv_string(eh_jv_get(eh_jv_at(files, i), "name"), ""));
    static const char *const parts[] = { "encoder", "decoder", "joiner", "tokens" };
    for (int i = 0; i < 4; i++) {
        const eh_jv *part = eh_jv_get(sherpa, parts[i]);
        if (part && part->type != EH_JV_NULL) safe = safe && eh_path_segment(eh_jv_string(part, ""));
    }
    const eh_jv *gguf = eh_jv_path(def, "gguf.file");
    if (gguf && gguf->type != EH_JV_NULL) safe = safe && eh_path_segment(eh_jv_string(gguf, ""));
    if (!safe) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "ignoring custom model with an invalid path segment: %s", id);
        return NULL;
    }
    custom *c = SDL_calloc(1, sizeof *c);
    c->definition = SDL_strdup(text);
    eh_model *m = &c->model;
    m->id = SDL_strdup(id);
    m->label = kept(eh_jv_get(def, "label"));
    m->note = kept(eh_jv_get(def, "note"));
    m->kind = !SDL_strcmp(kind, "stt") ? EH_MODEL_STT : EH_MODEL_CLEANUP;
    m->custom = true;
    m->file_count = eh_jv_count(files);
    eh_model_file *list = SDL_calloc((size_t)m->file_count + 1, sizeof *list);
    for (int i = 0; i < m->file_count; i++) {
        const eh_jv *f = eh_jv_at(files, i);
        list[i].name = SDL_strdup(eh_jv_string(eh_jv_get(f, "name"), ""));
        list[i].url = SDL_strdup(eh_jv_string(eh_jv_get(f, "url"), ""));
        list[i].sha256 = SDL_strdup(eh_jv_string(eh_jv_get(f, "sha256"), ""));
        list[i].bytes = (long long)eh_jv_number(eh_jv_get(f, "bytes"), 0);
    }
    m->files = list;
    m->gguf = kept(gguf);
    m->sherpa = (eh_stt_files){ kept(eh_jv_get(sherpa, "encoder")), kept(eh_jv_get(sherpa, "decoder")), kept(eh_jv_get(sherpa, "joiner")), kept(eh_jv_get(sherpa, "tokens")), kept(eh_jv_get(sherpa, "modelType")) };
    return c;
}

void eh_models_set_custom(const eh_jv *list) {
    int count = list && list->type == EH_JV_ARRAY ? eh_jv_count(list) : 0, n = 0;
    custom **next = SDL_calloc((size_t)count + 1, sizeof *next);
    for (int i = 0; i < count; i++) {
        const eh_jv *def = eh_jv_at(list, i);
        if (!def || def->type != EH_JV_OBJECT) continue;
        char *text = eh_jv_write(def, false);
        // The same definition as before is the same model: whoever holds it keeps a good pointer.
        custom *c = NULL;
        for (int k = 0; k < custom_count && !c; k++)
            if (customs[k] && !SDL_strcmp(customs[k]->definition, text)) c = customs[k], customs[k] = NULL;
        if (!c) c = make_custom(def, text);
        if (c) next[n++] = c;
        SDL_free(text);
    }
    // A model no longer listed is not freed: a download of it may still be ending.
    SDL_free(customs);
    customs = next;
    custom_count = n;
}

const eh_model *eh_catalog_find(eh_model_kind kind, const char *id) {
    for (int i = 0; i < eh_catalog_count; i++) {
        const eh_model *m = &eh_catalog[i];
        if (m->kind == kind && (id ? !SDL_strcmp(m->id, id) : m->is_default)) return m;
    }
    for (int i = 0; id && i < custom_count; i++)
        if (customs[i]->model.kind == kind && !SDL_strcmp(customs[i]->model.id, id)) return &customs[i]->model;
    return NULL;
}

int eh_models_list(eh_model_kind kind, const eh_model **out, int max) {
    int n = 0;
    for (int i = 0; i < eh_catalog_count && n < max; i++)
        if (eh_catalog[i].kind == kind) out[n++] = &eh_catalog[i];
    for (int i = 0; i < custom_count && n < max; i++)
        if (customs[i]->model.kind == kind) out[n++] = &customs[i]->model;
    return n;
}

static char model_dirs[4][1024];
static int model_dir_count;

void eh_models_set_dirs(const char *const *dirs, int count) {
    model_dir_count = SDL_min(count, (int)SDL_arraysize(model_dirs));
    for (int i = 0; i < model_dir_count; i++) SDL_strlcpy(model_dirs[i], dirs[i], sizeof model_dirs[i]);
}

const char *eh_models_own_dir(void) { return model_dir_count ? model_dirs[0] : "."; }

static int by_name(const void *a, const void *b) { return SDL_strcmp((*(const eh_model_file *const *)a)->name, (*(const eh_model_file *const *)b)->name); }

void eh_model_fingerprint(const eh_model *m, char hex[65]) {
    const eh_model_file **files = SDL_malloc(sizeof(*files) * (size_t)SDL_max(1, m->file_count));
    for (int i = 0; i < m->file_count; i++) files[i] = &m->files[i];
    SDL_qsort((void *)files, (size_t)m->file_count, sizeof(*files), by_name);
    gs_sha256 s;
    gs_sha256_init(&s);
    gs_sha256_update(&s, "[", 1);
    for (int i = 0; i < m->file_count; i++) {
        // Names, checksums and addresses hold nothing JSON would escape. A file without a
        // checksum is told apart by where it comes from and how long it is.
        char item[2600], bytes[32] = "null";
        const eh_model_file *f = files[i];
        if (f->bytes) SDL_snprintf(bytes, sizeof bytes, "%lld", f->bytes);
        int n = f->sha256 && f->sha256[0] ? SDL_snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"sha256\":\"%s\"}", i ? "," : "", f->name, f->sha256)
                                          : SDL_snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"url\":%s%s%s,\"bytes\":%s}", i ? "," : "", f->name, f->url && f->url[0] ? "\"" : "", f->url && f->url[0] ? f->url : "null", f->url && f->url[0] ? "\"" : "", bytes);
        gs_sha256_update(&s, item, (size_t)n);
    }
    gs_sha256_update(&s, "]", 1);
    uint8_t digest[32];
    gs_sha256_final(&s, digest);
    gs_sha256_hex(digest, hex);
    SDL_free((void *)files);
}

static bool installed_in(const char *base, const eh_model *m, char *dir, size_t dir_size) {
    char path[1200];
    SDL_snprintf(dir, dir_size, "%s" EH_SEP "%s" EH_SEP "%s", base, eh_model_kind_dir(m->kind), m->id);
    SDL_snprintf(path, sizeof path, "%s" EH_SEP ".complete", dir);
    size_t len;
    char *raw = SDL_LoadFile(path, &len);
    if (!raw) return false;  // no marker: not installed
    // An empty or unreadable marker is one from before sizes were recorded: presence is enough.
    gs_json *doc = len ? gs_json_parse(raw, len) : NULL;
    gs_jv root = gs_json_root(doc), sizes = gs_json_get(root, "files");
    const char *fingerprint = gs_json_str(gs_json_get(root, "fingerprint"), NULL);
    char mine[65];
    eh_model_fingerprint(m, mine);
    bool ok = !fingerprint || !SDL_strcmp(fingerprint, mine);  // else bytes from another revision of this id
    for (int i = 0; ok && i < m->file_count; i++) {
        SDL_PathInfo info;
        SDL_snprintf(path, sizeof path, "%s" EH_SEP "%s", dir, m->files[i].name);
        bool there = SDL_GetPathInfo(path, &info);
        gs_jv expected = gs_json_get(sizes, m->files[i].name);
        ok = there && (!expected || (double)info.size == gs_json_num(expected, -1));
    }
    gs_json_free(doc);
    SDL_free(raw);
    return ok;
}

bool eh_model_installed(const eh_model *m, char *dir, size_t dir_size) {
    char scratch[1024];
    if (!dir) { dir = scratch; dir_size = sizeof scratch; }
    for (int i = 0; i < model_dir_count; i++)
        if (installed_in(model_dirs[i], m, dir, dir_size)) return true;
    return false;
}
