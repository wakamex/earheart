// `earheart settings-smoke [SETTINGS_VECTORS]` and `earheart wizard-smoke`: a window driven as a
// user drives it, with mouse, wheel and keyboard events given to the real window, and what it
// then shows and saves checked. The checks are the windows' own from scripts/settings-smoke.js.
// They need a display, and exit 1 when a check fails.
#include <SDL3/SDL.h>
#include <stdio.h>
#include "app.h"
#include "correction_window.h"
#include "deliver.h"
#include "history.h"
#include "gs_desk.h"
#include "hotkeys.h"
#include "gs_json.h"
#include "loop.h"
#include "paths.h"
#include "settings.h"
#include "settings_window.h"
#include "ui.h"
#include "wizard_window.h"

static int failed, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } } while (0)
#define IS(what, expected) do { checks++; const char *got_ = eh_settings_window_text(what); if (SDL_strcmp(got_, expected)) { printf("  FAILED %s:%d: %s is \"%s\", expected \"%s\"\n", __FILE__, __LINE__, what, got_, expected); failed++; } } while (0)

// The window being driven.
static struct {
    bool (*event)(const SDL_Event *e);
    int (*frame)(void);
    struct eh_ui *(*ui)(void);
    bool (*open)(void);
} T;

static eh_ui *ui(void) { return (eh_ui *)T.ui(); }

static void frames(int n) {
    // The main thread's own work (the clipboard is written there) is done between frames.
    for (int i = 0; i < n && T.open(); i++) SDL_PumpEvents(), T.frame();
}

static SDL_WindowID window(void) { return SDL_GetWindowID(eh_ui_window(ui())); }

static void pointer(Uint32 type, float x, float y) {
    SDL_Event e = { 0 };
    if (type == SDL_EVENT_MOUSE_MOTION) e.motion = (SDL_MouseMotionEvent){ .type = type, .windowID = window(), .x = x, .y = y };
    else e.button = (SDL_MouseButtonEvent){ .type = type, .windowID = window(), .button = SDL_BUTTON_LEFT, .down = type == SDL_EVENT_MOUSE_BUTTON_DOWN, .x = x, .y = y };
    T.event(&e);
}

// Where the wheel is turned: the page's left margin, where no text area is under the pointer to
// take the wheel for itself.
#define PAGE_GUTTER_X 190

static void wheel(float x, float y, float amount) {
    pointer(SDL_EVENT_MOUSE_MOTION, x, y);
    SDL_Event e = { 0 };
    e.wheel = (SDL_MouseWheelEvent){ .type = SDL_EVENT_MOUSE_WHEEL, .windowID = window(), .y = amount, .mouse_x = x, .mouse_y = y };
    T.event(&e);
    frames(2);
}

static void key(SDL_Scancode sc, SDL_Keycode k, SDL_Keymod mod) {
    SDL_Event e = { 0 };
    e.key = (SDL_KeyboardEvent){ .type = SDL_EVENT_KEY_DOWN, .windowID = window(), .scancode = sc, .key = k, .mod = mod, .down = true };
    T.event(&e);
    frames(2);
}

static void type_text(const char *text) {
    SDL_Event e = { 0 };
    e.text = (SDL_TextInputEvent){ .type = SDL_EVENT_TEXT_INPUT, .windowID = window(), .text = text };
    T.event(&e);
    frames(2);
}

// Scrolls the page until the control is whole inside it. False when the page has no such control.
static bool reveal(const char *id, gs_rect *r) {
    int w, h;
    if (!T.open()) return false;
    SDL_GetWindowSize(eh_ui_window(ui()), &w, &h);
    for (int tries = 0; tries < 200; tries++) {
        frames(1);
        if (!eh_ui_stop_rect(ui(), id, r)) return false;
        if (!eh_ui_stop_paged(ui(), id)) return true;  // not on the page: always where it is
        if (r->y >= 80 && r->y + r->h <= (float)h - 72) return true;
        wheel(PAGE_GUTTER_X, (float)h / 2, r->y < 80 ? 1 : -1);
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
    if (SDL_getenv("EARHEART_SMOKE_VERBOSE")) printf("  clicked %s at %.0f,%.0f (rect %.0f,%.0f %.0fx%.0f); focus now \"%s\"\n", id, x, y, r.x, r.y, r.w, r.h, T.open() ? eh_ui_focus_name(ui()) : "(closed)");
    return true;
}

static bool has(const char *id) {
    gs_rect r;
    frames(1);
    return T.open() && eh_ui_stop_rect(ui(), id, &r);
}

// Frames until the window says `what` is no longer `busy`, or five seconds pass.
static void settle(const char *what, const char *busy) {
    for (Uint64 until = SDL_GetTicks() + 5000; SDL_GetTicks() < until && T.open();) {
        frames(1);
        if (SDL_strcmp(eh_settings_window_text(what), busy)) return;
        SDL_Delay(10);
    }
}

// A stand-in for the desktop's key grabs: one key is another app's.
#define TAKEN "CommandOrControl+Alt+T"
static int fake_add(const char *accelerator, int name, char *error, size_t size) { return SDL_strcmp(accelerator, TAKEN) ? 0 : 1; }
static void fake_remove(const char *accelerator) {}
static void no_hotkey(int id) {}

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

// The app with a folder of its own for its settings and a stand-in for the desktop's key grabs.
static char data[1200];

static bool begin(const char *name) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    SDL_snprintf(data, sizeof data, "%s" EH_SEP "%s-%llu", eh_data_dir(), name, (unsigned long long)SDL_GetTicksNS());
    SDL_CreateDirectory(data);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "earheart: %s\n", SDL_GetError()); return false; }
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    eh_app_init(data, NULL, eh_deliver);
    // The desktop's hotkeys are set up as in the app, though the test grabs none: on X11 that is
    // also what keeps an X error from ending the process.
    char why[256];
    gs_hotkeys_init(no_hotkey, why, sizeof why);
    static const eh_hotkey_backend fake = { fake_add, fake_remove };
    eh_lock();
    eh_hotkeys_backend(&fake);
    eh_hotkeys_apply(eh_settings(), NULL);
    eh_unlock();
    return true;
}

static int end(const char *name, const char *extra) {
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
    printf("%s: %d checks, %d failed%s\n", name, checks, failed, extra);
    return failed ? 1 : 0;
}

static void settings_target(void) {
    T.event = eh_settings_window_event, T.frame = eh_settings_window_frame, T.ui = eh_settings_window_ui, T.open = eh_settings_window_is_open;
}

int eh_cmd_settings_smoke(int argc, char **argv) {
    if (!begin("settings-smoke")) return 1;
    settings_target();

    // Two saved transcripts, one long enough to be cut at three lines: the history has rows.
    eh_lock();
    eh_history_add("a short one", "A short one.", true, false, 10);
    eh_history_add("long", "This transcript goes on for long enough that the history shows only its first three lines until it is opened, so it says the same thing several times over: this transcript goes on for long enough that the history shows only its first three lines until it is opened, and once more, this transcript goes on for long enough that the history shows only its first three lines until it is opened, and a last time so that it is surely more than three lines at any width the window has.", false, false, 10);
    eh_unlock();

    // The window opens on General with its six sections in the index, and an untouched draft is
    // the settings it was filled from.
    eh_settings_window_open(NULL);
    if (!T.open()) { printf("no window: %s\n", SDL_GetError()); return 1; }
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
    for (int i = 0; i < 60; i++) wheel(PAGE_GUTTER_X, (float)h / 2, -3);
    IS("section", "advanced");
    for (int i = 0; i < 60; i++) wheel(PAGE_GUTTER_X, (float)h / 2, 3);
    IS("section", "general");

    // Over a text area with nothing of its own to scroll, the wheel still moves the page.
    CHECK(reveal("cleanup-dictionary", &r));
    for (int i = 0; i < 80; i++) wheel(r.x + 40, r.y + r.h / 2, -3);
    IS("section", "advanced");
    for (int i = 0; i < 60; i++) wheel(PAGE_GUTTER_X, (float)h / 2, 3);
    IS("section", "general");

    // History: rows with their buttons, a long one opened and closed, and what comes after the
    // rows still there to be used.
    CHECK(has("history-copy-0") && has("history-copy-1") && has("history-correct-0") && has("history-more-0") && !has("history-more-1") && has("history-original-1"));
    // (Measured from the Clear history button above the rows, which scrolling moves with them.)
    gs_rect top, row;
    CHECK(reveal("history-more-0", &row));
    CHECK(eh_ui_stop_rect(ui(), "history-clear", &top) && eh_ui_stop_rect(ui(), "history-copy-0", &row));
    float closed = row.y - top.y;
    CHECK(click("history-more-0"));
    CHECK(eh_ui_stop_rect(ui(), "history-clear", &top) && eh_ui_stop_rect(ui(), "history-copy-0", &row) && row.y - top.y > closed + 10);
    CHECK(click("history-more-0"));
    CHECK(eh_ui_stop_rect(ui(), "history-clear", &top) && eh_ui_stop_rect(ui(), "history-copy-0", &row) && SDL_fabsf(row.y - top.y - closed) < 1);
    if (SDL_getenv("EARHEART_SMOKE_VERBOSE")) printf("  history row closed at %.2f, was %.2f\n", row.y - top.y, closed);
    CHECK(click("max-seconds") && !SDL_strcmp(eh_ui_focus_name(ui()), "max-seconds"));
    for (int i = 0; i < 60; i++) wheel(PAGE_GUTTER_X, (float)h / 2, 3);

    // A hotkey field: a click arms it, Escape lets go with nothing changed, a combination fills it,
    // a key without a modifier does not, and Clear empties it.
    CHECK(click("pause-hotkey") && eh_ui_capturing(ui()));
    key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0);
    CHECK(!eh_ui_capturing(ui()) && T.open());
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
    // A model from Hugging Face: the address is read before anything is asked of the network, so
    // one that is not a repository is refused at once, in the form's own words.
    CHECK(has("stt-hf-url") && has("stt-hf-find") && has("stt-hf-browse") && !has("stt-hf-add") && has("cleanup-hf-url"));
    CHECK(click("stt-hf-url"));
    type_text("https://example.com/owner/model");
    CHECK(click("stt-hf-find"));
    for (Uint64 until = SDL_GetTicks() + 3000; SDL_GetTicks() < until && !SDL_strcmp(eh_settings_window_text("stt-hf"), "Looking up versions…");) frames(1), SDL_Delay(10);
    IS("stt-hf", "Only huggingface.co URLs (or a bare owner/model) are supported");
    CHECK(!has("stt-hf-add"));
    key(SDL_SCANCODE_A, SDLK_A, SDL_KMOD_LCTRL);
    key(SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, 0);
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
    CHECK(T.open() && setting("audio.maxRecordingSeconds") == 3600);
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
    CHECK(T.open() && !saved[0] && setting("audio.maxRecordingSeconds") == 120);
    SDL_free(saved);
    IS("pause-hotkey", TAKEN);
    CHECK(SDL_strstr(eh_settings_window_text("pause-hotkey-status"), "Could not register \"Ctrl+Alt+T\"") != NULL);
#endif
    // Corrected, the save goes through and closes the window.
    CHECK(click("pause-hotkey"));
    key(SDL_SCANCODE_P, SDLK_P, SDL_KMOD_LALT);
    IS("pause-hotkey-status", "");
    CHECK(click("save"));
    for (Uint64 until = SDL_GetTicks() + 5000; SDL_GetTicks() < until && T.open();) frames(1), SDL_Delay(10);
    CHECK(!T.open());
    char *pause = setting_text("pauseHotkey");
    CHECK(!SDL_strcmp(pause, "Alt+P"));
    SDL_free(pause);

    // Settings files as v0.39.0 wrote them: opened and written out untouched, each is unchanged.
    int files = 0;
    if (argc > 0) {
        size_t len = 0;
        char *text = SDL_LoadFile(argv[0], &len);
        gs_jval *doc = text ? gs_jval_parse(text, len) : NULL;
        const gs_jval *stored = gs_jval_get(doc, "stored");
        CHECK(gs_jval_count(stored) > 0);
        for (int i = 0; i < gs_jval_count(stored); i++, files++) {
            char error[256];
            // The form trims dictionary terms and drops empty ones, as the Electron form does, so
            // the file is given to it that way.
            gs_jval *file = gs_jval_clone(gs_jval_get(gs_jval_at(stored, i), "merged")), *terms = gs_jval_array();
            const gs_jval *before = gs_jval_path(file, "cleanup.dictionary");
            for (int t = 0; t < gs_jval_count(before); t++) {
                const char *term = gs_jval_string(gs_jval_at(before, t), "");
                while (*term == ' ') term++;
                size_t n = SDL_strlen(term);
                while (n && term[n - 1] == ' ') n--;
                char *trimmed = SDL_strndup(term, n);
                if (n) gs_jval_push(terms, gs_jval_str(trimmed));
                SDL_free(trimmed);
            }
            gs_jval_set_path(file, "cleanup.dictionary", terms);
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
        gs_jval_free(doc);
        SDL_free(text);
    }

    char extra[64];
    SDL_snprintf(extra, sizeof extra, ", %d stored settings files round-tripped", files);
    return end("settings-smoke", extra);
}

/* ---------------- the wizard ---------------- */

static void wizard_target(void) {
    T.event = eh_wizard_window_event, T.frame = eh_wizard_window_frame, T.ui = eh_wizard_window_ui, T.open = eh_wizard_window_is_open;
}

int eh_cmd_wizard_smoke(int argc, char **argv) {
    if (!begin("wizard-smoke")) return 1;
    wizard_target();
    eh_wizard_window_open();
    if (!T.open()) { printf("no window: %s\n", SDL_GetError()); return 1; }
    frames(4);

    // Seven steps, forward and back; the first has no Back, and Skip is always there.
    CHECK(eh_wizard_window_step() == 0 && has("next") && has("skip") && !has("back"));
    for (int step = 1; step < 7; step++) {
        CHECK(click("next"));
        CHECK(eh_wizard_window_step() == step && has("back"));
    }
    for (int step = 5; step >= 1; step--) {
        CHECK(click("back"));
        CHECK(eh_wizard_window_step() == step);
    }

    // The hotkey step: capture, and Reset back to the default.
    CHECK(has("hotkey") && has("hotkey-clear"));
    CHECK(click("hotkey"));
    CHECK(eh_ui_capturing(ui()));
    key(SDL_SCANCODE_T, SDLK_T, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    CHECK(!eh_ui_capturing(ui()));
    CHECK(click("next") && has("mic-device"));
    CHECK(click("next"));
    // Cleanup: off puts its choices out of reach.
    CHECK(click("next") && has("cleanup-enabled") && has("cleanup-builtin-model") && has("cleanup-style"));
    CHECK(click("cleanup-enabled"));
    CHECK(!has("cleanup-builtin-model") && !has("cleanup-style"));
    CHECK(click("cleanup-enabled") && has("cleanup-builtin-model"));
    CHECK(click("cleanup-enabled"));
    // Output, with the summary.
    CHECK(click("next") && has("output-paste") && has("output-clipboard"));
    CHECK(click("output-clipboard"));
    // The last step: with cleanup off only the speech model is wanted. Its download is put off,
    // which lets the wizard finish.
    CHECK(click("next") && eh_wizard_window_step() == 6);
    if (has("download-later")) CHECK(click("download-later"));
    CHECK(!has("download-later"));
#ifndef __APPLE__
    // The hotkey chosen is another app's: nothing closes, and the hotkey step says why.
    CHECK(click("next"));
    for (Uint64 until = SDL_GetTicks() + 5000; SDL_GetTicks() < until && eh_wizard_window_step() == 6;) frames(1), SDL_Delay(10);
    CHECK(T.open() && eh_wizard_window_step() == 1 && !eh_settings_window_is_open());
    char *mode = setting_text("output.mode");
    CHECK(!SDL_strcmp(mode, "clipboard") && !setting_text("cleanup.enabled")[0]);
    SDL_free(mode);
#else
    for (int step = 5; step >= 1; step--) click("back");
#endif
    // Corrected, it finishes: the choices are saved, the wizard closes and the settings open with
    // a line saying where their values came from.
    CHECK(click("hotkey-clear"));
    for (int step = 2; step < 7; step++) CHECK(click("next"));
    if (has("download-later")) CHECK(click("download-later"));
    CHECK(click("next"));
    for (Uint64 until = SDL_GetTicks() + 5000; SDL_GetTicks() < until && T.open();) frames(1), SDL_Delay(10);
    CHECK(!T.open() && eh_settings_window_is_open());
    eh_lock();
    CHECK(!eh_setting_bool("cleanup.enabled") && !SDL_strcmp(eh_setting_str("stt.engine"), "builtin") && !SDL_strcmp(eh_setting_str("output.mode"), "clipboard"));
    CHECK(!SDL_strcmp(eh_setting_str("hotkey"), "CommandOrControl+Shift+Space") && eh_hotkeys_status(EH_HK_RECORD).ok);
    eh_unlock();
    settings_target();
    CHECK(has("wizard-banner-dismiss"));
    CHECK(click("wizard-banner-dismiss") && !has("wizard-banner-dismiss"));
    // Run again from the settings, and skipped: the settings come back without that line.
    CHECK(click("open-wizard"));
    frames(2);
    CHECK(eh_wizard_window_is_open());
    wizard_target();
    frames(3);
    CHECK(eh_wizard_window_step() == 0 && click("skip"));
    for (Uint64 until = SDL_GetTicks() + 5000; SDL_GetTicks() < until && T.open();) frames(1), SDL_Delay(10);
    CHECK(!T.open() && eh_settings_window_is_open());
    settings_target();
    CHECK(!has("wizard-banner-dismiss") && has("save"));
    eh_settings_window_close();
    return end("wizard-smoke", "");
}

/* ---------------- the correction editor ---------------- */

static void correction_target(void) {
    T.event = eh_correction_window_event, T.frame = eh_correction_window_frame, T.ui = eh_correction_window_ui, T.open = eh_correction_window_is_open;
}

#define HOLDS(what, expected) do { checks++; const char *got_ = eh_correction_window_text(what); if (SDL_strcmp(got_, expected)) { printf("  FAILED %s:%d: %s is \"%s\", expected \"%s\"\n", __FILE__, __LINE__, what, got_, expected); failed++; } } while (0)

static bool in_dictionary(const char *term) {
    eh_lock();
    const gs_jval *list = gs_jval_path(eh_settings(), "cleanup.dictionary");
    bool found = false;
    for (int i = 0; i < gs_jval_count(list); i++) found = found || !SDL_strcmp(gs_jval_string(gs_jval_at(list, i), ""), term);
    eh_unlock();
    return found;
}

int eh_cmd_correction_smoke(int argc, char **argv) {
    if (!begin("correction-smoke")) return 1;
    correction_target();
    char error[200];
    CHECK(eh_correction_window_open("I use gesso with zig and sherpa on x eleven", error, sizeof error));
    if (!T.open()) { printf("no window: %s\n", SDL_GetError()); return 1; }
    frames(4);
    CHECK(has("transcript") && has("spelling") && has("cancel") && has("copy"));
    // Nothing selected: nothing to replace, nothing to undo.
    CHECK(!has("remember") && !has("replace") && !has("undo"));
    CHECK(!SDL_strcmp(eh_ui_focus_name(ui()), "transcript"));

    // By keyboard: Shift with the arrows selects, Tab goes to the spelling, Enter replaces and
    // remembers. The cursor starts at the top of the transcript.
    for (int i = 0; i < 6; i++) key(SDL_SCANCODE_RIGHT, SDLK_RIGHT, 0);
    HOLDS("spelling", "");
    for (int i = 0; i < 5; i++) key(SDL_SCANCODE_RIGHT, SDLK_RIGHT, SDL_KMOD_LSHIFT);
    HOLDS("spelling", "gesso");
    CHECK(has("remember") && has("replace"));
    // A blank at the selection's end is not part of what is replaced.
    key(SDL_SCANCODE_RIGHT, SDLK_RIGHT, SDL_KMOD_LSHIFT);
    HOLDS("spelling", "gesso");
    key(SDL_SCANCODE_TAB, SDLK_TAB, 0);
    CHECK(!SDL_strcmp(eh_ui_focus_name(ui()), "spelling"));
    key(SDL_SCANCODE_A, SDLK_A, SDL_KMOD_LCTRL);
    type_text("Gesso");
    key(SDL_SCANCODE_RETURN, SDLK_RETURN, 0);
    HOLDS("text", "I use Gesso with zig and sherpa on x eleven");
    HOLDS("status", "Replaced and remembered “Gesso”.");
    HOLDS("spelling", "");
    CHECK(in_dictionary("Gesso") && has("undo"));
    CHECK(!SDL_strcmp(eh_ui_focus_name(ui()), "transcript"));

    // Undo puts the text back and selects it again; the spelling stays remembered.
    CHECK(click("undo"));
    HOLDS("text", "I use gesso with zig and sherpa on x eleven");
    HOLDS("spelling", "gesso");
    CHECK(in_dictionary("Gesso") && !has("undo"));

    // A phrase, selected from the end, replaced without remembering.
    eh_ui_focus(ui(), "transcript");
    frames(2);
    key(SDL_SCANCODE_END, SDLK_END, SDL_KMOD_LCTRL);
    for (int i = 0; i < 8; i++) key(SDL_SCANCODE_LEFT, SDLK_LEFT, SDL_KMOD_LSHIFT);
    HOLDS("spelling", "x eleven");
    key(SDL_SCANCODE_TAB, SDLK_TAB, 0);
    key(SDL_SCANCODE_A, SDLK_A, SDL_KMOD_LCTRL);
    type_text("X11");
    CHECK(click("replace"));
    HOLDS("text", "I use gesso with zig and sherpa on X11");
    HOLDS("status", "Replaced this occurrence.");
    CHECK(!in_dictionary("X11") && has("undo"));

    // Escape in the spelling field gives up the selection and nothing else.
    for (int i = 0; i < 3; i++) key(SDL_SCANCODE_LEFT, SDLK_LEFT, SDL_KMOD_LSHIFT);
    HOLDS("spelling", "X11");
    key(SDL_SCANCODE_TAB, SDLK_TAB, 0);
    type_text("Z");
    key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0);
    HOLDS("status", "Pending replacement cancelled.");
    HOLDS("spelling", "");
    CHECK(T.open() && !SDL_strcmp(eh_ui_focus_name(ui()), "transcript"));

    // The transcript changed by hand: the replacements before it can no longer be undone.
    key(SDL_SCANCODE_END, SDLK_END, SDL_KMOD_LCTRL);
    type_text(".");
    HOLDS("text", "I use gesso with zig and sherpa on X11.");
    CHECK(!has("undo"));

    // A spelling typed and not applied stops the copy and says what to do.
    for (int i = 0; i < 4; i++) key(SDL_SCANCODE_LEFT, SDLK_LEFT, SDL_KMOD_LSHIFT);
    HOLDS("spelling", "X11.");
    key(SDL_SCANCODE_TAB, SDLK_TAB, 0);
    type_text("zz");
    key(SDL_SCANCODE_RETURN, SDLK_RETURN, SDL_KMOD_LCTRL);
    HOLDS("status", "Apply your spelling with Replace & remember or Replace only before copying.");
    CHECK(T.open() && !SDL_strcmp(eh_ui_focus_name(ui()), "remember"));

    // Another transcript does not replace an open draft.
    CHECK(eh_correction_window_open("something else", error, sizeof error));
    frames(2);
    HOLDS("text", "I use gesso with zig and sherpa on X11.");
    HOLDS("status", "Your current correction is still open. Copy or close it before opening another transcript.");

    // Closing with a draft is asked about; kept, the draft is still there.
    CHECK(click("cancel"));
    frames(2);
    CHECK(T.open() && eh_ui_asking(ui()) && !has("cancel"));
    key(SDL_SCANCODE_RETURN, SDLK_RETURN, 0);  // the keyboard starts on the answer that keeps the draft
    CHECK(T.open() && !eh_ui_asking(ui()) && has("cancel"));
    HOLDS("text", "I use gesso with zig and sherpa on X11.");

    // The pending spelling given up, Ctrl+Enter copies: the clipboard holds the text and the editor closes.
    CHECK(click("spelling"));
    key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0);
    key(SDL_SCANCODE_RETURN, SDLK_RETURN, SDL_KMOD_LCTRL);
    for (Uint64 until = SDL_GetTicks() + 5000; SDL_GetTicks() < until && T.open();) frames(1), SDL_Delay(10);
    CHECK(!T.open());
    char *clip = SDL_GetClipboardText();
    CHECK(clip && !SDL_strcmp(clip, "I use gesso with zig and sherpa on X11."));
    SDL_free(clip);

    // With nothing saved the editor says so, and an untouched one closes without a question.
    CHECK(eh_correction_window_open("", error, sizeof error));
    frames(3);
    HOLDS("status", "No saved transcript yet. Enable History and finish a dictation first.");
    CHECK(!has("copy"));
    key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0);
    frames(2);
    CHECK(!T.open());
    return end("correction-smoke", "");
}
