// A window to dictate into, for the end-to-end tests. It puts TEXT on the clipboard, shows a
// window and takes the keyboard; when a paste keystroke arrives it prints what the clipboard holds
// ("pasted: ..."), and a moment later what it holds then ("clipboard: ..."), which is how the tests
// see both the text delivered and the user's own clipboard coming back. Exits 0 after that, or 1
// when no paste arrived within the time given.
//   paste-target TEXT [SECONDS]
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>

int main(int argc, char **argv) {
    const char *original = argc > 1 ? argv[1] : "the user's own clipboard";
    Uint64 deadline = SDL_GetTicks() + (Uint64)(argc > 2 ? SDL_atoi(argv[2]) : 120) * 1000, pasted_at = 0;
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "paste-target: %s\n", SDL_GetError()); return 2; }
    SDL_Window *window = SDL_CreateWindow("paste-target", 320, 120, 0);
    if (!window) { fprintf(stderr, "paste-target: %s\n", SDL_GetError()); return 2; }
    SDL_SetClipboardText(original);
    SDL_RaiseWindow(window);
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
