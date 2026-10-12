// The dictionary's rules against renderer/vocabulary.js, on the lists of
// tools/gen-vocabulary-vectors.js.
#include <SDL3/SDL.h>
#include <string.h>
#include "commit.h"
#include "test.h"

static bool same_list(const gs_jval *a, const gs_jval *b) {
    if (gs_jval_count(a) != gs_jval_count(b)) return false;
    for (int i = 0; i < gs_jval_count(a); i++)
        if (strcmp(gs_jval_string(gs_jval_at(a, i), ""), gs_jval_string(gs_jval_at(b, i), ""))) return false;
    return true;
}

void test_vocabulary(const char *vectors) {
    size_t len = 0;
    char *text = SDL_LoadFile(vectors, &len);
    gs_jval *doc = text ? gs_jval_parse(text, len) : NULL;
    if (!doc) { printf("  cannot read %s\n", vectors); failures++; SDL_free(text); return; }
    const gs_jval *merges = gs_jval_get(doc, "merges"), *remembers = gs_jval_get(doc, "remembers");
    int wrong = 0;
    for (int i = 0; i < gs_jval_count(merges) && wrong < 5; i++) {
        const gs_jval *c = gs_jval_at(merges, i);
        gs_jval *terms = eh_vocabulary_terms(gs_jval_get(c, "edited"));
        gs_jval *merged = eh_vocabulary_merge(gs_jval_get(c, "baseline"), gs_jval_get(c, "edited"), gs_jval_get(c, "live"));
        if (!same_list(terms, gs_jval_get(c, "terms")) || !same_list(merged, gs_jval_get(c, "merged"))) wrong++, failures++, printf("  merge %d differs\n", i);
        gs_jval_free(terms);
        gs_jval_free(merged);
    }
    for (int i = 0; i < gs_jval_count(remembers) && wrong < 5; i++) {
        const gs_jval *c = gs_jval_at(remembers, i), *expect = gs_jval_get(c, "out");
        char error[200] = "";
        gs_jval *out = eh_vocabulary_remember(gs_jval_get(c, "terms"), gs_jval_string(gs_jval_get(c, "spelling"), ""), error, sizeof error);
        bool want_list = expect && gs_jval_kind(expect) == GS_JSON_ARRAY;
        if ((out != NULL) != want_list || (out && !same_list(out, expect)) || (!out && strcmp(error, gs_jval_string(gs_jval_get(c, "error"), "")))) {
            wrong++, failures++;
            printf("  remembered spelling %d differs: \"%s\"\n", i, error);
        }
        gs_jval_free(out);
    }
    printf("  %d merges and %d remembered spellings compared\n", gs_jval_count(merges), gs_jval_count(remembers));
    gs_jval_free(doc);
    SDL_free(text);
}
