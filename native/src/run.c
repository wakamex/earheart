// The app as the user runs it: it lives in the tray, listens for its hotkeys, and takes the
// arguments of later instances (`earheart --toggle` bound to a system shortcut is how a Wayland
// desktop that refuses key grabs starts a dictation).
//
//   earheart                       start, or bring up the running instance
//   earheart --hidden              start in the tray without the wizard
//   earheart --toggle | --pause | --discard | --correct-last
//                                  the same as the hotkey, sent to the running instance
//   earheart --smoke-test          start, then quit: for CI
//   earheart --smoke-test --engine-check   also check that both engines load
#include <SDL3/SDL.h>
#include <stdio.h>
#include "app.h"
#include "deliver.h"
#include "desk.h"
#include "engine.h"
#include "history.h"
#include "icons.h"
#include "loop.h"
#include "overlay.h"
#include "paths.h"
#include "recorder.h"
#include "settings.h"
#include "stb_image.h"

enum { HOTKEY_RECORD = 1, HOTKEY_PAUSE, HOTKEY_DISCARD, HOTKEY_CORRECT };

static struct {
    SDL_Tray *tray;
    SDL_TrayMenu *menu;
    SDL_Surface *icon, *icon_recording;
    Uint32 refresh_event;
    bool hotkeys;           // the desktop lets keys be grabbed
    bool discard_held;      // the discard hotkey is registered, which it is only while a dictation is live
    FILE *log;
    SDL_Mutex *log_lock;
} R;

/* ---------------- the log ---------------- */

static void log_line(void *user, int category, SDL_LogPriority priority, const char *message) {
    SDL_Time now;
    SDL_DateTime dt = { 0 };
    if (SDL_GetCurrentTime(&now)) SDL_TimeToDateTime(now, &dt, true);
    SDL_LockMutex(R.log_lock);
    for (int i = 0; i < 2; i++) {
        FILE *f = i ? stderr : R.log;
        if (!f) continue;
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s%s\n", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, priority >= SDL_LOG_PRIORITY_WARN ? "[warn] " : "", message);
        fflush(f);
    }
    SDL_UnlockMutex(R.log_lock);
}

/* ---------------- actions ---------------- */

typedef enum { ACT_TOGGLE, ACT_PAUSE, ACT_DISCARD, ACT_CANCEL, ACT_COPY_LAST, ACT_MODE_PASTE, ACT_MODE_PASTE_COPY, ACT_MODE_CLIPBOARD, ACT_HOTKEYS } action;

static void apply_hotkeys(void);

// Every action runs as a task, so the thread that asked (the tray's, a hotkey's, another
// instance's) never waits on the pipeline.
static void act(void *user) {
    char error[256];
    switch ((action)(intptr_t)user) {
    case ACT_TOGGLE: eh_pipeline_toggle(); break;
    case ACT_PAUSE: eh_pipeline_pause_toggle(); break;
    case ACT_DISCARD: eh_pipeline_discard(); break;
    case ACT_CANCEL: eh_pipeline_cancel(); break;
    case ACT_COPY_LAST: {
        const eh_jv *last = eh_jv_at(eh_history_list(), 0);
        if (!last || eh_pipeline_state() == EH_PROCESSING) break;
        eh_jv *output = eh_jv_object();
        eh_jv_set(output, "mode", eh_jv_str("clipboard"));
        char *text = SDL_strdup(eh_jv_string(eh_jv_get(last, "text"), ""));
        eh_delivery result;
        if (!eh_deliver(text, output, NULL, &result, error, sizeof error)) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "could not copy transcript: %s", error);
        SDL_free(text);
        eh_jv_free(output);
        break;
    }
    case ACT_MODE_PASTE: case ACT_MODE_PASTE_COPY: case ACT_MODE_CLIPBOARD: {
        const char *mode = (action)(intptr_t)user == ACT_MODE_PASTE ? "paste" : (action)(intptr_t)user == ACT_MODE_PASTE_COPY ? "paste-copy" : "clipboard";
        eh_jv *next = eh_jv_clone(eh_settings());
        eh_jv_set_path(next, "output.mode", eh_jv_str(mode));
        eh_jv_set_path(next, "output.restoreClipboard", eh_jv_bool(true));
        if (!eh_settings_save(next, error, sizeof error)) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "could not save the output mode: %s", error);
        break;
    }
    case ACT_HOTKEYS: apply_hotkeys(); break;
    }
}

static void spawn_action(action a) { eh_spawn(act, (void *)(intptr_t)a); }

static void hotkey_fired(int id) {
    spawn_action(id == HOTKEY_RECORD ? ACT_TOGGLE : id == HOTKEY_PAUSE ? ACT_PAUSE : ACT_DISCARD);
}

static void instance_args(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (!SDL_strcmp(argv[i], "--toggle")) spawn_action(ACT_TOGGLE);
        else if (!SDL_strcmp(argv[i], "--pause")) spawn_action(ACT_PAUSE);
        else if (!SDL_strcmp(argv[i], "--discard")) spawn_action(ACT_DISCARD);
    }
}

/* ---------------- hotkeys ---------------- */

// With the lock held. One hotkey from the settings; an empty one is released.
static void apply_hotkey(int id, const char *path, const char *what, bool wanted) {
    const char *text = eh_setting_str(path);
    char error[256], label[64];
    eh_accelerator a;
    if (!wanted || !*text) { EH_AWAIT(eh_hotkey_unregister(id)); return; }
    bool ok = eh_accelerator_parse(text, &a, error, sizeof error);
    if (ok) EH_AWAIT(ok = eh_hotkey_register(id, &a, error, sizeof error));
    if (ok) return;
    eh_accelerator_label(text, label, sizeof label);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "could not register the %s hotkey \"%s\" (%s)", what, label, error);
}

static void apply_hotkeys(void) {
    if (!R.hotkeys) return;
    apply_hotkey(HOTKEY_RECORD, "hotkey", "record", true);
    apply_hotkey(HOTKEY_PAUSE, "pauseHotkey", "pause", true);
    // The discard hotkey is held only while a dictation is live, so the key is the user's the
    // rest of the time.
    R.discard_held = eh_pipeline_state() != EH_IDLE;
    apply_hotkey(HOTKEY_DISCARD, "discardHotkey", "discard", R.discard_held);
}

/* ---------------- the tray ---------------- */

static void menu_click(void *user, SDL_TrayEntry *entry) { spawn_action((action)(intptr_t)user); }

static void quit_click(void *user, SDL_TrayEntry *entry) {
    SDL_Event e = { .type = SDL_EVENT_QUIT };
    SDL_PushEvent(&e);
}

static void add(const char *label, SDL_TrayEntryFlags flags, SDL_TrayCallback cb, action a) {
    SDL_TrayEntry *e = SDL_InsertTrayEntryAt(R.menu, -1, label, flags);
    if (e && cb) SDL_SetTrayEntryCallback(e, cb, (void *)(intptr_t)a);
}

// On the main thread: the menu and the icon follow the pipeline's state and the settings.
static void refresh_tray(void) {
    if (!R.tray) return;
    eh_lock();
    eh_state state = eh_pipeline_state();
    const char *mode = eh_setting_str("output.mode");
    // Paste without restoring the clipboard is what "paste and keep" means.
    bool keep = !SDL_strcmp(mode, "paste-copy") || (!SDL_strcmp(mode, "paste") && !eh_setting_bool("output.restoreClipboard"));
    bool paste = !SDL_strcmp(mode, "paste") && !keep, has_last = eh_jv_count(eh_history_list()) > 0;
    eh_unlock();

    // The list is SDL's own and shifts under a removal, so it is asked for again each time.
    for (int count = 1; count;) {
        const SDL_TrayEntry **entries = SDL_GetTrayEntries(R.menu, &count);
        if (entries && count) SDL_RemoveTrayEntry((SDL_TrayEntry *)entries[count - 1]);
    }
    add(state == EH_RECORDING ? "Stop & transcribe" : state == EH_PROCESSING ? "Processing…" : "Start dictation",
        SDL_TRAYENTRY_BUTTON | (state == EH_PROCESSING ? SDL_TRAYENTRY_DISABLED : 0), menu_click, ACT_TOGGLE);
    if (state != EH_IDLE) add("Cancel", SDL_TRAYENTRY_BUTTON, menu_click, ACT_CANCEL);
    add(NULL, 0, NULL, 0);
    add("Paste into active app", SDL_TRAYENTRY_CHECKBOX | (paste ? SDL_TRAYENTRY_CHECKED : 0), menu_click, ACT_MODE_PASTE);
    add("Paste and keep on clipboard", SDL_TRAYENTRY_CHECKBOX | (keep ? SDL_TRAYENTRY_CHECKED : 0), menu_click, ACT_MODE_PASTE_COPY);
    add("Copy to clipboard only", SDL_TRAYENTRY_CHECKBOX | (!paste && !keep ? SDL_TRAYENTRY_CHECKED : 0), menu_click, ACT_MODE_CLIPBOARD);
    add(NULL, 0, NULL, 0);
    add("Copy last transcription", SDL_TRAYENTRY_BUTTON | (has_last && state != EH_PROCESSING ? 0 : SDL_TRAYENTRY_DISABLED), menu_click, ACT_COPY_LAST);
    add(NULL, 0, NULL, 0);
    SDL_TrayEntry *quit = SDL_InsertTrayEntryAt(R.menu, -1, "Quit Earheart", SDL_TRAYENTRY_BUTTON);
    if (quit) SDL_SetTrayEntryCallback(quit, quit_click, NULL);
    SDL_SetTrayIcon(R.tray, state == EH_RECORDING ? R.icon_recording : R.icon);
    SDL_SetTrayTooltip(R.tray, state == EH_RECORDING ? "Earheart — recording" : state == EH_PROCESSING ? "Earheart — processing" : "Earheart — ready");
}

static SDL_Surface *icon_surface(const unsigned char *png, int len) {
    int w, h, n;
    unsigned char *pixels = stbi_load_from_memory(png, len, &w, &h, &n, 4);
    if (!pixels) return NULL;
    SDL_Surface *borrowed = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4), *own = borrowed ? SDL_DuplicateSurface(borrowed) : NULL;
    SDL_DestroySurface(borrowed);
    stbi_image_free(pixels);
    return own;
}

/* ---------------- what the pipeline reports ---------------- */

// Called with the lock held, from whichever task changed something. The tray is the main
// thread's, so it is only told to look again.
static void observer(const char *kind, const char *a, const char *b, double number, const eh_jv *detail) {
    if (!SDL_strcmp(kind, "notify")) {
        // Until there are desktop notifications, the log holds them.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", a, b);
    } else if (!SDL_strcmp(kind, "status")) {
        SDL_Log("status: %s%s%s", a, eh_jv_get(detail, "message") ? ": " : "", eh_jv_string(eh_jv_get(detail, "message"), ""));
    } else if (!SDL_strcmp(kind, "state")) {
        // The discard hotkey comes and goes with the dictation.
        if (R.hotkeys && (SDL_strcmp(a, "idle") != 0) != R.discard_held) spawn_action(ACT_HOTKEYS);
    }
    // The main thread is woken: the tray for what its menu shows, the overlay for everything.
    bool tray = !SDL_strcmp(kind, "state") || !SDL_strcmp(kind, "history");
    if (tray || !SDL_strcmp(kind, "status") || !SDL_strcmp(kind, "progress") || !SDL_strcmp(kind, "partial") || !SDL_strcmp(kind, "overlay")) {
        SDL_Event e = { .type = R.refresh_event, .user.code = tray };
        SDL_PushEvent(&e);
    }
}

static void settings_changed(void *user, const eh_jv *previous, const eh_jv *current) {
    static const char *const keys[] = { "hotkey", "pauseHotkey", "discardHotkey" };
    for (int i = 0; i < 3; i++)
        if (!eh_jv_equal(eh_jv_get(previous, keys[i]), eh_jv_get(current, keys[i]))) { spawn_action(ACT_HOTKEYS); break; }
    SDL_Event e = { .type = R.refresh_event, .user.code = 1 };
    SDL_PushEvent(&e);
}

static bool engine_check(void) {
    bool ok = true;
    static const char *const kinds[] = { "stt", "cleanup" };
    for (int i = 0; i < 2; i++) {
        eh_engine *e = eh_engine_new(kinds[i]);
        eh_jw w;
        eh_jw_begin(&w);
        eh_jw_str(&w, "type", "loadcheck");
        eh_engine_reply reply;
        // Generous: on Apple silicon the first load compiles the GPU backend's shaders.
        bool loaded = eh_engine_call(e, &w, NULL, 0, 120000, NULL, NULL, &reply) == EH_ENGINE_OK;
        printf("engine %s: %s%s\n", kinds[i], loaded ? "ok" : "FAILED: ", loaded ? "" : reply.error);
        ok = ok && loaded;
        eh_engine_reply_free(&reply);
        eh_engine_free(e);
    }
    return ok;
}

int eh_run(int argc, char **argv) {
    bool smoke = false, check = false, forwardable = false;
    for (int i = 1; i < argc; i++) {
        if (!SDL_strcmp(argv[i], "--smoke-test")) smoke = true;
        else if (!SDL_strcmp(argv[i], "--engine-check")) check = true;
        else if (!SDL_strcmp(argv[i], "--toggle") || !SDL_strcmp(argv[i], "--pause") || !SDL_strcmp(argv[i], "--discard") || !SDL_strcmp(argv[i], "--correct-last")) forwardable = true;
        else if (SDL_strcmp(argv[i], "--hidden")) { fprintf(stderr, "earheart: unknown option %s\n", argv[i]); return 2; }
    }
    const char *data = eh_data_dir();
    SDL_CreateDirectory(data);
    // One instance. A later one hands over what it was asked to do and leaves.
    if (!smoke && !eh_instance_acquire(data, instance_args)) {
        if (!eh_instance_forward(data, argc - 1, argv + 1)) { fprintf(stderr, "earheart: another instance is running and did not answer\n"); return 1; }
        return 0;
    }
    if (forwardable && !smoke) fprintf(stderr, "earheart: no instance was running; started one\n");

    char path[1200];
    SDL_snprintf(path, sizeof path, "%s" EH_SEP "earheart.log", data);
    R.log = fopen(path, "a");
    R.log_lock = SDL_CreateMutex();
    SDL_SetLogOutputFunction(log_line, NULL);
    SDL_SetAppMetadata("Earheart", EARHEART_VERSION, "dev.wakamex.earheart");
    // A tray app: no Dock icon and no menu bar of its own on macOS.
    SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { SDL_Log("could not start: %s", SDL_GetError()); return 1; }
    R.refresh_event = SDL_RegisterEvents(1);

    eh_app_init(data, observer, eh_deliver);
    eh_lock();
    eh_settings_on_changed(settings_changed, NULL);
    // A WAV file in place of the microphone, for the end-to-end tests.
    if (SDL_getenv("EARHEART_FAKE_MIC")) eh_recorder_fake_microphone(SDL_getenv("EARHEART_FAKE_MIC"), 1);
    char error[256];
    R.hotkeys = eh_hotkeys_init(hotkey_fired, error, sizeof error);
    if (!R.hotkeys) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "no global hotkeys here (%s): bind a system shortcut to `earheart --toggle`", error);
    eh_unlock();
    spawn_action(ACT_HOTKEYS);

    R.icon = icon_surface(icon_tray, sizeof icon_tray);
    R.icon_recording = icon_surface(icon_tray_recording, sizeof icon_tray_recording);
    R.tray = SDL_CreateTray(R.icon, "Earheart — ready");
    if (R.tray) R.menu = SDL_CreateTrayMenu(R.tray);
    else SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "no tray here (%s)", SDL_GetError());
    refresh_tray();
    SDL_Log("Earheart %s started; data in %s", EARHEART_VERSION, data);

    eh_overlay_init();

    int status = 0;
    if (smoke) {
        // Everything is started; the queue is emptied once, then the engines are checked when asked.
        for (SDL_Event e; SDL_PollEvent(&e);) {}
        eh_loop_drain();
        if (check && !engine_check()) status = 1;
    } else {
        // The overlay says when it wants its next frame; until then the loop sleeps on events.
        for (int wait = -1;;) {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, wait)) {
                do {
                    if (e.type == SDL_EVENT_QUIT) goto done;
                    if (e.type == R.refresh_event && e.user.code) refresh_tray();
                    eh_overlay_event(&e);
                } while (SDL_PollEvent(&e));
            }
            wait = eh_overlay_frame();
        }
    }
done:
    eh_overlay_quit();
    eh_lock();
    eh_engines_stop();
    eh_unlock();
    if (R.tray) SDL_DestroyTray(R.tray);
    SDL_Quit();
    return status;
}
