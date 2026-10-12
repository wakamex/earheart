// The Hugging Face model lookup against main/services/hf-models.js, on the addresses and
// repositories of tools/gen-hf-vectors.js: what an address parses to, the versions a repository's
// files make and their order, the model definition for each, and every refusal's message.
#include <SDL3/SDL.h>
#include <string.h>
#include "hf.h"
#include "test.h"

static void show(const char *what, const eh_jv *got, const eh_jv *want) {
    char *a = got ? eh_jv_write(got, false) : SDL_strdup("(none)"), *b = want ? eh_jv_write(want, false) : SDL_strdup("(none)");
    size_t at = 0;
    while (a[at] && a[at] == b[at]) at++;
    size_t from = at > 60 ? at - 60 : 0;
    printf("  %s differs at %zu:\n    got      …%.160s\n    expected …%.160s\n", what, at, a + from, b + from);
    SDL_free(a);
    SDL_free(b);
}

void test_hf(const char *vectors) {
    size_t len = 0;
    char *text = SDL_LoadFile(vectors, &len);
    eh_jv *doc = text ? eh_jv_parse(text, len) : NULL;
    if (!doc) { printf("  cannot read %s\n", vectors); failures++; SDL_free(text); return; }
    int wrong = 0;
    const eh_jv *parses = eh_jv_get(doc, "parses"), *repos = eh_jv_get(doc, "repos");
    for (int i = 0; i < eh_jv_count(parses); i++) {
        const eh_jv *c = eh_jv_at(parses, i), *out = eh_jv_get(c, "out");
        const char *input = eh_jv_string(eh_jv_get(c, "input"), "");
        eh_hf_repo r;
        char error[400] = "";
        bool ok = eh_hf_parse(input, &r, error, sizeof error);
        bool same = ok == (out != NULL);
        if (same && ok) same = !strcmp(r.owner, eh_jv_string(eh_jv_get(out, "owner"), "")) && !strcmp(r.repo, eh_jv_string(eh_jv_get(out, "repo"), "")) && !strcmp(r.ref, eh_jv_string(eh_jv_get(out, "ref"), ""));
        if (same && !ok) same = !strcmp(error, eh_jv_string(eh_jv_get(c, "error"), ""));
        if (!same) wrong++, failures++, printf("  address \"%s\": got %s/%s@%s \"%s\"\n", input, r.owner, r.repo, r.ref, error);
    }
    int usable = 0;
    for (int i = 0; i < eh_jv_count(repos) && wrong < 6; i++) {
        const eh_jv *c = eh_jv_at(repos, i), *want = eh_jv_get(c, "out"), *status = eh_jv_get(c, "status");
        eh_model_kind kind = !strcmp(eh_jv_string(eh_jv_get(c, "kind"), ""), "cleanup") ? EH_MODEL_CLEANUP : EH_MODEL_STT;
        eh_hf_repo r = { 0 };
        SDL_strlcpy(r.owner, eh_jv_string(eh_jv_get(c, "owner"), ""), sizeof r.owner);
        SDL_strlcpy(r.repo, eh_jv_string(eh_jv_get(c, "repo"), ""), sizeof r.repo);
        SDL_strlcpy(r.ref, eh_jv_string(eh_jv_get(c, "ref"), ""), sizeof r.ref);
        char error[1400] = "";
        eh_jv *got = eh_hf_listing(kind, &r, (int)eh_jv_number(eh_jv_get(status, "info"), 200), eh_jv_get(c, "info"), (int)eh_jv_number(eh_jv_get(status, "tree"), 200), eh_jv_get(c, "tree"), error, sizeof error);
        if (!want) {
            const char *expected = eh_jv_string(eh_jv_get(c, "error"), "");
            if (got || strcmp(error, expected)) wrong++, failures++, printf("  repository %d (%s/%s): refusal differs:\n    got      %s\n    expected %s\n", i, r.owner, r.repo, got ? "(a listing)" : error, expected);
        } else if (!got || !eh_jv_equal(got, want)) {
            wrong++, failures++;
            if (got) show("listing", got, want);
            else printf("  repository %d (%s/%s) refused: %s\n", i, r.owner, r.repo, error);
        } else {
            usable++;
            const eh_jv *variants = eh_jv_get(got, "variants"), *models = eh_jv_get(c, "models");
            for (int v = 0; v < eh_jv_count(variants); v++) {
                eh_jv *model = eh_hf_model(kind, eh_jv_string(eh_jv_get(got, "repo"), ""), eh_jv_at(variants, v));
                if (!eh_jv_equal(model, eh_jv_at(models, v))) wrong++, failures++, show("model", model, eh_jv_at(models, v));
                eh_jv_free(model);
            }
        }
        eh_jv_free(got);
    }
    CHECK(!strcmp(eh_hf_search_url(EH_MODEL_CLEANUP), eh_jv_string(eh_jv_path(doc, "search.cleanup"), "")));
    CHECK(!strcmp(eh_hf_search_url(EH_MODEL_STT), eh_jv_string(eh_jv_path(doc, "search.stt"), "")));
    printf("  %d addresses and %d repositories compared, %d of them usable\n", eh_jv_count(parses), eh_jv_count(repos), usable);
    eh_jv_free(doc);
    SDL_free(text);
}
