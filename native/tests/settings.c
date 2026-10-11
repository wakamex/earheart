// Settings against main/settings.js and main/cleanup-styles.js (tests/vectors/settings.json), and
// the file itself: saving, reading back, a file that is not JSON, taking over the Electron build's.
#include <SDL3/SDL.h>
#include <string.h>
#include "files.h"
#include "settings.h"
#include "test.h"

static int heard;
static void listener(void *user, const eh_jv *previous, const eh_jv *current) {
    if (heard++) return;  // later saves in this test change other things
    CHECK(!strcmp(eh_jv_string(eh_jv_get(previous, "hotkey"), ""), "CommandOrControl+Shift+Space"));
    CHECK(!strcmp(eh_jv_string(eh_jv_get(current, "hotkey"), ""), "Alt+D"));
}

void test_settings(const char *vectors) {
    size_t len;
    char *text = SDL_LoadFile(vectors, &len);
    eh_jv *doc = text ? eh_jv_parse(text, len) : NULL;
    CHECK(doc);
    eh_jv *stored = eh_jv_get(doc, "stored"), *cleanups = eh_jv_get(doc, "cleanups");
    for (int i = 0; i < eh_jv_count(stored); i++) {
        eh_jv *got = eh_settings_resolve(eh_jv_get(eh_jv_at(stored, i), "input"));
        const eh_jv *want = eh_jv_get(eh_jv_at(stored, i), "merged");
        if (!eh_jv_equal(got, want) || !eh_jv_equal(want, got)) {
            char *a = eh_jv_write(got, false), *b = eh_jv_write(want, false);
            printf("  stored file %d differs\n    want: %.600s\n    got:  %.600s\n", i, b, a);
            failures++;
            SDL_free(a);
            SDL_free(b);
        }
        eh_jv_free(got);
    }
    for (int i = 0; i < eh_jv_count(cleanups); i++) {
        const eh_jv *c = eh_jv_at(cleanups, i), *sampling = eh_jv_get(c, "sampling");
        eh_cleanup_plan plan = eh_resolve_cleanup(eh_jv_get(c, "input"));
        CHECK(!strcmp(plan.system_prompt, eh_jv_string(eh_jv_get(c, "systemPrompt"), "?")));
        CHECK(plan.has_temperature == (eh_jv_get(sampling, "temperature") != NULL));
        if (plan.has_temperature) CHECK(plan.sampling.temperature == (float)eh_jv_number(eh_jv_get(sampling, "temperature"), -9));
        if (plan.has_top_p) CHECK(plan.sampling.top_p == (float)eh_jv_number(eh_jv_get(sampling, "topP"), -9));
        CHECK(plan.sampling.top_k == (int)eh_jv_number(eh_jv_get(sampling, "topK"), 0));
        CHECK(plan.sampling.min_p == (float)eh_jv_number(eh_jv_get(sampling, "minP"), 0));
        SDL_free(plan.system_prompt);
    }
    printf("  %d stored files and %d cleanup sections compared\n", eh_jv_count(stored), eh_jv_count(cleanups));
    eh_jv_free(doc);
    SDL_free(text);

    // The file. A first run has none; a save writes it whole and tells the listeners.
    char dir[512], path[600], other[600], error[256];
    SDL_snprintf(dir, sizeof dir, "%searheart-test-%llu", SDL_GetPrefPath(NULL, "earheart-tests"), (unsigned long long)SDL_GetTicksNS());
    SDL_snprintf(path, sizeof path, "%s/settings.json", dir);
    eh_settings_init(path, NULL);
    CHECK(eh_settings_first_run());
    CHECK(!strcmp(eh_setting_str("stt.builtin.model"), "parakeet-tdt-0.6b-v3") && eh_setting_num("history.limit") == 100 && eh_setting_bool("cleanup.enabled"));
    eh_settings_on_changed(listener, NULL);
    CHECK(eh_setting_set("hotkey", eh_jv_str("Alt+D"), error, sizeof error) && heard == 1 && !eh_settings_first_run());
    eh_settings_init(path, NULL);
    CHECK(!eh_settings_first_run() && !strcmp(eh_setting_str("hotkey"), "Alt+D"));

    // A file that is not a JSON object is moved aside, and the defaults stand.
    CHECK(eh_write_file_atomic(path, "[1, 2", 6, error, sizeof error));
    eh_settings_init(path, NULL);
    CHECK(!strcmp(eh_setting_str("hotkey"), "CommandOrControl+Shift+Space") && !SDL_GetPathInfo(path, NULL));
    int count = 0;
    char **left = SDL_GlobDirectory(dir, "settings.json.corrupt-*", 0, &count);
    CHECK(count == 1);
    SDL_free(left);

    // The Electron build's settings are taken over once, without the keys only it can read.
    SDL_snprintf(other, sizeof other, "%s/electron.json", dir);
    const char *theirs = "{\"hotkey\":\"Alt+E\",\"stt\":{\"engine\":\"remote\",\"apiKeyEncrypted\":\"djEw\",\"baseUrl\":\"http://h/v1\"}}";
    CHECK(eh_write_file_atomic(other, theirs, strlen(theirs), error, sizeof error));
    eh_settings_init(path, other);
    CHECK(!eh_settings_first_run() && !strcmp(eh_setting_str("hotkey"), "Alt+E") && !strcmp(eh_setting_str("stt.baseUrl"), "http://h/v1"));
    CHECK(!eh_jv_path(eh_settings(), "stt.apiKeyEncrypted") && SDL_GetPathInfo(path, NULL));

    char **files = SDL_GlobDirectory(dir, "*", 0, &count);
    for (int i = 0; i < count; i++) { SDL_snprintf(other, sizeof other, "%s/%s", dir, files[i]); SDL_RemovePath(other); }
    SDL_free(files);
    SDL_RemovePath(dir);
}
