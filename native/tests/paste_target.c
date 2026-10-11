// A window to dictate into, for the end-to-end tests. It puts TEXT on the clipboard, shows a
// window and takes the keyboard; when a paste keystroke arrives it prints what the clipboard holds
// ("pasted: ..."), and a moment later what it holds then ("clipboard: ..."), which is how the tests
// see both the text delivered and the user's own clipboard coming back. Exits 0 after that, or 1
// when no paste arrived within the time given.
//   paste-target TEXT [SECONDS]
//   paste-target --hotkey          Windows: presses Ctrl+Shift+Space, the default record hotkey
//   paste-target --repeat IN.wav N OUT.wav   writes IN's audio N times over, half a second apart
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>

static int press_hotkey(void) {
    const WORD keys[] = { VK_CONTROL, VK_SHIFT, VK_SPACE };
    INPUT in[6] = { 0 };
    for (int i = 0; i < 6; i++) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = keys[i < 3 ? i : 5 - i];
        if (i >= 3) in[i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    return SendInput(6, in, sizeof(INPUT)) == 6 ? 0 : 1;
}

// Windows gives the foreground only to a process that already has it or that the user just
// used; a tap of Alt counts as use, and the test's window must be the one a paste lands in.
static void take_foreground(SDL_Window *window) {
    HWND hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}
#else
static int press_hotkey(void) { return 2; }
static void take_foreground(SDL_Window *window) { SDL_RaiseWindow(window); }
#endif

// A longer recording for the tests, from the one clip they have.
static int repeat_wav(const char *in, int times, const char *out) {
    size_t len;
    Uint8 *wav = SDL_LoadFile(in, &len);
    if (!wav || len < 44) return 1;
    Uint32 data = (Uint32)(len - 44), gap = 16000, total = (data + gap) * (Uint32)times, riff = 36 + total;
    SDL_IOStream *io = SDL_IOFromFile(out, "wb");
    if (!io) return 1;
    Uint8 head[44];
    SDL_memcpy(head, wav, 44);
    for (int i = 0; i < 4; i++) { head[4 + i] = (Uint8)(riff >> (8 * i)); head[40 + i] = (Uint8)(total >> (8 * i)); }
    Uint8 *silence = SDL_calloc(1, gap);
    bool ok = SDL_WriteIO(io, head, 44) == 44;
    for (int i = 0; ok && i < times; i++) ok = SDL_WriteIO(io, wav + 44, data) == data && SDL_WriteIO(io, silence, gap) == gap;
    return SDL_CloseIO(io) && ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && !SDL_strcmp(argv[1], "--hotkey")) return press_hotkey();
    if (argc > 4 && !SDL_strcmp(argv[1], "--repeat")) return repeat_wav(argv[2], SDL_atoi(argv[3]), argv[4]);
    const char *original = argc > 1 ? argv[1] : "the user's own clipboard";
    Uint64 deadline = SDL_GetTicks() + (Uint64)(argc > 2 ? SDL_atoi(argv[2]) : 120) * 1000, pasted_at = 0;
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "paste-target: %s\n", SDL_GetError()); return 2; }
    SDL_Window *window = SDL_CreateWindow("paste-target", 320, 120, 0);
    if (!window) { fprintf(stderr, "paste-target: %s\n", SDL_GetError()); return 2; }
    SDL_SetClipboardText(original);
    SDL_RaiseWindow(window);
    take_foreground(window);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("ready: %u\n", (unsigned)SDL_GetWindowID(window));
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) return 1;
            bool chord = e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_V && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI));
            if (chord && !pasted_at) {
                char *text = SDL_GetClipboardText();
                printf("pasted: %s\n", text ? text : "");
                SDL_free(text);
                pasted_at = SDL_GetTicks();
            }
        }
        // The app puts the old contents back a second after its keystroke.
        if (pasted_at && SDL_GetTicks() - pasted_at > 2500) {
            char *text = SDL_GetClipboardText();
            printf("clipboard: %s\n", text ? text : "");
            SDL_free(text);
            return 0;
        }
        if (SDL_GetTicks() > deadline) { fprintf(stderr, "paste-target: no paste arrived\n"); return 1; }
        SDL_Delay(10);
    }
}
