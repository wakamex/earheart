#include "history.h"
#include <SDL3/SDL.h>
#include "files.h"

static struct { char *path; eh_jv *items; } H;

void eh_history_init(const char *path) {
    SDL_free(H.path);
    eh_jv_free(H.items);
    H.path = SDL_strdup(path);
    H.items = NULL;
}

static eh_jv *load(void) {
    if (H.items) return H.items;
    H.items = eh_jv_array();
    size_t len;
    char *text = SDL_LoadFile(H.path, &len);
    if (!text) return H.items;
    eh_jv *parsed = eh_jv_parse(text, len);
    SDL_free(text);
    if (parsed && parsed->type == EH_JV_ARRAY) {
        // Entries without a text are not entries.
        for (int i = 0; i < parsed->count; i++)
            if (eh_jv_string(eh_jv_get(parsed->items[i], "text"), NULL)) eh_jv_push(H.items, eh_jv_clone(parsed->items[i]));
    } else {
        // Not a history: moved aside, so the next write cannot destroy what was there.
        char backup[1200];
        SDL_snprintf(backup, sizeof backup, "%s.corrupt-%lld", H.path, (long long)SDL_GetTicksNS());
        if (SDL_RenamePath(H.path, backup)) SDL_Log("corrupt history preserved at %s", backup);
        else SDL_Log("corrupt history backup failed: %s", SDL_GetError());
    }
    eh_jv_free(parsed);
    return H.items;
}

const eh_jv *eh_history_list(void) { return load(); }

void eh_history_add(const char *raw, const char *text, bool cleaned, bool incomplete, int limit) {
    eh_jv *items = load(), *entry = eh_jv_object();
    eh_jv_set(entry, "raw", eh_jv_str(raw));
    eh_jv_set(entry, "text", eh_jv_str(text));
    eh_jv_set(entry, "cleaned", eh_jv_bool(cleaned));
    if (incomplete) eh_jv_set(entry, "incomplete", eh_jv_bool(true));
    // The time as JavaScript's toISOString writes it: UTC, to the millisecond.
    SDL_Time now;
    SDL_DateTime dt;
    char at[40] = "";
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, false))
        SDL_snprintf(at, sizeof at, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, dt.nanosecond / 1000000);
    eh_jv_set(entry, "at", eh_jv_str(at));
    // Newest first.
    eh_jv_push(items, entry);
    for (int i = items->count - 1; i > 0; i--) items->items[i] = items->items[i - 1];
    items->items[0] = entry;
    for (int max = limit > 0 ? limit : 100; items->count > max;) eh_jv_free(items->items[--items->count]);
    char *json = eh_jv_write(items, true), error[256];
    if (!eh_write_file_atomic(H.path, json, SDL_strlen(json), error, sizeof error)) SDL_Log("history write failed: %s", error);
    SDL_free(json);
}

void eh_history_clear(void) {
    eh_jv_free(H.items);
    H.items = eh_jv_array();
    if (SDL_GetPathInfo(H.path, NULL) && !SDL_RemovePath(H.path)) SDL_Log("history clear failed: %s", SDL_GetError());
}
