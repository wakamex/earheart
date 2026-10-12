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
#include "activate.h"
#include "gs_desk.h"
#include "engine.h"
#include "history.h"
#include "hotkeys.h"
#include "icons.h"
#include "loop.h"
#include "overlay.h"
#include "paths.h"
#include "recorder.h"
#include "settings.h"
#include "correction_window.h"
#include "settings_window.h"
#include "wizard_window.h"
#include "stb_image.h"


static struct {
    SDL_Tray *tray;
    SDL_TrayMenu *menu;
    SDL_Surface *icon, *icon_recording;
    Uint32 refresh_event;
    bool hotkeys;           // the desktop lets keys be grabbed
    // The keys grabbed: which accelerator each of the desktop's hotkey numbers stands for.
    struct { char accelerator[64]; int name; } grabbed[8];
    FILE *log;
    SDL_Mutex *log_lock;
    // What another instance asked for before this one had finished starting, done once it has.
    SDL_AtomicInt ready;
    SDL_AtomicInt early;    // one bit per action
    bool hidden, announce, open_settings;
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

typedef enum { ACT_TOGGLE, ACT_PAUSE, ACT_DISCARD, ACT_CANCEL, ACT_COPY_LAST, ACT_MODE_PASTE, ACT_MODE_PASTE_COPY, ACT_MODE_CLIPBOARD, ACT_HOTKEYS, ACT_ARM, ACT_DISARM } action;

static void apply_hotkeys(void);
static void settings_click(void *user, SDL_TrayEntry *entry);

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
        const gs_jval *last = gs_jval_at(eh_history_list(), 0);
        if (!last || eh_pipeline_state() == EH_PROCESSING) break;
        gs_jval *output = gs_jval_object();
        gs_jval_set(output, "mode", gs_jval_str("clipboard"));
        char *text = SDL_strdup(gs_jval_string(gs_jval_get(last, "text"), ""));
        eh_delivery result;
        if (!eh_deliver(text, output, NULL, &result, error, sizeof error)) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "could not copy transcript: %s", error);
        SDL_free(text);
        gs_jval_free(output);
        break;
    }
    case ACT_MODE_PASTE: case ACT_MODE_PASTE_COPY: case ACT_MODE_CLIPBOARD: {
        const char *mode = (action)(intptr_t)user == ACT_MODE_PASTE ? "paste" : (action)(intptr_t)user == ACT_MODE_PASTE_COPY ? "paste-copy" : "clipboard";
        gs_jval *next = gs_jval_clone(eh_settings());
        gs_jval_set_path(next, "output.mode", gs_jval_str(mode));
        gs_jval_set_path(next, "output.restoreClipboard", gs_jval_bool(true));
        if (!eh_settings_save(next, error, sizeof error)) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "could not save the output mode: %s", error);
        break;
    }
    case ACT_HOTKEYS: apply_hotkeys(); break;
    // The discard hotkey comes and goes with the dictation, so the key is the user's the rest of the time.
    case ACT_ARM: eh_hotkeys_arm_discard(true); break;
    case ACT_DISARM: eh_hotkeys_arm_discard(false); break;
    }
}

static void spawn_action(action a) { eh_spawn(act, (void *)(intptr_t)a); }

// Tells the main thread to do something that is its own to do: 1 rebuild the tray, 2 open the
// settings, 3 open the wizard, 4 open the correction editor on the last transcript.
static void wake(int what) {
    SDL_Event e = { .user = { .type = R.refresh_event, .code = what } };
    SDL_PushEvent(&e);
}

// A hotkey was pressed: which one is looked up under the lock, where the table is kept.
static void fired(void *user) {
    int id = (int)(intptr_t)user;
    if (id < 1 || id > (int)SDL_arraysize(R.grabbed) || !R.grabbed[id - 1].accelerator[0]) return;
    switch (R.grabbed[id - 1].name) {
    case EH_HK_RECORD: eh_pipeline_toggle(); break;
    case EH_HK_PAUSE: eh_pipeline_pause_toggle(); break;
    case EH_HK_DISCARD: eh_pipeline_discard(); break;
    case EH_HK_CORRECT: wake(4); break;
    default: break;
    }
}

static void hotkey_fired(int id) { eh_spawn(fired, (void *)(intptr_t)id); }

// Called on the instance listener's thread, which is live before the app is.
static void instance_args(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (!SDL_strcmp(argv[i], "--settings")) { wake(2); continue; }
        if (!SDL_strcmp(argv[i], "--correct-last")) { wake(4); continue; }
        action a = !SDL_strcmp(argv[i], "--toggle") ? ACT_TOGGLE : !SDL_strcmp(argv[i], "--pause") ? ACT_PAUSE : !SDL_strcmp(argv[i], "--discard") ? ACT_DISCARD : ACT_HOTKEYS;
        if (a == ACT_HOTKEYS) continue;
        if (SDL_GetAtomicInt(&R.ready)) { spawn_action(a); continue; }
        for (int seen = SDL_GetAtomicInt(&R.early); !SDL_CompareAndSwapAtomicInt(&R.early, seen, seen | 1 << a); seen = SDL_GetAtomicInt(&R.early)) {}
    }
}

/* ---------------- hotkeys ---------------- */

// The desktop's key grabs, as src/hotkeys.c asks for them. Grabbing can wait for the main thread
// (macOS), so the lock is let go meanwhile; hotkeys.c keeps its changes from overlapping.
static int grab(const char *accelerator, int name, char *error, size_t error_size) {
    gs_accelerator a;
    if (!gs_accelerator_parse(accelerator, &a, error, error_size)) return 2;
    int slot = -1;
    for (int i = 0; i < (int)SDL_arraysize(R.grabbed); i++) {
        if (!SDL_strcmp(R.grabbed[i].accelerator, accelerator)) return 1;  // held already, as another hotkey
        if (slot < 0 && !R.grabbed[i].accelerator[0]) slot = i;
    }
    if (slot < 0 || !R.hotkeys) return 1;
    // The slot is taken before the wait, so a second grab cannot choose it meanwhile.
    SDL_strlcpy(R.grabbed[slot].accelerator, accelerator, sizeof R.grabbed[slot].accelerator);
    R.grabbed[slot].name = -1;
    bool ok;
    char why[256];
    EH_AWAIT(ok = gs_hotkey_register(slot + 1, &a, why, sizeof why));
    if (ok) R.grabbed[slot].name = name;
    else R.grabbed[slot].accelerator[0] = 0;
    return ok ? 0 : 1;
}

static void ungrab(const char *accelerator) {
    for (int i = 0; i < (int)SDL_arraysize(R.grabbed); i++) {
        if (SDL_strcmp(R.grabbed[i].accelerator, accelerator)) continue;
        R.grabbed[i].name = -1;
        EH_AWAIT(gs_hotkey_unregister(i + 1));
        R.grabbed[i].accelerator[0] = 0;
    }
}

// With the lock held: the hotkeys of the settings, each failure logged with its reason.
static void apply_hotkeys(void) {
    eh_hotkey_results results;
    eh_hotkeys_apply(eh_settings(), &results);
    static const char *const what[EH_HK_COUNT] = { "record", "pause", "discard", "correction" };
    for (int n = 0; n < EH_HK_COUNT; n++)
        if (!results.r[n].ok && R.hotkeys) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s hotkey: %s", what[n], results.r[n].error);
}

// The hotkeys, and then what a start shows (announceStartup of main/setup-notices.js): the wizard
// on a first run, the settings when the record hotkey could not be registered or when asked for;
// started hidden, nothing. Windows are opened by the main thread, which is told by an event.
static void startup(void *user) {
    apply_hotkeys();
    if (!R.announce) return;
    int open = 0;
    if (!R.hidden && eh_settings_first_run()) open = 3;
    else if (R.open_settings || (!R.hidden && R.hotkeys && !eh_hotkeys_status(EH_HK_RECORD).ok)) open = 2;
    if (!open) return;
    SDL_Event e = { .user = { .type = R.refresh_event, .code = open } };
    SDL_PushEvent(&e);
}

/* ---------------- the tray ---------------- */

static void menu_click(void *user, SDL_TrayEntry *entry) { spawn_action((action)(intptr_t)user); }

static void quit_click(void *user, SDL_TrayEntry *entry) {
    SDL_Event e = { .type = SDL_EVENT_QUIT };
    SDL_PushEvent(&e);
}

// Windows are opened on the main thread: the click is passed on as an event.
static void settings_click(void *user, SDL_TrayEntry *entry) {
    SDL_Event e = { .user = { .type = R.refresh_event, .code = 2 } };
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
    bool paste = !SDL_strcmp(mode, "paste") && !keep, has_last = gs_jval_count(eh_history_list()) > 0;
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
    SDL_TrayEntry *open = SDL_InsertTrayEntryAt(R.menu, -1, "Settings…", SDL_TRAYENTRY_BUTTON);
    if (open) SDL_SetTrayEntryCallback(open, settings_click, NULL);
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
static void observer(const char *kind, const char *a, const char *b, double number, const gs_jval *detail) {
    if (!SDL_strcmp(kind, "notify")) {
        // Until there are desktop notifications, the log holds them.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", a, b);
    } else if (!SDL_strcmp(kind, "status")) {
        SDL_Log("status: %s%s%s", a, gs_jval_get(detail, "message") ? ": " : "", gs_jval_string(gs_jval_get(detail, "message"), ""));
    } else if (!SDL_strcmp(kind, "state")) {
        spawn_action(SDL_strcmp(a, "idle") ? ACT_ARM : ACT_DISARM);
    }
    // The main thread is woken: the tray for what its menu shows, the overlay for everything.
    bool tray = !SDL_strcmp(kind, "state") || !SDL_strcmp(kind, "history");
    if (tray || !SDL_strcmp(kind, "status") || !SDL_strcmp(kind, "progress") || !SDL_strcmp(kind, "partial") || !SDL_strcmp(kind, "overlay")) {
        SDL_Event e = { .user = { .type = R.refresh_event, .code = tray } };
        SDL_PushEvent(&e);
    }
}

static void settings_changed(void *user, const gs_jval *previous, const gs_jval *current) {
    SDL_Event e = { .user = { .type = R.refresh_event, .code = 1 } };
    SDL_PushEvent(&e);
}

// On the main thread: the editor on the newest saved transcript, when no dictation is under way.
static void correct_last(void) {
    eh_lock();
    char *text = SDL_strdup(gs_jval_string(gs_jval_get(gs_jval_at(eh_history_list(), 0), "text"), ""));
    eh_unlock();
    char error[200];
    if (!eh_correction_window_open(text, error, sizeof error)) SDL_Log("correction editor not opened: %s", error);
    SDL_free(text);
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
    bool smoke = false, check = false, forwardable = false, open_settings = false;
    for (int i = 1; i < argc; i++) {
        if (!SDL_strcmp(argv[i], "--smoke-test")) smoke = true;
        else if (!SDL_strcmp(argv[i], "--engine-check")) check = true;
        else if (!SDL_strcmp(argv[i], "--toggle") || !SDL_strcmp(argv[i], "--pause") || !SDL_strcmp(argv[i], "--discard") || !SDL_strcmp(argv[i], "--correct-last")) forwardable = true;
        else if (!SDL_strcmp(argv[i], "--settings")) open_settings = true;
        else if (SDL_strcmp(argv[i], "--hidden")) { fprintf(stderr, "earheart: unknown option %s\n", argv[i]); return 2; }
    }
    const char *data = eh_data_dir();
    SDL_CreateDirectory(data);
    // One instance. A later one hands over what it was asked to do and leaves.
    if (!smoke && !gs_instance_acquire(data, instance_args)) {
        if (!gs_instance_forward(data, argc - 1, argv + 1)) { fprintf(stderr, "earheart: another instance is running and did not answer\n"); return 1; }
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
    // Closing the settings window is not quitting: the app lives in the tray.
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { SDL_Log("could not start: %s", SDL_GetError()); return 1; }
    R.refresh_event = SDL_RegisterEvents(1);

    eh_app_init(data, observer, eh_deliver);
    eh_lock();
    eh_settings_on_changed(settings_changed, NULL);
    // A WAV file in place of the microphone, for the end-to-end tests.
    if (SDL_getenv("EARHEART_FAKE_MIC")) eh_recorder_fake_microphone(SDL_getenv("EARHEART_FAKE_MIC"), 1);
    char error[256];
    static const eh_hotkey_backend desktop = { grab, ungrab };
    eh_hotkeys_backend(&desktop);
    R.hotkeys = gs_hotkeys_init(hotkey_fired, error, sizeof error);
    if (!R.hotkeys) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "no global hotkeys here (%s): bind a system shortcut to `earheart --toggle`", error);
    eh_unlock();
    for (int i = 1; i < argc; i++) R.hidden = R.hidden || !SDL_strcmp(argv[i], "--hidden");
    R.announce = !smoke;
    R.open_settings = open_settings;
    eh_spawn(startup, NULL);
    SDL_SetAtomicInt(&R.ready, 1);
    static const action early[] = { ACT_TOGGLE, ACT_PAUSE, ACT_DISCARD };
    for (int i = 0, seen = SDL_SetAtomicInt(&R.early, 0); i < 3; i++)
        if (seen & 1 << early[i]) spawn_action(early[i]);

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
        // Events are pumped while the tasks finish: on macOS registering a hotkey waits for this
        // thread to run it.
        while (eh_loop_busy()) {
            for (SDL_Event e; SDL_PollEvent(&e);) {}
            SDL_Delay(5);
        }
        if (check && !engine_check()) status = 1;
    } else {
        // The overlay says when it wants its next frame; until then the loop sleeps on events.
        bool settings_open = false, wizard_open = false, correction_open = false;
        for (int wait = -1;;) {
            SDL_Event e;
            // Never longer than a second: a signal to quit is noticed when events are next looked
            // at, and on macOS nothing wakes a wait for it.
            if (SDL_WaitEventTimeout(&e, wait < 0 || wait > 1000 ? 1000 : wait)) {
                do {
                    if (e.type == SDL_EVENT_QUIT) goto done;
                    if (e.type == R.refresh_event && e.user.code == 1) refresh_tray();
                    if (e.type == R.refresh_event && e.user.code == 2) eh_settings_window_open(NULL);
                    if (e.type == R.refresh_event && e.user.code == 3) eh_wizard_window_open();
                    if (e.type == R.refresh_event && e.user.code == 4) correct_last();
                    if (!eh_overlay_event(&e) && !eh_settings_window_event(&e) && !eh_wizard_window_event(&e)) eh_correction_window_event(&e);
                } while (SDL_PollEvent(&e));
            }
            wait = eh_overlay_frame();
            int windows[3] = { eh_settings_window_frame(), eh_wizard_window_frame(), eh_correction_window_frame() };
            for (int i = 0; i < 3; i++)
                if (windows[i] >= 0 && (wait < 0 || windows[i] < wait)) wait = windows[i];
            // A window one of them opened or closed is drawn at once.
            if (eh_settings_window_is_open() != settings_open || eh_wizard_window_is_open() != wizard_open || eh_correction_window_is_open() != correction_open) wait = 0;
            settings_open = eh_settings_window_is_open(), wizard_open = eh_wizard_window_is_open(), correction_open = eh_correction_window_is_open();
        }
    }
done:
    eh_correction_window_close();
    eh_wizard_window_close();
    eh_settings_window_close();
    eh_overlay_quit();
    eh_lock();
    eh_engines_stop();
    eh_unlock();
    if (R.tray) SDL_DestroyTray(R.tray);
    SDL_Quit();
    return status;
}
