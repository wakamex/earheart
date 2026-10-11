// Downloading a catalogue model: each file streamed to a .part beside its place, resumed where a
// broken download left off, hashed as it arrives, and moved into place only when its size and
// SHA-256 match; then a completion marker with every file's size and the definition's
// fingerprint, in the format of main/engines/model-manager.js, so either build recognises the
// other's installs. Each call waits for the network; call it without the loop's lock.
#include "download.h"
#include <SDL3/SDL.h>
#include "gs_http.h"
#include "gs_sha256.h"
#include "paths.h"

typedef struct {
    SDL_IOStream *io;
    gs_sha256 hash;
    long long file_received, done_before;  // of this file; of the files before it
    long long total;
    const char *name;
    eh_download_progress *progress;
    void *user;
    bool write_failed;
} sink;

static bool take(void *user, const uint8_t *data, size_t len) {
    sink *s = user;
    if (SDL_WriteIO(s->io, data, len) != len) { s->write_failed = true; return false; }
    gs_sha256_update(&s->hash, data, len);
    s->file_received += (long long)len;
    if (s->progress) s->progress(s->user, s->done_before + s->file_received, s->total, s->name);
    return true;
}

static long long file_size(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE ? (long long)info.size : -1;
}

static bool verified(const char *path, const eh_model_file *f) {
    char hex[65];
    return file_size(path) == f->bytes && gs_sha256_file(path, hex) && !SDL_strcmp(hex, f->sha256);
}

static bool fetch_file(const char *dir, const eh_model_file *f, sink *s, SDL_AtomicInt *stop, char *error, size_t error_size) {
    char dest[1200], part[1210];
    SDL_snprintf(dest, sizeof dest, "%s" EH_SEP "%s", dir, f->name);
    SDL_snprintf(part, sizeof part, "%s.part", dest);
    for (int attempt = 0; attempt < 2; attempt++) {
        // A part of the right length is a finished download that was never checked. A longer one,
        // or one that fails its check, is not this file.
        long long have = file_size(part);
        if (have == f->bytes && verified(part, f)) return SDL_RenamePath(part, dest);
        if (have >= f->bytes || have < 0) { SDL_RemovePath(part); have = 0; }
        gs_sha256_init(&s->hash);
        s->file_received = 0;
        s->name = f->name;
        if (have > 0) {
            // The bytes already here are hashed again, so the sum covers the whole file.
            SDL_IOStream *old = SDL_IOFromFile(part, "rb");
            static uint8_t buf[1 << 16];
            for (size_t n; old && (n = SDL_ReadIO(old, buf, sizeof buf)) > 0;) gs_sha256_update(&s->hash, buf, n);
            if (old) SDL_CloseIO(old);
            s->file_received = have;
            if (s->progress) s->progress(s->user, s->done_before + have, s->total, f->name);
        }
        s->io = SDL_IOFromFile(part, have > 0 ? "ab" : "wb");
        if (!s->io) { SDL_snprintf(error, error_size, "Could not write %s: %s", f->name, SDL_GetError()); return false; }
        gs_http_request r = { .url = f->url, .follow = true };
        int status = gs_http_stream(&r, have, take, s, stop);
        SDL_CloseIO(s->io);
        if (SDL_GetAtomicInt(stop)) { SDL_strlcpy(error, "aborted", error_size); return false; }
        if (s->write_failed) { SDL_snprintf(error, error_size, "Could not write %s: the disk may be full", f->name); return false; }
        if (status != 200 && status != 206) {
            // A server that will not resume from here: the part is dropped and the file fetched whole.
            if (have > 0 && attempt == 0) { SDL_RemovePath(part); continue; }
            if (!status) SDL_snprintf(error, error_size, "Download failed for %s: no answer from the server", f->name);
            else SDL_snprintf(error, error_size, "Download failed for %s: HTTP %d", f->name, status);
            return false;
        }
        uint8_t digest[32];
        char hex[65];
        gs_sha256_final(&s->hash, digest);
        gs_sha256_hex(digest, hex);
        if (s->file_received == f->bytes && !SDL_strcmp(hex, f->sha256)) return SDL_RenamePath(part, dest);
        // Short of its length the connection broke, and the part stays for the next try. At its
        // length, or past it (a server that ignored the range), the bytes are wrong.
        if (s->file_received < f->bytes) { SDL_snprintf(error, error_size, "Download failed for %s: the connection broke", f->name); return false; }
        SDL_RemovePath(part);
        if (have > 0 && attempt == 0) continue;
        SDL_snprintf(error, error_size, "%s mismatch for %s", s->file_received == f->bytes ? "Checksum" : "Size", f->name);
        return false;
    }
    return false;
}

bool eh_model_download(const eh_model *m, SDL_AtomicInt *stop, eh_download_progress *progress, void *user, char *error, size_t error_size) {
    char dir[1100], path[1300];
    SDL_snprintf(dir, sizeof dir, "%s" EH_SEP "%s" EH_SEP "%s", eh_models_own_dir(), eh_model_kind_dir(m->kind), m->id);
    if (!SDL_CreateDirectory(dir)) { SDL_snprintf(error, error_size, "Could not make %s: %s", dir, SDL_GetError()); return false; }
    sink s = { .progress = progress, .user = user };
    for (int i = 0; i < m->file_count; i++) s.total += m->files[i].bytes;
    for (int i = 0; i < m->file_count; i++) {
        const eh_model_file *f = &m->files[i];
        SDL_snprintf(path, sizeof path, "%s" EH_SEP "%s", dir, f->name);
        // A file already in place and sound is kept; one that is not is fetched again.
        if (file_size(path) >= 0 && !verified(path, f)) SDL_RemovePath(path);
        if (file_size(path) < 0 && !fetch_file(dir, f, &s, stop, error, error_size)) return false;
        s.done_before += f->bytes;
        if (progress) progress(user, s.done_before, s.total, f->name);
    }
    // The marker: each file's size, and which definition these bytes belong to.
    char fingerprint[65], *json = NULL;
    size_t n = 0, cap = 4096;
    json = SDL_malloc(cap);
    eh_model_fingerprint(m, fingerprint);
    n += (size_t)SDL_snprintf(json + n, cap - n, "{\"files\":{");
    for (int i = 0; i < m->file_count; i++) n += (size_t)SDL_snprintf(json + n, cap - n, "%s\"%s\":%lld", i ? "," : "", m->files[i].name, m->files[i].bytes);
    n += (size_t)SDL_snprintf(json + n, cap - n, "},\"fingerprint\":\"%s\"}", fingerprint);
    SDL_snprintf(path, sizeof path, "%s" EH_SEP ".complete", dir);
    bool ok = !SDL_GetAtomicInt(stop) && SDL_SaveFile(path, json, n);
    SDL_free(json);
    if (!ok) SDL_strlcpy(error, SDL_GetAtomicInt(stop) ? "aborted" : SDL_GetError(), error_size);
    return ok;
}

bool eh_model_remove(const eh_model *m, char *error, size_t error_size) {
    char dir[1100], path[1300];
    SDL_snprintf(dir, sizeof dir, "%s" EH_SEP "%s" EH_SEP "%s", eh_models_own_dir(), eh_model_kind_dir(m->kind), m->id);
    int count = 0;
    char **names = SDL_GlobDirectory(dir, NULL, 0, &count);
    bool ok = true;
    // The marker first, so a removal cut short leaves a model that reads as not installed.
    SDL_snprintf(path, sizeof path, "%s" EH_SEP ".complete", dir);
    SDL_RemovePath(path);
    for (int i = 0; names && i < count; i++) {
        SDL_snprintf(path, sizeof path, "%s" EH_SEP "%s", dir, names[i]);
        ok = SDL_RemovePath(path) && ok;
    }
    SDL_free(names);
    ok = (SDL_RemovePath(dir) || !SDL_GetPathInfo(dir, NULL)) && ok;
    if (!ok) SDL_snprintf(error, error_size, "Could not remove %s: %s", m->label ? m->label : m->id, SDL_GetError());
    return ok;
}
