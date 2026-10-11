// Shows what the clipboard offers and whether a snapshot of it survives being put back: each
// format with its size, then the text after writing something else and restoring.
//   clipboard-probe
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>

typedef struct { char **types; void **data; size_t *sizes; size_t count; } snapshot;

static const void *offer(void *user, const char *mime, size_t *size) {
    snapshot *s = user;
    for (size_t i = 0; i < s->count; i++)
        if (!SDL_strcmp(s->types[i], mime)) { *size = s->sizes[i]; return s->data[i]; }
    *size = 0;
    return NULL;
}

int main(int argc, char **argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "%s\n", SDL_GetError()); return 2; }
    SDL_Window *w = SDL_CreateWindow("clipboard-probe", 200, 80, SDL_WINDOW_HIDDEN);
    (void)w;
    snapshot s = { 0 };
    size_t n = 0;
    char **types = SDL_GetClipboardMimeTypes(&n);
    char *before = SDL_GetClipboardText();
    printf("text before: \"%s\"\n", before);
    s.types = SDL_calloc(n + 1, sizeof *s.types);
    s.data = SDL_calloc(n + 1, sizeof *s.data);
    s.sizes = SDL_calloc(n + 1, sizeof *s.sizes);
    for (size_t i = 0; i < n; i++) {
        size_t size = 0;
        void *data = SDL_GetClipboardData(types[i], &size);
        printf("format %s: %zu bytes\n", types[i], size);
        if (!data || !size) continue;
        s.types[s.count] = SDL_strdup(types[i]);
        s.data[s.count] = data;
        s.sizes[s.count++] = size;
    }
    SDL_SetClipboardText("something else");
    for (int i = 0; i < 20; i++) { SDL_PumpEvents(); SDL_Delay(10); }
    bool ok = SDL_SetClipboardData(offer, NULL, &s, (const char **)s.types, s.count);
    for (int i = 0; i < 20; i++) { SDL_PumpEvents(); SDL_Delay(10); }
    char *after = SDL_GetClipboardText();
    printf("restore %s; text after: \"%s\"\n", ok ? "accepted" : SDL_GetError(), after);
    int same = !SDL_strcmp(before, after);
    // Stay the clipboard's owner long enough for another program to read it.
    for (int i = 0; i < (argc > 1 ? SDL_atoi(argv[1]) * 100 : 0); i++) { SDL_PumpEvents(); SDL_Delay(10); }
    return same ? 0 : 1;
}
