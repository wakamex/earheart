#include "catalog.h"
#include <SDL3/SDL.h>
#include "gs_json.h"
#include "gs_sha256.h"
#include "paths.h"

const char *eh_model_kind_dir(eh_model_kind kind) { return kind == EH_MODEL_STT ? "stt" : "cleanup"; }

const eh_model *eh_catalog_find(eh_model_kind kind, const char *id) {
    for (int i = 0; i < eh_catalog_count; i++) {
        const eh_model *m = &eh_catalog[i];
        if (m->kind == kind && (id ? !SDL_strcmp(m->id, id) : m->is_default)) return m;
    }
    return NULL;
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
        // Names and checksums hold nothing JSON would escape.
        char item[512];
        int n = SDL_snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"sha256\":\"%s\"}", i ? "," : "", files[i]->name, files[i]->sha256);
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
