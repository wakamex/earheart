#include "model_jobs.h"
#include <SDL3/SDL.h>
#include "download.h"
#include "hf.h"
#include "gs_json.h"
#include "settings.h"
#include "loop.h"

typedef struct {
    const eh_model *model;
    eh_model_job job;
    SDL_AtomicInt stop;
    SDL_AtomicU32 received_mb, total_mb;  // written by the download, which runs without the lock
    bool removing;
} slot;

static slot slots[32];

static slot *slot_of(const eh_model *m, bool make) {
    slot *free_slot = NULL;
    for (size_t i = 0; i < SDL_arraysize(slots); i++) {
        if (slots[i].model == m) return &slots[i];
        if (!free_slot && (!slots[i].model || (!slots[i].job.active && !slots[i].removing))) free_slot = &slots[i];
    }
    if (!make || !free_slot) return NULL;
    SDL_memset(free_slot, 0, sizeof *free_slot);
    free_slot->model = m;
    return free_slot;
}

static void progress(void *user, long long received, long long total, const char *file) {
    slot *s = user;
    SDL_SetAtomicU32(&s->received_mb, (Uint32)(received >> 10));
    SDL_SetAtomicU32(&s->total_mb, (Uint32)(total >> 10));
}

static void run(void *user) {
    slot *s = user;
    char error[200] = "";
    bool ok;
    EH_AWAIT(ok = eh_model_download(s->model, &s->stop, progress, s, error, sizeof error));
    s->job.active = false;
    s->job.done = true;
    s->job.ok = ok;
    s->job.cancelled = !ok && SDL_GetAtomicInt(&s->stop);
    SDL_strlcpy(s->job.error, ok ? "" : error, sizeof s->job.error);
}

bool eh_model_job_start(const eh_model *m, char *error, size_t error_size) {
    slot *s = slot_of(m, true);
    if (!s) { SDL_strlcpy(error, "Too many downloads at once", error_size); return false; }
    if (s->job.active) { SDL_strlcpy(error, "Already downloading", error_size); return false; }
    if (s->removing) { SDL_strlcpy(error, "This model is being removed", error_size); return false; }
    s->job = (eh_model_job){ .active = true };
    SDL_SetAtomicInt(&s->stop, 0);
    SDL_SetAtomicU32(&s->received_mb, 0);
    SDL_SetAtomicU32(&s->total_mb, 0);
    eh_spawn(run, s);
    return true;
}

void eh_model_job_cancel(const eh_model *m) {
    slot *s = slot_of(m, false);
    if (s && s->job.active) SDL_SetAtomicInt(&s->stop, 1);
}

eh_model_job eh_model_job_get(const eh_model *m) {
    slot *s = slot_of(m, false);
    if (!s) return (eh_model_job){ 0 };
    s->job.received = (long long)SDL_GetAtomicU32(&s->received_mb) << 10;
    s->job.total = (long long)SDL_GetAtomicU32(&s->total_mb) << 10;
    return s->job;
}

void eh_model_job_forget(const eh_model *m) {
    slot *s = slot_of(m, false);
    if (s && !s->job.active) s->job = (eh_model_job){ 0 };
}

bool eh_model_jobs_active(void) {
    for (size_t i = 0; i < SDL_arraysize(slots); i++)
        if (slots[i].model && slots[i].job.active) return true;
    return false;
}

bool eh_model_job_remove(const eh_model *m, char *error, size_t error_size) {
    slot *s = slot_of(m, true);
    if (s && s->removing) { SDL_strlcpy(error, "This model is already being changed", error_size); return false; }
    if (s) s->removing = true;
    // A download of it is stopped first, and waited for: the lock is let go so it can end.
    while (s && s->job.active) {
        SDL_SetAtomicInt(&s->stop, 1);
        EH_AWAIT(SDL_Delay(20));
    }
    bool ok = eh_model_remove(m, error, error_size);
    if (s) s->removing = false, s->job = (eh_model_job){ 0 };
    return ok;
}

/* ---------------- the user's own models ---------------- */

static const gs_jval *custom_entry(const char *id) {
    const gs_jval *list = gs_jval_get(eh_settings(), "customModels");
    for (int i = 0; i < gs_jval_count(list); i++)
        if (!SDL_strcmp(gs_jval_string(gs_jval_get(gs_jval_at(list, i), "id"), ""), id)) return gs_jval_at(list, i);
    return NULL;
}

// The settings with `customModels` replaced: every entry but the one named, and `add` when given.
static bool save_customs(const char *without, gs_jval *add, const char *kind_reset, char *error, size_t error_size) {
    gs_jval *next = gs_jval_clone(eh_settings()), *list = gs_jval_array();
    const gs_jval *old = gs_jval_get(eh_settings(), "customModels");
    for (int i = 0; i < gs_jval_count(old); i++)
        if (SDL_strcmp(gs_jval_string(gs_jval_get(gs_jval_at(old, i), "id"), ""), without)) gs_jval_push(list, gs_jval_clone(gs_jval_at(old, i)));
    if (add) gs_jval_push(list, add);
    gs_jval_set(next, "customModels", list);
    if (kind_reset) {
        char path[40];
        SDL_snprintf(path, sizeof path, "%s.builtin.model", kind_reset);
        const eh_model *fallback = eh_catalog_find(!SDL_strcmp(kind_reset, "stt") ? EH_MODEL_STT : EH_MODEL_CLEANUP, NULL);
        if (fallback) gs_jval_set_path(next, path, gs_jval_str(fallback->id));
    }
    return eh_settings_save(next, error, error_size);
}

static const char *delete_error(const char *error) {
    return SDL_strstr(error, "ermission") || SDL_strstr(error, "busy") || SDL_strstr(error, "ccess") || SDL_strstr(error, "used by")
               ? "The model's files are in use or locked. Close anything using the model, then try again." : error;
}

bool eh_custom_model_add(eh_model_kind kind, const char *address, const char *variant, char *model_id, size_t id_size, char *error, size_t error_size) {
    char *asked = SDL_strdup(address), *wanted = SDL_strdup(variant ? variant : "");
    gs_jval *listing;
    EH_AWAIT(listing = eh_hf_discover(kind, asked, error, error_size));
    SDL_free(asked);
    if (!listing) { SDL_free(wanted); return false; }
    const gs_jval *variants = gs_jval_get(listing, "variants"), *chosen = NULL;
    const char *recommended = gs_jval_string(gs_jval_get(listing, "recommended"), "");
    for (int pass = 0; pass < 2 && !chosen; pass++)
        for (int i = 0; i < gs_jval_count(variants) && !chosen; i++)
            if (!SDL_strcmp(gs_jval_string(gs_jval_get(gs_jval_at(variants, i), "label"), ""), pass ? recommended : wanted)) chosen = gs_jval_at(variants, i);
    SDL_free(wanted);
    if (!chosen) {
        SDL_strlcpy(error, "That version is no longer available", error_size);
        gs_jval_free(listing);
        return false;
    }
    gs_jval *model = eh_hf_model(kind, gs_jval_string(gs_jval_get(listing, "repo"), ""), chosen);
    gs_jval_free(listing);
    const char *id = gs_jval_string(gs_jval_get(model, "id"), "");
    SDL_strlcpy(model_id, id, id_size);
    // An earlier definition under this id that is not this one: its files are another revision's.
    const gs_jval *existing = custom_entry(id);
    if (existing && !gs_jval_equal(existing, model)) {
        const eh_model *old = eh_catalog_find(!SDL_strcmp(gs_jval_string(gs_jval_get(existing, "kind"), ""), "stt") ? EH_MODEL_STT : EH_MODEL_CLEANUP, id);
        char why[200];
        if (old && !eh_model_job_remove(old, why, sizeof why)) {
            SDL_strlcpy(error, delete_error(why), error_size);
            gs_jval_free(model);
            return false;
        }
    }
    char *kept = SDL_strdup(id);
    bool ok = save_customs(kept, model, NULL, error, error_size);
    SDL_free(kept);
    return ok;
}

bool eh_custom_model_remove(const char *id, char *error, size_t error_size) {
    const gs_jval *entry = custom_entry(id);
    const char *kind = entry && !SDL_strcmp(gs_jval_string(gs_jval_get(entry, "kind"), ""), "stt") ? "stt" : "cleanup";
    const eh_model *m = eh_catalog_find(!SDL_strcmp(kind, "stt") ? EH_MODEL_STT : EH_MODEL_CLEANUP, id);
    char why[200] = "", path[40], *name = SDL_strdup(id);
    if (m && !eh_model_job_remove(m, why, sizeof why)) {
        SDL_strlcpy(error, delete_error(why), error_size);
        SDL_free(name);
        return false;
    }
    SDL_snprintf(path, sizeof path, "%s.builtin.model", kind);
    bool chosen = !SDL_strcmp(eh_setting_str(path), name);
    bool ok = save_customs(name, NULL, chosen ? kind : NULL, error, error_size);
    SDL_free(name);
    return ok;
}
