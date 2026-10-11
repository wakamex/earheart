#include "model_jobs.h"
#include <SDL3/SDL.h>
#include "download.h"
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
