// The dictionary's rules against renderer/vocabulary.js, on the lists of
// tools/gen-vocabulary-vectors.js.
#include <SDL3/SDL.h>
#include <string.h>
#include "commit.h"
#include "test.h"

static bool same_list(const eh_jv *a, const eh_jv *b) {
    if (eh_jv_count(a) != eh_jv_count(b)) return false;
    for (int i = 0; i < eh_jv_count(a); i++)
        if (strcmp(eh_jv_string(eh_jv_at(a, i), ""), eh_jv_string(eh_jv_at(b, i), ""))) return false;
    return true;
}

void test_vocabulary(const char *vectors) {
    size_t len = 0;
    char *text = SDL_LoadFile(vectors, &len);
    eh_jv *doc = text ? eh_jv_parse(text, len) : NULL;
    if (!doc) { printf("  cannot read %s\n", vectors); failures++; SDL_free(text); return; }
    const eh_jv *merges = eh_jv_get(doc, "merges"), *remembers = eh_jv_get(doc, "remembers");
    int wrong = 0;
    for (int i = 0; i < eh_jv_count(merges) && wrong < 5; i++) {
        const eh_jv *c = eh_jv_at(merges, i);
        eh_jv *terms = eh_vocabulary_terms(eh_jv_get(c, "edited"));
        eh_jv *merged = eh_vocabulary_merge(eh_jv_get(c, "baseline"), eh_jv_get(c, "edited"), eh_jv_get(c, "live"));
        if (!same_list(terms, eh_jv_get(c, "terms")) || !same_list(merged, eh_jv_get(c, "merged"))) wrong++, failures++, printf("  merge %d differs\n", i);
        eh_jv_free(terms);
        eh_jv_free(merged);
    }
    for (int i = 0; i < eh_jv_count(remembers) && wrong < 5; i++) {
        const eh_jv *c = eh_jv_at(remembers, i), *expect = eh_jv_get(c, "out");
        char error[200] = "";
        eh_jv *out = eh_vocabulary_remember(eh_jv_get(c, "terms"), eh_jv_string(eh_jv_get(c, "spelling"), ""), error, sizeof error);
        bool want_list = expect && expect->type == EH_JV_ARRAY;
        if ((out != NULL) != want_list || (out && !same_list(out, expect)) || (!out && strcmp(error, eh_jv_string(eh_jv_get(c, "error"), "")))) {
            wrong++, failures++;
            printf("  remembered spelling %d differs: \"%s\"\n", i, error);
        }
        eh_jv_free(out);
    }
    printf("  %d merges and %d remembered spellings compared\n", eh_jv_count(merges), eh_jv_count(remembers));
    eh_jv_free(doc);
    SDL_free(text);
}
