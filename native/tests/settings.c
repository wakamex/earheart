// Settings against main/settings.js and main/cleanup-styles.js (tests/vectors/settings.json), and
// the file itself: saving, reading back, a file that is not JSON, taking over the Electron build's.
#include <SDL3/SDL.h>
#include <string.h>
#include "files.h"
#include "settings.h"
#include "test.h"

static int heard;
static void listener(void *user, const gs_jval *previous, const gs_jval *current) {
    if (heard++) return;  // later saves in this test change other things
    CHECK(!strcmp(gs_jval_string(gs_jval_get(previous, "hotkey"), ""), "CommandOrControl+Shift+Space"));
    CHECK(!strcmp(gs_jval_string(gs_jval_get(current, "hotkey"), ""), "Alt+D"));
}

void test_settings(const char *vectors) {
    size_t len;
    char *text = SDL_LoadFile(vectors, &len);
    gs_jval *doc = text ? gs_jval_parse(text, len) : NULL;
    CHECK(doc);
    gs_jval *stored = gs_jval_get(doc, "stored"), *cleanups = gs_jval_get(doc, "cleanups");
    for (int i = 0; i < gs_jval_count(stored); i++) {
        gs_jval *got = eh_settings_resolve(gs_jval_get(gs_jval_at(stored, i), "input"));
        const gs_jval *want = gs_jval_get(gs_jval_at(stored, i), "merged");
        if (!gs_jval_equal(got, want) || !gs_jval_equal(want, got)) {
            char *a = gs_jval_write(got, false), *b = gs_jval_write(want, false);
            printf("  stored file %d differs\n    want: %.600s\n    got:  %.600s\n", i, b, a);
            failures++;
            SDL_free(a);
            SDL_free(b);
        }
        gs_jval_free(got);
    }
    for (int i = 0; i < gs_jval_count(cleanups); i++) {
        const gs_jval *c = gs_jval_at(cleanups, i), *sampling = gs_jval_get(c, "sampling");
        eh_cleanup_plan plan = eh_resolve_cleanup(gs_jval_get(c, "input"));
        CHECK(!strcmp(plan.system_prompt, gs_jval_string(gs_jval_get(c, "systemPrompt"), "?")));
        CHECK(plan.has_temperature == (gs_jval_get(sampling, "temperature") != NULL));
        if (plan.has_temperature) CHECK(plan.sampling.temperature == (float)gs_jval_number(gs_jval_get(sampling, "temperature"), -9));
        if (plan.has_top_p) CHECK(plan.sampling.top_p == (float)gs_jval_number(gs_jval_get(sampling, "topP"), -9));
        CHECK(plan.sampling.top_k == (int)gs_jval_number(gs_jval_get(sampling, "topK"), 0));
        CHECK(plan.sampling.min_p == (float)gs_jval_number(gs_jval_get(sampling, "minP"), 0));
        SDL_free(plan.system_prompt);
    }
    printf("  %d stored files and %d cleanup sections compared\n", gs_jval_count(stored), gs_jval_count(cleanups));
    gs_jval_free(doc);
    SDL_free(text);

    // The file. A first run has none; a save writes it whole and tells the listeners.
    char dir[512], path[600], other[600], error[256];
    SDL_snprintf(dir, sizeof dir, "%searheart-test-%llu", SDL_GetPrefPath(NULL, "earheart-tests"), (unsigned long long)SDL_GetTicksNS());
    SDL_snprintf(path, sizeof path, "%s/settings.json", dir);
    eh_settings_init(path, NULL);
    CHECK(eh_settings_first_run());
    CHECK(!strcmp(eh_setting_str("stt.builtin.model"), "parakeet-tdt-0.6b-v3") && eh_setting_num("history.limit") == 100 && eh_setting_bool("cleanup.enabled"));
    eh_settings_on_changed(listener, NULL);
    CHECK(eh_setting_set("hotkey", gs_jval_str("Alt+D"), error, sizeof error) && heard == 1 && !eh_settings_first_run());
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
    CHECK(!gs_jval_path(eh_settings(), "stt.apiKeyEncrypted") && SDL_GetPathInfo(path, NULL));

    char **files = SDL_GlobDirectory(dir, "*", 0, &count);
    for (int i = 0; i < count; i++) { SDL_snprintf(other, sizeof other, "%s/%s", dir, files[i]); SDL_RemovePath(other); }
    SDL_free(files);
    SDL_RemovePath(dir);
}
