#include "history.h"
#include <SDL3/SDL.h>
#include "files.h"

static struct { char *path; gs_jval *items; } H;

void eh_history_init(const char *path) {
    SDL_free(H.path);
    gs_jval_free(H.items);
    H.path = SDL_strdup(path);
    H.items = NULL;
}

static gs_jval *load(void) {
    if (H.items) return H.items;
    H.items = gs_jval_array();
    size_t len;
    char *text = SDL_LoadFile(H.path, &len);
    if (!text) return H.items;
    gs_jval *parsed = gs_jval_parse(text, len);
    SDL_free(text);
    if (parsed && gs_jval_kind(parsed) == GS_JSON_ARRAY) {
        // Entries without a text are not entries.
        for (int i = 0; i < gs_jval_count(parsed); i++)
            if (gs_jval_string(gs_jval_get(gs_jval_at(parsed, i), "text"), NULL)) gs_jval_push(H.items, gs_jval_clone(gs_jval_at(parsed, i)));
    } else {
        // Not a history: moved aside, so the next write cannot destroy what was there.
        char backup[1200];
        SDL_snprintf(backup, sizeof backup, "%s.corrupt-%lld", H.path, (long long)SDL_GetTicksNS());
        if (SDL_RenamePath(H.path, backup)) SDL_Log("corrupt history preserved at %s", backup);
        else SDL_Log("corrupt history backup failed: %s", SDL_GetError());
    }
    gs_jval_free(parsed);
    return H.items;
}

const gs_jval *eh_history_list(void) { return load(); }

void eh_history_add(const char *raw, const char *text, bool cleaned, bool incomplete, int limit) {
    gs_jval *items = load(), *entry = gs_jval_object();
    gs_jval_set(entry, "raw", gs_jval_str(raw));
    gs_jval_set(entry, "text", gs_jval_str(text));
    gs_jval_set(entry, "cleaned", gs_jval_bool(cleaned));
    if (incomplete) gs_jval_set(entry, "incomplete", gs_jval_bool(true));
    // The time as JavaScript's toISOString writes it: UTC, to the millisecond.
    SDL_Time now;
    SDL_DateTime dt;
    char at[40] = "";
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, false))
        SDL_snprintf(at, sizeof at, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, dt.nanosecond / 1000000);
    gs_jval_set(entry, "at", gs_jval_str(at));
    // Newest first.
    gs_jval *kept = gs_jval_array();
    gs_jval_push(kept, entry);
    for (int i = 0, max = limit > 0 ? limit : 100; i < gs_jval_count(items) && i + 1 < max; i++) gs_jval_push(kept, gs_jval_clone(gs_jval_at(items, i)));
    gs_jval_free(items);
    items = H.items = kept;
    char *json = eh_json_text(items, true), error[256];
    if (!eh_write_file_atomic(H.path, json, SDL_strlen(json), error, sizeof error)) SDL_Log("history write failed: %s", error);
    SDL_free(json);
}

void eh_history_clear(void) {
    gs_jval_free(H.items);
    H.items = gs_jval_array();
    if (SDL_GetPathInfo(H.path, NULL) && !SDL_RemovePath(H.path)) SDL_Log("history clear failed: %s", SDL_GetError());
}
