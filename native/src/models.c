#include "catalog.h"
#include <SDL3/SDL.h>

const char *eh_model_kind_dir(eh_model_kind kind) { return kind == EH_MODEL_STT ? "stt" : "cleanup"; }

const eh_model *eh_catalog_find(eh_model_kind kind, const char *id) {
    for (int i = 0; i < eh_catalog_count; i++) {
        const eh_model *m = &eh_catalog[i];
        if (m->kind == kind && (id ? !SDL_strcmp(m->id, id) : m->is_default)) return m;
    }
    return NULL;
}
