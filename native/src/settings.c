#include "settings.h"
#include <string.h>
#include <SDL3/SDL.h>
#include <math.h>
#include "defaults.h"
#include "files.h"
#include "text.h"

static struct {
    char *path;
    eh_jv *defaults, *current;
    bool first_run;
    struct { eh_settings_listener *fn; void *user; } listeners[8];
    int listener_count;
} S;

static eh_jv *defaults(void) {
    if (!S.defaults) S.defaults = eh_jv_parse(eh_defaults_json, sizeof eh_defaults_json - 1);
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

static void migrate(eh_jv *stored) {
    eh_jv *stt = eh_jv_get(stored, "stt"), *cleanup = eh_jv_get(stored, "cleanup");
    if (stt && stt->type == EH_JV_OBJECT) {
        // A section saved before there was a built-in engine meant the remote one.
        const char *engine = eh_jv_string(eh_jv_get(stt, "engine"), NULL);
        if (!eh_jv_get(stt, "engine") || (engine && !SDL_strcmp(engine, "server"))) eh_jv_set(stt, "engine", eh_jv_str("remote"));
        if (eh_jv_number(eh_jv_path(stt, "livePreview.chunkSeconds"), 0) == 5) eh_jv_set_path(stt, "livePreview.chunkSeconds", eh_jv_num(10));
    }
    eh_jv_remove(stored, "sttServer");
    if (cleanup && cleanup->type == EH_JV_OBJECT) {
        if (!eh_jv_get(cleanup, "engine")) eh_jv_set(cleanup, "engine", eh_jv_str("remote"));
        if (eh_jv_number(eh_jv_get(cleanup, "timeoutMs"), 0) == 60000) eh_jv_set(cleanup, "timeoutMs", eh_jv_num(120000));
        const char *prompt = eh_jv_string(eh_jv_get(cleanup, "systemPrompt"), NULL), *current = eh_jv_string(eh_jv_path(defaults(), "cleanup.systemPrompt"), "");
        if (prompt) {
            char *unwrapped = unwrap_prompt(prompt);
            if (!SDL_strcmp(unwrapped, current)) eh_jv_set(cleanup, "systemPrompt", eh_jv_str(current));
            SDL_free(unwrapped);
        }
        // A bare temperature from before styles existed becomes the custom style with nothing else filtering.
        eh_jv *temperature = eh_jv_get(cleanup, "temperature");
        if (!eh_jv_get(cleanup, "style") && temperature) {
            eh_jv *custom = eh_jv_object();
            eh_jv_set(custom, "temperature", eh_jv_clone(temperature));
            eh_jv_set(custom, "topP", eh_jv_num(EH_NEUTRAL_TOP_P));
            eh_jv_set(custom, "topK", eh_jv_num(EH_NEUTRAL_TOP_K));
            eh_jv_set(custom, "minP", eh_jv_num(EH_NEUTRAL_MIN_P));
            eh_jv_set(cleanup, "style", eh_jv_str("custom"));
            eh_jv_set(cleanup, "custom", custom);
            eh_jv_remove(cleanup, "temperature");
        }
    }
}

static void clamp(eh_jv *merged, const char *path, double min, double max) {
    eh_jv *v = eh_jv_path(merged, path);
    double n = eh_jv_number(v, NAN);
    eh_jv_set_path(merged, path, isfinite(n) ? eh_jv_num(floor(SDL_min(max, SDL_max(min, n)) + 0.5)) : eh_jv_clone(eh_jv_path(defaults(), path)));
}

static eh_jv *merged_over_defaults(const eh_jv *next) {
    eh_jv *merged = eh_jv_merge(defaults(), next);
    clamp(merged, "audio.maxRecordingSeconds", 10, 3600);
    clamp(merged, "engines.idleUnloadMinutes", 0, 240);
    return merged;
}

eh_jv *eh_settings_resolve(const eh_jv *stored) {
    eh_jv *copy = eh_jv_clone(stored);
    if (copy->type == EH_JV_OBJECT) migrate(copy);
    eh_jv *merged = merged_over_defaults(copy);
    eh_jv_free(copy);
    return merged;
}

/* ---------------- the file ---------------- */

// The file's content as an object, or NULL. A file that is not a JSON object is moved aside, so
// the next save cannot overwrite what the user had.
static eh_jv *read_stored(const char *path) {
    size_t len;
    char *text = SDL_LoadFile(path, &len);
    if (!text) return NULL;
    eh_jv *parsed = eh_jv_parse(text, len);
    SDL_free(text);
    if (parsed && parsed->type == EH_JV_OBJECT) return parsed;
    eh_jv_free(parsed);
    char backup[1200];
    SDL_snprintf(backup, sizeof backup, "%s.corrupt-%lld", path, (long long)SDL_GetTicksNS());
    if (SDL_RenamePath(path, backup)) SDL_Log("settings file was not valid JSON; preserved it at %s and started from defaults", backup);
    else SDL_Log("settings file was not valid JSON and could not be preserved: %s", SDL_GetError());
    return NULL;
}

void eh_settings_init(const char *path, const char *import_from) {
    SDL_free(S.path);
    eh_jv_free(S.current);
    S.path = SDL_strdup(path);
    bool exists = SDL_GetPathInfo(path, NULL);
    eh_jv *stored = exists ? read_stored(path) : NULL;
    bool imported = false;
    if (!exists && import_from && SDL_GetPathInfo(import_from, NULL)) {
        // The Electron build's settings. Its API keys are encrypted for that program alone and
        // cannot be read here; they are entered again.
        size_t len;
        char *text = SDL_LoadFile(import_from, &len);
        stored = text ? eh_jv_parse(text, len) : NULL;
        SDL_free(text);
        if (stored && stored->type == EH_JV_OBJECT) {
            static const char *const sections[] = { "stt", "cleanup" };
            for (int i = 0; i < 2; i++) {
                eh_jv *section = eh_jv_get(stored, sections[i]);
                if (section && section->type == EH_JV_OBJECT) eh_jv_remove(section, "apiKeyEncrypted");
            }
            imported = true;
        } else {
            eh_jv_free(stored);
            stored = NULL;
        }
    }
    eh_jv *empty = eh_jv_object();
    S.current = eh_settings_resolve(stored ? stored : empty);
    eh_jv_free(empty);
    eh_jv_free(stored);
    S.first_run = !exists && !imported;
    if (imported) {
        char error[256];
        if (!eh_settings_save(eh_jv_clone(S.current), error, sizeof error)) SDL_Log("the imported settings could not be saved: %s", error);
        else SDL_Log("took over the settings of the Electron build from %s", import_from);
    }
}

bool eh_settings_first_run(void) { return S.first_run; }
const eh_jv *eh_settings(void) { return S.current; }
const char *eh_setting_str(const char *path) { return eh_jv_string(eh_jv_path(S.current, path), ""); }
double eh_setting_num(const char *path) { return eh_jv_number(eh_jv_path(S.current, path), 0); }
bool eh_setting_bool(const char *path) { return eh_jv_boolean(eh_jv_path(S.current, path), false); }

void eh_settings_on_changed(eh_settings_listener *fn, void *user) {
    if (S.listener_count < (int)SDL_arraysize(S.listeners)) {
        S.listeners[S.listener_count].fn = fn;
        S.listeners[S.listener_count++].user = user;
    }
}

bool eh_settings_save(eh_jv *next, char *error, size_t error_size) {
    eh_jv *merged = merged_over_defaults(next);
    eh_jv_free(next);
    char *text = eh_jv_write(merged, true);
    bool ok = eh_write_file_atomic(S.path, text, SDL_strlen(text), error, error_size);
    SDL_free(text);
    if (!ok) { eh_jv_free(merged); return false; }
    eh_jv *previous = S.current;
    S.current = merged;
    S.first_run = false;
    for (int i = 0; i < S.listener_count; i++) S.listeners[i].fn(S.listeners[i].user, previous, S.current);
    eh_jv_free(previous);
    return true;
}

bool eh_setting_set(const char *path, eh_jv *value, char *error, size_t error_size) {
    eh_jv *next = eh_jv_clone(S.current);
    eh_jv_set_path(next, path, value);
    return eh_settings_save(next, error, error_size);
}

/* ---------------- cleanup ---------------- */

eh_cleanup_plan eh_resolve_cleanup(const eh_jv *cleanup) {
    eh_cleanup_plan plan = { .sampling = { .temperature = -1, .top_p = -1 } };
    // The base prompt, then the user's preferred terms, which apply in every style.
    size_t cap = 4096, n = 0;
    char *prompt = SDL_malloc(cap);
#define ADD(text) do { const char *t_ = (text); size_t l_ = SDL_strlen(t_); if (n + l_ + 1 > cap) prompt = SDL_realloc(prompt, cap = (cap + l_) * 2); SDL_memcpy(prompt + n, t_, l_); n += l_; prompt[n] = 0; } while (0)
    prompt[0] = 0;
    ADD(eh_jv_string(eh_jv_get(cleanup, "systemPrompt"), ""));
    eh_jv *terms = eh_jv_get(cleanup, "dictionary");
    bool any = false;
    for (int i = 0; i < eh_jv_count(terms); i++) {
        const eh_jv *item = eh_jv_at(terms, i);
        char number[40];
        const char *raw = item->type == EH_JV_STRING ? item->string : NULL;
        if (item->type == EH_JV_NUMBER) { SDL_snprintf(number, sizeof number, "%.15g", item->number); raw = number; }
        if (item->type == EH_JV_BOOL) raw = item->boolean ? "true" : "false";
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
    const char *style = eh_jv_string(eh_jv_get(cleanup, "style"), "");
    if (!SDL_strcmp(style, "custom")) {
        const eh_jv *custom = eh_jv_get(cleanup, "custom");
        plan.has_temperature = eh_jv_get(custom, "temperature") && eh_jv_get(custom, "temperature")->type == EH_JV_NUMBER;
        plan.has_top_p = eh_jv_get(custom, "topP") && eh_jv_get(custom, "topP")->type == EH_JV_NUMBER;
        plan.sampling.temperature = (float)eh_jv_number(eh_jv_get(custom, "temperature"), -1);
        plan.sampling.top_p = (float)eh_jv_number(eh_jv_get(custom, "topP"), -1);
        plan.sampling.top_k = (int)eh_jv_number(eh_jv_get(custom, "topK"), 0);
        plan.sampling.min_p = (float)eh_jv_number(eh_jv_get(custom, "minP"), 0);
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
