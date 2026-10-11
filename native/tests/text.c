// The text helpers against the JavaScript they were ported from (tests/vectors/text.json).
#include <SDL3/SDL.h>
#include <string.h>
#include "gs_json.h"
#include "test.h"
#include "text.h"

static int differing;

static void expect(const char *what, const char *input, char *got, const char *want) {
    if (strcmp(got, want)) {
        failures++;
        if (differing++ < 12) printf("  %s differs\n    input: %s\n    want:  %s\n    got:   %s\n", what, input, want, got);
    }
    SDL_free(got);
}

void test_text(const char *vectors) {
    size_t len;
    char *text = SDL_LoadFile(vectors, &len);
    gs_json *doc = text ? gs_json_parse(text, len) : NULL;
    CHECK(doc);
    gs_jv root = gs_json_root(doc);
    int n = 0;
    for (gs_jv r = gs_json_first(gs_json_get(root, "strip")); r; r = gs_json_next(r), n++) {
        const char *input = gs_json_str(gs_json_at(r, 0), "");
        expect("fillers", input, eh_strip_fillers(input), gs_json_str(gs_json_at(r, 1), "?"));
        expect("repeats", input, eh_collapse_repeats(input), gs_json_str(gs_json_at(r, 2), "?"));
        expect("stumbles", input, eh_strip_stumbles(input), gs_json_str(gs_json_at(r, 3), "?"));
    }
    for (gs_jv r = gs_json_first(gs_json_get(root, "tails")); r; r = gs_json_next(r), n++)
        expect("tail", gs_json_str(gs_json_at(r, 0), ""), eh_residual_tail(gs_json_str(gs_json_at(r, 0), ""), gs_json_str(gs_json_at(r, 1), "")), gs_json_str(gs_json_at(r, 2), "?"));
    for (gs_jv r = gs_json_first(gs_json_get(root, "trims")); r; r = gs_json_next(r), n++)
        expect("trim", gs_json_str(gs_json_at(r, 0), ""), eh_trim(gs_json_str(gs_json_at(r, 0), "")), gs_json_str(gs_json_at(r, 1), "?"));
    if (differing > 12) printf("  and %d more\n", differing - 12);
    printf("  %d texts compared\n", n);
    gs_json_free(doc);
    SDL_free(text);
}
