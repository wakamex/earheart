#include "settings.h"
#include <string.h>
#include <SDL3/SDL.h>
#include <math.h>
#include "defaults.h"
#include "files.h"
#include "text.h"

static struct {
    char *path;
    gs_jval *defaults, *current;
    bool first_run;
    struct { eh_settings_listener *fn; void *user; } listeners[8];
    int listener_count;
} S;

static gs_jval *defaults(void) {
    if (!S.defaults) S.defaults = gs_jval_parse(eh_defaults_json, sizeof eh_defaults_json - 1);
    return S.defaults;
}

/* ---------------- bringing an old file up to date ---------------- */

// The default prompt was once stored wrapped at 80 columns; unwrapped, it is recognised and
// replaced by today's, so those users get prompt improvements too.
static char *unwrap_prompt(const char *text) {
    char *out = SDL_malloc(SDL_strlen(text) + 1);
    size_t n = 0;
    for (const char *paragraph = text;;) {
        const char *paragraph_end = SDL_strstr(paragraph, "\n\n");
        if (!paragraph_end) paragraph_end = paragraph + SDL_strlen(paragraph);
        // Within a paragraph a line that starts a list item stays a line; any other continues the one before.
        bool first = true;
        for (const char *line = paragraph;;) {
            const char *end = memchr(line, '\n', (size_t)(paragraph_end - line));
            if (!end) end = paragraph_end;
            char *raw = SDL_strndup(line, (size_t)(end - line)), *trimmed = eh_trim(raw);
            if (!first) out[n++] = !SDL_strncmp(trimmed, "- ", 2) ? '\n' : ' ';
            SDL_memcpy(out + n, trimmed, SDL_strlen(trimmed));
            n += SDL_strlen(trimmed);
            first = false;
            SDL_free(raw);
            SDL_free(trimmed);
            if (end == paragraph_end) break;
            line = end + 1;
        }
        if (!*paragraph_end) break;
        out[n++] = '\n';
        out[n++] = '\n';
        paragraph = paragraph_end + 2;
    }
    out[n] = 0;
    return out;
}

static void migrate(gs_jval *stored) {
    gs_jval *stt = gs_jval_get(stored, "stt"), *cleanup = gs_jval_get(stored, "cleanup");
    if (stt && gs_jval_kind(stt) == GS_JSON_OBJECT) {
        // A section saved before there was a built-in engine meant the remote one.
        const char *engine = gs_jval_string(gs_jval_get(stt, "engine"), NULL);
        if (!gs_jval_get(stt, "engine") || (engine && !SDL_strcmp(engine, "server"))) gs_jval_set(stt, "engine", gs_jval_str("remote"));
        if (gs_jval_number(gs_jval_path(stt, "livePreview.chunkSeconds"), 0) == 5) gs_jval_set_path(stt, "livePreview.chunkSeconds", gs_jval_num(10));
    }
    gs_jval_remove(stored, "sttServer");
    if (cleanup && gs_jval_kind(cleanup) == GS_JSON_OBJECT) {
        if (!gs_jval_get(cleanup, "engine")) gs_jval_set(cleanup, "engine", gs_jval_str("remote"));
        if (gs_jval_number(gs_jval_get(cleanup, "timeoutMs"), 0) == 60000) gs_jval_set(cleanup, "timeoutMs", gs_jval_num(120000));
        const char *prompt = gs_jval_string(gs_jval_get(cleanup, "systemPrompt"), NULL), *current = gs_jval_string(gs_jval_path(defaults(), "cleanup.systemPrompt"), "");
        if (prompt) {
            char *unwrapped = unwrap_prompt(prompt);
            if (!SDL_strcmp(unwrapped, current)) gs_jval_set(cleanup, "systemPrompt", gs_jval_str(current));
            SDL_free(unwrapped);
        }
        // A bare temperature from before styles existed becomes the custom style with nothing else filtering.
        gs_jval *temperature = gs_jval_get(cleanup, "temperature");
        if (!gs_jval_get(cleanup, "style") && temperature) {
            gs_jval *custom = gs_jval_object();
            gs_jval_set(custom, "temperature", gs_jval_clone(temperature));
            gs_jval_set(custom, "topP", gs_jval_num(EH_NEUTRAL_TOP_P));
            gs_jval_set(custom, "topK", gs_jval_num(EH_NEUTRAL_TOP_K));
            gs_jval_set(custom, "minP", gs_jval_num(EH_NEUTRAL_MIN_P));
            gs_jval_set(cleanup, "style", gs_jval_str("custom"));
            gs_jval_set(cleanup, "custom", custom);
            gs_jval_remove(cleanup, "temperature");
        }
    }
}

static void clamp(gs_jval *merged, const char *path, double min, double max) {
    gs_jval *v = gs_jval_path(merged, path);
    double n = gs_jval_number(v, NAN);
    gs_jval_set_path(merged, path, isfinite(n) ? gs_jval_num(floor(SDL_min(max, SDL_max(min, n)) + 0.5)) : gs_jval_clone(gs_jval_path(defaults(), path)));
}

static gs_jval *merged_over_defaults(const gs_jval *next) {
    gs_jval *merged = gs_jval_merge(defaults(), next);
    clamp(merged, "audio.maxRecordingSeconds", 10, 3600);
    clamp(merged, "engines.idleUnloadMinutes", 0, 240);
    return merged;
}

gs_jval *eh_settings_resolve(const gs_jval *stored) {
    gs_jval *copy = gs_jval_clone(stored);
    if (gs_jval_kind(copy) == GS_JSON_OBJECT) migrate(copy);
    gs_jval *merged = merged_over_defaults(copy);
    gs_jval_free(copy);
    return merged;
}

/* ---------------- the file ---------------- */

// The file's content as an object, or NULL. A file that is not a JSON object is moved aside, so
// the next save cannot overwrite what the user had.
static gs_jval *read_stored(const char *path) {
    size_t len;
    char *text = SDL_LoadFile(path, &len);
    if (!text) return NULL;
    gs_jval *parsed = gs_jval_parse(text, len);
    SDL_free(text);
    if (parsed && gs_jval_kind(parsed) == GS_JSON_OBJECT) return parsed;
    gs_jval_free(parsed);
    char backup[1200];
    SDL_snprintf(backup, sizeof backup, "%s.corrupt-%lld", path, (long long)SDL_GetTicksNS());
    if (SDL_RenamePath(path, backup)) SDL_Log("settings file was not valid JSON; preserved it at %s and started from defaults", backup);
    else SDL_Log("settings file was not valid JSON and could not be preserved: %s", SDL_GetError());
    return NULL;
}

void eh_settings_init(const char *path, const char *import_from) {
    SDL_free(S.path);
    gs_jval_free(S.current);
    S.path = SDL_strdup(path);
    bool exists = SDL_GetPathInfo(path, NULL);
    gs_jval *stored = exists ? read_stored(path) : NULL;
    bool imported = false;
    if (!exists && import_from && SDL_GetPathInfo(import_from, NULL)) {
        // The Electron build's settings. Its API keys are encrypted for that program alone and
        // cannot be read here; they are entered again.
        size_t len;
        char *text = SDL_LoadFile(import_from, &len);
        stored = text ? gs_jval_parse(text, len) : NULL;
        SDL_free(text);
        if (stored && gs_jval_kind(stored) == GS_JSON_OBJECT) {
            static const char *const sections[] = { "stt", "cleanup" };
            for (int i = 0; i < 2; i++) {
                gs_jval *section = gs_jval_get(stored, sections[i]);
                if (section && gs_jval_kind(section) == GS_JSON_OBJECT) gs_jval_remove(section, "apiKeyEncrypted");
            }
            imported = true;
        } else {
            gs_jval_free(stored);
            stored = NULL;
        }
    }
    gs_jval *empty = gs_jval_object();
    S.current = eh_settings_resolve(stored ? stored : empty);
    gs_jval_free(empty);
    gs_jval_free(stored);
    S.first_run = !exists && !imported;
    if (imported) {
        char error[256];
        if (!eh_settings_save(gs_jval_clone(S.current), error, sizeof error)) SDL_Log("the imported settings could not be saved: %s", error);
        else SDL_Log("took over the settings of the Electron build from %s", import_from);
    }
}

bool eh_settings_first_run(void) { return S.first_run; }
const gs_jval *eh_settings(void) { return S.current; }
const char *eh_setting_str(const char *path) { return gs_jval_string(gs_jval_path(S.current, path), ""); }
double eh_setting_num(const char *path) { return gs_jval_number(gs_jval_path(S.current, path), 0); }
bool eh_setting_bool(const char *path) { return gs_jval_boolean(gs_jval_path(S.current, path), false); }

void eh_settings_on_changed(eh_settings_listener *fn, void *user) {
    if (S.listener_count < (int)SDL_arraysize(S.listeners)) {
        S.listeners[S.listener_count].fn = fn;
        S.listeners[S.listener_count++].user = user;
    }
}

bool eh_settings_save(gs_jval *next, char *error, size_t error_size) {
    gs_jval *merged = merged_over_defaults(next);
    gs_jval_free(next);
    char *text = eh_json_text(merged, true);
    bool ok = eh_write_file_atomic(S.path, text, SDL_strlen(text), error, error_size);
    SDL_free(text);
    if (!ok) { gs_jval_free(merged); return false; }
    gs_jval *previous = S.current;
    S.current = merged;
    S.first_run = false;
    for (int i = 0; i < S.listener_count; i++) S.listeners[i].fn(S.listeners[i].user, previous, S.current);
    gs_jval_free(previous);
    return true;
}

bool eh_setting_set(const char *path, gs_jval *value, char *error, size_t error_size) {
    gs_jval *next = gs_jval_clone(S.current);
    gs_jval_set_path(next, path, value);
    return eh_settings_save(next, error, error_size);
}

/* ---------------- cleanup ---------------- */

eh_cleanup_plan eh_resolve_cleanup(const gs_jval *cleanup) {
    eh_cleanup_plan plan = { .sampling = { .temperature = -1, .top_p = -1 } };
    // The base prompt, then the user's preferred terms, which apply in every style.
    size_t cap = 4096, n = 0;
    char *prompt = SDL_malloc(cap);
#define ADD(text) do { const char *t_ = (text); size_t l_ = SDL_strlen(t_); if (n + l_ + 1 > cap) prompt = SDL_realloc(prompt, cap = (cap + l_) * 2); SDL_memcpy(prompt + n, t_, l_); n += l_; prompt[n] = 0; } while (0)
    prompt[0] = 0;
    ADD(gs_jval_string(gs_jval_get(cleanup, "systemPrompt"), ""));
    gs_jval *terms = gs_jval_get(cleanup, "dictionary");
    bool any = false;
    for (int i = 0; i < gs_jval_count(terms); i++) {
        const gs_jval *item = gs_jval_at(terms, i);
        char number[40];
        const char *raw = gs_jval_kind(item) == GS_JSON_STRING ? gs_jval_string(item, "") : NULL;
        if (gs_jval_kind(item) == GS_JSON_NUMBER) { SDL_snprintf(number, sizeof number, "%.15g", gs_jval_number(item, 0)); raw = number; }
        if (gs_jval_kind(item) == GS_JSON_BOOL) raw = gs_jval_boolean(item, false) ? "true" : "false";
        char *term = eh_trim(raw ? raw : "");
        if (*term) {
            if (!any)
                ADD("\n\nPreferred vocabulary: the speaker uses these exact terms and spellings. When a word or phrase in the transcript is a "
                    "mishearing, misspelling or wrong casing of one of them, replace it with the term exactly as written below. Never force a "
                    "term in where the speaker clearly said something else.");
            any = true;
            ADD("\n- ");
            ADD(term);
        }
        SDL_free(term);
    }
    const char *style = gs_jval_string(gs_jval_get(cleanup, "style"), "");
    if (!SDL_strcmp(style, "custom")) {
        const gs_jval *custom = gs_jval_get(cleanup, "custom");
        plan.has_temperature = gs_jval_kind(gs_jval_get(custom, "temperature")) == GS_JSON_NUMBER;
        plan.has_top_p = gs_jval_kind(gs_jval_get(custom, "topP")) == GS_JSON_NUMBER;
        plan.sampling.temperature = (float)gs_jval_number(gs_jval_get(custom, "temperature"), -1);
        plan.sampling.top_p = (float)gs_jval_number(gs_jval_get(custom, "topP"), -1);
        plan.sampling.top_k = (int)gs_jval_number(gs_jval_get(custom, "topK"), 0);
        plan.sampling.min_p = (float)gs_jval_number(gs_jval_get(custom, "minP"), 0);
    } else {
        // An unknown style is the default one.
        size_t pick = 0, fallback = 0;
        bool found = false;
        for (size_t i = 0; i < SDL_arraysize(eh_styles); i++) {
            if (!SDL_strcmp(eh_styles[i].id, EH_DEFAULT_STYLE)) fallback = i;
            if (!found && !SDL_strcmp(eh_styles[i].id, style)) { pick = i; found = true; }
        }
        if (!found) pick = fallback;
        ADD("\n\nEditing style: ");
        ADD(eh_styles[pick].directive);
        plan.has_temperature = plan.has_top_p = true;
        plan.sampling.temperature = (float)eh_styles[pick].temperature;
        plan.sampling.top_p = (float)eh_styles[pick].top_p;
        plan.sampling.top_k = eh_styles[pick].top_k;
        plan.sampling.min_p = (float)eh_styles[pick].min_p;
    }
#undef ADD
    plan.system_prompt = prompt;
    return plan;
}
