// `earheart settings-smoke [SETTINGS_VECTORS]`: the settings window driven as a user drives it,
// with mouse, wheel and keyboard events given to the real window, and what it then shows and
// saves checked. The checks are the settings window's own from scripts/settings-smoke.js. Needs a
// display. Exits 1 when a check fails.
#include <SDL3/SDL.h>
#include <stdio.h>
#include "app.h"
#include "deliver.h"
#include "hotkeys.h"
#include "jv.h"
#include "loop.h"
#include "paths.h"
#include "settings.h"
#include "settings_window.h"
#include "ui.h"

static int failed, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } } while (0)
#define IS(what, expected) do { checks++; const char *got_ = eh_settings_window_text(what); if (SDL_strcmp(got_, expected)) { printf("  FAILED %s:%d: %s is \"%s\", expected \"%s\"\n", __FILE__, __LINE__, what, got_, expected); failed++; } } while (0)

static eh_ui *ui(void) { return (eh_ui *)eh_settings_window_ui(); }

static void frames(int n) {
    for (int i = 0; i < n && eh_settings_window_is_open(); i++) eh_settings_window_frame();
}

static SDL_WindowID window(void) { return SDL_GetWindowID(eh_ui_window(ui())); }

static void pointer(Uint32 type, float x, float y) {
    SDL_Event e = { 0 };
    if (type == SDL_EVENT_MOUSE_MOTION) e.motion = (SDL_MouseMotionEvent){ .type = type, .windowID = window(), .x = x, .y = y };
    else e.button = (SDL_MouseButtonEvent){ .type = type, .windowID = window(), .button = SDL_BUTTON_LEFT, .down = type == SDL_EVENT_MOUSE_BUTTON_DOWN, .x = x, .y = y };
    eh_settings_window_event(&e);
}

static void wheel(float x, float y, float amount) {
    pointer(SDL_EVENT_MOUSE_MOTION, x, y);
    SDL_Event e = { 0 };
    e.wheel = (SDL_MouseWheelEvent){ .type = SDL_EVENT_MOUSE_WHEEL, .windowID = window(), .y = amount, .mouse_x = x, .mouse_y = y };
    eh_settings_window_event(&e);
    frames(2);
}

static void key(SDL_Scancode sc, SDL_Keycode k, SDL_Keymod mod) {
    SDL_Event e = { 0 };
    e.key = (SDL_KeyboardEvent){ .type = SDL_EVENT_KEY_DOWN, .windowID = window(), .scancode = sc, .key = k, .mod = mod, .down = true };
    eh_settings_window_event(&e);
    frames(2);
}

static void type_text(const char *text) {
    SDL_Event e = { 0 };
    e.text = (SDL_TextInputEvent){ .type = SDL_EVENT_TEXT_INPUT, .windowID = window(), .text = text };
    eh_settings_window_event(&e);
    frames(2);
}

// Scrolls the page until the control is whole inside it. False when the page has no such control.
static bool reveal(const char *id, gs_rect *r) {
    int w, h;
    if (!eh_settings_window_is_open()) return false;
    SDL_GetWindowSize(eh_ui_window(ui()), &w, &h);
    for (int tries = 0; tries < 200; tries++) {
        frames(1);
        if (!eh_ui_stop_rect(ui(), id, r)) return false;
        if (!SDL_strncmp(id, "tab-", 4) || !SDL_strcmp(id, "save") || !SDL_strncmp(id, "ask-", 4)) return true;  // the index, the footer, a question: not on the page
        if (r->y >= 80 && r->y + r->h <= (float)h - 72) return true;
        wheel((float)w - 200, (float)h / 2, r->y < 80 ? 1 : -1);
    }
    return false;
}

static bool click(const char *id) {
    gs_rect r;
    if (!reveal(id, &r)) { printf("  no control \"%s\" to click\n", id); return false; }
    float x = r.x + SDL_min(r.w / 2, 60), y = r.y + r.h / 2;
    pointer(SDL_EVENT_MOUSE_MOTION, x, y);
    frames(1);
    pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
    frames(1);
    pointer(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
    frames(3);
    if (SDL_getenv("EARHEART_SMOKE_VERBOSE")) printf("  clicked %s at %.0f,%.0f (rect %.0f,%.0f %.0fx%.0f); focus now \"%s\"\n", id, x, y, r.x, r.y, r.w, r.h, eh_settings_window_is_open() ? eh_ui_focus_name(ui()) : "(closed)");
    return true;
}

static bool has(const char *id) {
    gs_rect r;
    frames(1);
    return eh_settings_window_is_open() && eh_ui_stop_rect(ui(), id, &r);
}

// Frames until the window says `what` is no longer `busy`, or five seconds pass.
static void settle(const char *what, const char *busy) {
    for (Uint64 until = SDL_GetTicks() + 5000; SDL_GetTicks() < until && eh_settings_window_is_open();) {
        frames(1);
        if (SDL_strcmp(eh_settings_window_text(what), busy)) return;
        SDL_Delay(10);
    }
}

// A stand-in for the desktop's key grabs: one key is another app's.
#define TAKEN "CommandOrControl+Alt+T"
static int fake_add(const char *accelerator, int name, char *error, size_t size) { return SDL_strcmp(accelerator, TAKEN) ? 0 : 1; }
static void fake_remove(const char *accelerator) {}

static double setting(const char *path) {
    eh_lock();
    double v = eh_setting_num(path);
    eh_unlock();
    return v;
}

static char *setting_text(const char *path) {
    eh_lock();
    char *v = SDL_strdup(eh_setting_str(path));
    eh_unlock();
    return v;
}

int eh_cmd_settings_smoke(int argc, char **argv) {
    char data[1200];
    setvbuf(stdout, NULL, _IOLBF, 0);
    SDL_snprintf(data, sizeof data, "%s" EH_SEP "settings-smoke-%llu", eh_data_dir(), (unsigned long long)SDL_GetTicksNS());
    SDL_CreateDirectory(data);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "earheart: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    eh_app_init(data, NULL, eh_deliver);
    static const eh_hotkey_backend fake = { fake_add, fake_remove };
    eh_lock();
    eh_hotkeys_backend(&fake);
    eh_hotkeys_apply(eh_settings(), NULL);
    eh_unlock();

    // The window opens on General with its six sections in the index, and an untouched draft is
    // the settings it was filled from.
    eh_settings_window_open(NULL);
    if (!eh_settings_window_is_open()) { printf("no window: %s\n", SDL_GetError()); return 1; }
    frames(4);
    IS("section", "general");
    static const char *const tabs[6] = { "tab-general", "tab-live", "tab-stt", "tab-cleanup", "tab-history", "tab-advanced" };
    for (int i = 0; i < 6; i++) CHECK(has(tabs[i]));
    CHECK(eh_settings_window_untouched());

    // Every control is a Tab stop, in the page's order, and Tab from the last comes back round.
    int stops = eh_ui_stops(ui());
    CHECK(stops >= 30);
    char first[56];
    key(SDL_SCANCODE_TAB, SDLK_TAB, 0);
    SDL_strlcpy(first, eh_ui_focus_name(ui()), sizeof first);
    CHECK(first[0]);
    bool saw_save = false, saw_prompt = false;
    for (int i = 1; i < stops; i++) {
        key(SDL_SCANCODE_TAB, SDLK_TAB, 0);
        saw_save = saw_save || !SDL_strcmp(eh_ui_focus_name(ui()), "save");
        saw_prompt = saw_prompt || !SDL_strcmp(eh_ui_focus_name(ui()), "cleanup-prompt");
    }
    CHECK(saw_save && saw_prompt);
    key(SDL_SCANCODE_TAB, SDLK_TAB, 0);
    CHECK(!SDL_strcmp(eh_ui_focus_name(ui()), first));
    key(SDL_SCANCODE_TAB, SDLK_TAB, SDL_KMOD_LSHIFT);
    key(SDL_SCANCODE_TAB, SDLK_TAB, 0);
    CHECK(!SDL_strcmp(eh_ui_focus_name(ui()), first));
    // A control reached by Tab is scrolled into the page.
    gs_rect r;
    int w, h;
    SDL_GetWindowSize(eh_ui_window(ui()), &w, &h);
    for (int i = 0; i < stops && SDL_strcmp(eh_ui_focus_name(ui()), "max-seconds"); i++) key(SDL_SCANCODE_TAB, SDLK_TAB, 0);
    frames(2);
    CHECK(eh_ui_stop_rect(ui(), "max-seconds", &r) && r.y >= 72 && r.y + r.h <= (float)h - 64);

    // The index: a click glides to the section and lights its entry; the wheel moves the light.
    CHECK(click("tab-stt"));
    for (int i = 0; i < 40; i++) frames(1), SDL_Delay(5);
    IS("section", "stt");
    CHECK(click("tab-general"));
    for (int i = 0; i < 40; i++) frames(1), SDL_Delay(5);
    IS("section", "general");
    for (int i = 0; i < 60; i++) wheel((float)w - 200, (float)h / 2, -3);
    IS("section", "advanced");
    for (int i = 0; i < 60; i++) wheel((float)w - 200, (float)h / 2, 3);
    IS("section", "general");

    // A hotkey field: a click arms it, Escape lets go with nothing changed, a combination fills it,
    // a key without a modifier does not, and Clear empties it.
    CHECK(click("pause-hotkey") && eh_ui_capturing(ui()));
    key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0);
    CHECK(!eh_ui_capturing(ui()) && eh_settings_window_is_open());
    IS("pause-hotkey", "");
    CHECK(click("pause-hotkey"));
    key(SDL_SCANCODE_K, SDLK_K, 0);
    CHECK(eh_ui_capturing(ui()));
    key(SDL_SCANCODE_LCTRL, SDLK_LCTRL, SDL_KMOD_LCTRL);
    CHECK(eh_ui_capturing(ui()));
    key(SDL_SCANCODE_K, SDLK_K, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    CHECK(!eh_ui_capturing(ui()));
#ifdef __APPLE__
    IS("pause-hotkey", "Control+Alt+K");
#else
    IS("pause-hotkey", "CommandOrControl+Alt+K");
#endif
    CHECK(!eh_settings_window_untouched());
    CHECK(click("pause-hotkey-clear"));
    IS("pause-hotkey", "");
    CHECK(eh_settings_window_untouched());
    // By keyboard: Enter arms the focused field.
    for (int i = 0; i < stops && SDL_strcmp(eh_ui_focus_name(ui()), "discard-hotkey"); i++) key(SDL_SCANCODE_TAB, SDLK_TAB, 0);
    key(SDL_SCANCODE_RETURN, SDLK_RETURN, 0);
    CHECK(eh_ui_capturing(ui()));
    key(SDL_SCANCODE_F9, SDLK_F9, SDL_KMOD_LSHIFT);
    IS("discard-hotkey", "Shift+F9");
    CHECK(click("discard-hotkey-clear"));

    // Cleanup turned off leaves its fields shown and out of reach; on again brings them back.
    CHECK(has("cleanup-prompt"));
    CHECK(click("cleanup-enabled"));
    CHECK(!has("cleanup-prompt") && !has("cleanup-builtin-model") && has("cleanup-enabled"));
    CHECK(click("cleanup-enabled"));
    CHECK(has("cleanup-prompt") && eh_settings_window_untouched());

    // The engine choice swaps the built-in model for the service's fields, and back.
    CHECK(has("stt-builtin-model") && !has("stt-url"));
    CHECK(click("stt-engine-external"));
    CHECK(!has("stt-builtin-model") && has("stt-url") && has("stt-key") && has("stt-model") && has("stt-language") && has("stt-test") && has("stt-fetch-models"));
    // A service that is not there: the test says so, and names no key.
    CHECK(click("stt-url"));
    key(SDL_SCANCODE_A, SDLK_A, SDL_KMOD_LCTRL);
    type_text("http://127.0.0.1:9/v1");
    CHECK(click("stt-test"));
    settle("stt-test", "Testing…");
    CHECK(SDL_strstr(eh_settings_window_text("stt-test"), "127.0.0.1:9") != NULL);
    CHECK(click("stt-fetch-models"));
    settle("stt-fetch", "Fetching…");
    CHECK(eh_settings_window_text("stt-fetch")[0] && SDL_strcmp(eh_settings_window_text("stt-fetch"), "Fetching…"));
    CHECK(click("stt-engine-builtin"));
    CHECK(has("stt-builtin-model"));
    // The style: custom values in place of the slider.
    CHECK(has("cleanup-style") && !has("cleanup-temperature"));
    CHECK(click("cleanup-style-mode-custom"));
    CHECK(!has("cleanup-style") && has("cleanup-temperature") && has("cleanup-top-p") && has("cleanup-top-k") && has("cleanup-min-p") && has("cleanup-custom-from-preset"));
    CHECK(click("cleanup-style-mode-preset"));

    // A number outside its limits is saved as the nearest limit, the window says so and stays open.
    CHECK(click("max-seconds"));
    key(SDL_SCANCODE_A, SDLK_A, SDL_KMOD_LCTRL);
    type_text("99999");
    IS("max-seconds", "99999");
    CHECK(click("save"));
    settle("save", "Saving…");
    IS("save", "Saved — Max dictation length set to 3600 s (allowed 10–3600)");
    CHECK(eh_settings_window_is_open() && setting("audio.maxRecordingSeconds") == 3600);
    IS("max-seconds", "3600");

    // A hotkey another app holds: everything else is saved, the hotkey keeps its saved value, the
    // field keeps what was typed with the reason under it, and the window stays open.
    CHECK(click("pause-hotkey"));
    key(SDL_SCANCODE_T, SDLK_T, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
#ifndef __APPLE__
    CHECK(click("max-seconds"));
    key(SDL_SCANCODE_A, SDLK_A, SDL_KMOD_LCTRL);
    type_text("120");
    CHECK(click("save"));
    settle("save", "Saving…");
    IS("save", "Saved, but the pause hotkey could not be registered");
    char *saved = setting_text("pauseHotkey");
    CHECK(eh_settings_window_is_open() && !saved[0] && setting("audio.maxRecordingSeconds") == 120);
    SDL_free(saved);
    IS("pause-hotkey", TAKEN);
    CHECK(SDL_strstr(eh_settings_window_text("pause-hotkey-status"), "Could not register \"Ctrl+Alt+T\"") != NULL);
#endif
    // Corrected, the save goes through and closes the window.
    CHECK(click("pause-hotkey"));
    key(SDL_SCANCODE_P, SDLK_P, SDL_KMOD_LALT);
    IS("pause-hotkey-status", "");
    CHECK(click("save"));
    for (Uint64 until = SDL_GetTicks() + 5000; SDL_GetTicks() < until && eh_settings_window_is_open();) frames(1), SDL_Delay(10);
    CHECK(!eh_settings_window_is_open());
    char *pause = setting_text("pauseHotkey");
    CHECK(!SDL_strcmp(pause, "Alt+P"));
    SDL_free(pause);

    // Settings files as v0.39.0 wrote them: opened and written out untouched, each is unchanged.
    int files = 0;
    if (argc > 0) {
        size_t len = 0;
        char *text = SDL_LoadFile(argv[0], &len);
        eh_jv *doc = text ? eh_jv_parse(text, len) : NULL;
        const eh_jv *stored = eh_jv_get(doc, "stored");
        CHECK(eh_jv_count(stored) > 0);
        for (int i = 0; i < eh_jv_count(stored); i++, files++) {
            char error[256];
            // The form trims dictionary terms and drops empty ones, as the Electron form does, so
            // the file is given to it that way.
            eh_jv *file = eh_jv_clone(eh_jv_get(eh_jv_at(stored, i), "merged")), *terms = eh_jv_array();
            const eh_jv *before = eh_jv_path(file, "cleanup.dictionary");
            for (int t = 0; t < eh_jv_count(before); t++) {
                const char *term = eh_jv_string(eh_jv_at(before, t), "");
                while (*term == ' ') term++;
                size_t n = SDL_strlen(term);
                while (n && term[n - 1] == ' ') n--;
                char *trimmed = SDL_strndup(term, n);
                if (n) eh_jv_push(terms, eh_jv_str(trimmed));
                SDL_free(trimmed);
            }
            eh_jv_set_path(file, "cleanup.dictionary", terms);
            eh_lock();
            bool ok = eh_settings_save(file, error, sizeof error);
            eh_unlock();
            CHECK(ok);
            eh_settings_window_open(NULL);
            frames(3);
            if (!eh_settings_window_untouched()) printf("  FAILED: stored settings file %d does not come back unchanged\n", i), failed++;
            checks++;
            eh_settings_window_close();
        }
        eh_jv_free(doc);
        SDL_free(text);
    }

    eh_lock();
    eh_hotkeys_release_all();
    eh_engines_stop();
    eh_unlock();
    // The folder the test made for its settings goes with it.
    int count = 0;
    char **names = SDL_GlobDirectory(data, NULL, 0, &count);
    for (int i = 0; names && i < count; i++) {
        char path[1500];
        SDL_snprintf(path, sizeof path, "%s" EH_SEP "%s", data, names[i]);
        SDL_RemovePath(path);
    }
    SDL_free(names);
    SDL_RemovePath(data);
    SDL_Quit();
    printf("settings-smoke: %d checks, %d failed, %d stored settings files round-tripped\n", checks, failed, files);
    return failed ? 1 : 0;
}
