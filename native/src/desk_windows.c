// Windows: RegisterHotKey on a thread with a message queue of its own, SendInput for the paste
// keystroke, and a named pipe for the one running instance.
#include <SDL3/SDL.h>
#include <windows.h>
#include "desk.h"

/* ---------------- hotkeys ---------------- */

#define WM_EH_REGISTER (WM_APP + 1)
#define WM_EH_UNREGISTER (WM_APP + 2)

typedef struct { int id; UINT mods, vk; BOOL ok; DWORD error; HANDLE done; } hotkey_request;

static struct {
    DWORD thread_id;
    HANDLE ready;
    void (*fired)(int id);
} W;

// Hotkeys belong to the thread that registered them, and their messages arrive in its queue.
static int hotkey_thread(void *user) {
    MSG msg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);  // makes the queue exist
    W.thread_id = GetCurrentThreadId();
    SetEvent(W.ready);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            W.fired((int)msg.wParam);
        } else if (msg.message == WM_EH_REGISTER || msg.message == WM_EH_UNREGISTER) {
            hotkey_request *r = (hotkey_request *)msg.lParam;
            UnregisterHotKey(NULL, r->id);
            r->ok = msg.message == WM_EH_UNREGISTER || RegisterHotKey(NULL, r->id, r->mods | MOD_NOREPEAT, r->vk);
            r->error = GetLastError();
            SetEvent(r->done);
        }
    }
    return 0;
}

bool eh_hotkeys_init(void (*fired)(int id), char *error, size_t error_size) {
    if (W.thread_id) return true;
    W.fired = fired;
    W.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    SDL_DetachThread(SDL_CreateThread(hotkey_thread, "hotkeys", NULL));
    WaitForSingleObject(W.ready, INFINITE);
    return true;
}

static UINT vk_of(const char *key) {
    static const struct { const char *name; UINT vk; } named[] = {
        { "space", VK_SPACE }, { "tab", VK_TAB }, { "enter", VK_RETURN }, { "return", VK_RETURN }, { "escape", VK_ESCAPE }, { "esc", VK_ESCAPE },
        { "backspace", VK_BACK }, { "delete", VK_DELETE }, { "insert", VK_INSERT }, { "home", VK_HOME }, { "end", VK_END },
        { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT }, { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
        { "plus", VK_OEM_PLUS }, { "capslock", VK_CAPITAL }, { "numlock", VK_NUMLOCK }, { "scrolllock", VK_SCROLL }, { "printscreen", VK_SNAPSHOT },
        { "numdec", VK_DECIMAL }, { "numadd", VK_ADD }, { "numsub", VK_SUBTRACT }, { "nummult", VK_MULTIPLY }, { "numdiv", VK_DIVIDE },
    };
    size_t n = SDL_strlen(key);
    if (n == 1 && key[0] >= 'a' && key[0] <= 'z') return (UINT)(key[0] - 'a' + 'A');
    if (n == 1 && key[0] >= '0' && key[0] <= '9') return (UINT)key[0];
    if (n == 1) {  // a punctuation mark: whichever key types it on this layout
        SHORT scan = VkKeyScanW((WCHAR)key[0]);
        return scan == -1 ? 0 : (UINT)(scan & 0xff);
    }
    if (key[0] == 'f' && SDL_isdigit((unsigned char)key[1])) return VK_F1 + (UINT)SDL_atoi(key + 1) - 1;
    if (!SDL_strncmp(key, "num", 3) && SDL_isdigit((unsigned char)key[3]) && n == 4) return VK_NUMPAD0 + (UINT)(key[3] - '0');
    for (size_t i = 0; i < SDL_arraysize(named); i++)
        if (!SDL_strcmp(key, named[i].name)) return named[i].vk;
    return 0;
}

static bool ask(UINT message, hotkey_request *r) {
    r->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    bool sent = PostThreadMessageW(W.thread_id, message, 0, (LPARAM)r);
    if (sent) WaitForSingleObject(r->done, INFINITE);
    CloseHandle(r->done);
    return sent && r->ok;
}

bool eh_hotkey_register(int id, const eh_accelerator *a, char *error, size_t error_size) {
    hotkey_request r = { .id = id, .vk = vk_of(a->key) };
    r.mods = (a->mods & EH_MOD_CTRL ? MOD_CONTROL : 0) | (a->mods & EH_MOD_SHIFT ? MOD_SHIFT : 0) | (a->mods & EH_MOD_ALT ? MOD_ALT : 0) | (a->mods & EH_MOD_SUPER ? MOD_WIN : 0);
    if (!r.vk) { SDL_strlcpy(error, "this keyboard has no such key", error_size); return false; }
    if (ask(WM_EH_REGISTER, &r)) return true;
    SDL_strlcpy(error, "already in use by another app", error_size);
    return false;
}

void eh_hotkey_unregister(int id) {
    hotkey_request r = { .id = id };
    if (W.thread_id) ask(WM_EH_UNREGISTER, &r);
}

/* ---------------- paste ---------------- */

bool eh_paste_permitted(void) { return true; }
void eh_paste_ask_permission(void) {}

eh_paste_result eh_paste_keystroke(char *detail, size_t detail_size) {
    INPUT in[4] = { 0 };
    for (int i = 0; i < 4; i++) in[i].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = 'V';
    in[2].ki.wVk = 'V';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(4, in, sizeof(INPUT)) == 4) return EH_PASTE_SENT;
    // Refused when the focused window belongs to a program running with higher privileges.
    SDL_snprintf(detail, detail_size, "SendInput failed with error %lu", (unsigned long)GetLastError());
    return EH_PASTE_FAILED;
}

/* ---------------- one instance ---------------- */

static void (*on_args)(int argc, char **argv);
static char pipe_name[160];

static void name_pipe(const char *data_dir) {
    // One instance per data folder; the name carries a hash of it.
    unsigned hash = 2166136261u;
    for (const char *p = data_dir; *p; p++) hash = (hash ^ (unsigned char)*p) * 16777619u;
    SDL_snprintf(pipe_name, sizeof pipe_name, "\\\\.\\pipe\\earheart-native-%08x", hash);
}

static HANDLE open_pipe(bool first) {
    return CreateNamedPipeA(pipe_name, PIPE_ACCESS_INBOUND | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0), PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, 0, 4096, 0, NULL);
}

static int listener(void *user) {
    HANDLE pipe = (HANDLE)user;
    for (;;) {
        if (ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buf[4096];
            DWORD len = 0, got;
            while (len < sizeof buf - 1 && ReadFile(pipe, buf + len, (DWORD)(sizeof buf - 1 - len), &got, NULL) && got) len += got;
            buf[len] = 0;
            char *argv[32];
            int argc = 0;
            for (char *p = buf; p < buf + len && argc < 32; p += SDL_strlen(p) + 1) argv[argc++] = p;
            if (argc) on_args(argc, argv);
        }
        DisconnectNamedPipe(pipe);
    }
}

bool eh_instance_forward(const char *data_dir, int argc, char **argv) {
    name_pipe(data_dir);
    HANDLE pipe = CreateFileA(pipe_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (int i = 0; ok && i < argc; i++) {
        DWORD wrote;
        ok = WriteFile(pipe, argv[i], (DWORD)SDL_strlen(argv[i]) + 1, &wrote, NULL);
    }
    CloseHandle(pipe);
    return ok;
}

bool eh_instance_acquire(const char *data_dir, void (*args)(int argc, char **argv)) {
    name_pipe(data_dir);
    HANDLE pipe = open_pipe(true);
    if (pipe == INVALID_HANDLE_VALUE) return GetLastError() != ERROR_ACCESS_DENIED;  // taken: an instance is running
    on_args = args;
    SDL_DetachThread(SDL_CreateThread(listener, "instance", pipe));
    return true;
}

/* ---------------- the clipboard's contents ---------------- */

struct eh_clip { UINT *formats; void **data; SIZE_T *sizes; int count; };

void eh_clipboard_free(eh_clip *s) {
    if (!s) return;
    for (int i = 0; i < s->count; i++) SDL_free(s->data[i]);
    SDL_free(s->formats);
    SDL_free(s->data);
    SDL_free(s->sizes);
    SDL_free(s);
}

// Formats whose handle is a block of memory. The others are objects of the drawing system (a
// bitmap, a palette, a metafile) that Windows makes again from the memory formats beside them.
static bool is_memory(UINT format) {
    return format != CF_BITMAP && format != CF_METAFILEPICT && format != CF_PALETTE && format != CF_ENHMETAFILE && format != CF_OWNERDISPLAY &&
           !(format >= CF_DSPTEXT && format <= CF_DSPENHMETAFILE) && !(format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST);
}

static bool open_clipboard(void) {
    for (int i = 0; i < 10; i++) {  // another program may hold it for a moment
        if (OpenClipboard(NULL)) return true;
        Sleep(10);
    }
    return false;
}

eh_clip *eh_clipboard_snapshot(void) {
    if (!open_clipboard()) return NULL;
    eh_clip *s = SDL_calloc(1, sizeof *s);
    int n = CountClipboardFormats();
    s->formats = SDL_calloc((size_t)n + 1, sizeof *s->formats);
    s->data = SDL_calloc((size_t)n + 1, sizeof *s->data);
    s->sizes = SDL_calloc((size_t)n + 1, sizeof *s->sizes);
    for (UINT f = EnumClipboardFormats(0); f && s->count < n; f = EnumClipboardFormats(f)) {
        if (!is_memory(f)) continue;
        HANDLE h = GetClipboardData(f);
        SIZE_T size = h ? GlobalSize(h) : 0;
        void *p = size ? GlobalLock(h) : NULL;
        if (!p) continue;
        s->data[s->count] = SDL_malloc(size);
        SDL_memcpy(s->data[s->count], p, size);
        GlobalUnlock(h);
        s->formats[s->count] = f;
        s->sizes[s->count++] = size;
    }
    CloseClipboard();
    if (!s->count) { eh_clipboard_free(s); return NULL; }
    return s;
}

bool eh_clipboard_restore(eh_clip *s) {
    bool ok = open_clipboard();
    if (ok) {
        EmptyClipboard();
        for (int i = 0; i < s->count; i++) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s->sizes[i]);
            void *p = h ? GlobalLock(h) : NULL;
            if (!p) continue;
            SDL_memcpy(p, s->data[i], s->sizes[i]);
            GlobalUnlock(h);
            if (!SetClipboardData(s->formats[i], h)) GlobalFree(h);  // the clipboard owns what it accepts
        }
        CloseClipboard();
    }
    eh_clipboard_free(s);
    return ok;
}
