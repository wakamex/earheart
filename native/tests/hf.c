// The Hugging Face model lookup against main/services/hf-models.js, on the addresses and
// repositories of tools/gen-hf-vectors.js: what an address parses to, the versions a repository's
// files make and their order, the model definition for each, and every refusal's message.
#include <SDL3/SDL.h>
#include <string.h>
#include "files.h"
#include "hf.h"
#include "test.h"

static void show(const char *what, const gs_jval *got, const gs_jval *want) {
    char *a = got ? eh_json_text(got, false) : SDL_strdup("(none)"), *b = want ? eh_json_text(want, false) : SDL_strdup("(none)");
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
    gs_jval *doc = text ? gs_jval_parse(text, len) : NULL;
    if (!doc) { printf("  cannot read %s\n", vectors); failures++; SDL_free(text); return; }
    int wrong = 0;
    const gs_jval *parses = gs_jval_get(doc, "parses"), *repos = gs_jval_get(doc, "repos");
    for (int i = 0; i < gs_jval_count(parses); i++) {
        const gs_jval *c = gs_jval_at(parses, i), *out = gs_jval_get(c, "out");
        const char *input = gs_jval_string(gs_jval_get(c, "input"), "");
        eh_hf_repo r;
        char error[400] = "";
        bool ok = eh_hf_parse(input, &r, error, sizeof error);
        bool same = ok == (out != NULL);
        if (same && ok) same = !strcmp(r.owner, gs_jval_string(gs_jval_get(out, "owner"), "")) && !strcmp(r.repo, gs_jval_string(gs_jval_get(out, "repo"), "")) && !strcmp(r.ref, gs_jval_string(gs_jval_get(out, "ref"), ""));
        if (same && !ok) same = !strcmp(error, gs_jval_string(gs_jval_get(c, "error"), ""));
        if (!same) wrong++, failures++, printf("  address \"%s\": got %s/%s@%s \"%s\"\n", input, r.owner, r.repo, r.ref, error);
    }
    int usable = 0;
    for (int i = 0; i < gs_jval_count(repos) && wrong < 6; i++) {
        const gs_jval *c = gs_jval_at(repos, i), *want = gs_jval_get(c, "out"), *status = gs_jval_get(c, "status");
        eh_model_kind kind = !strcmp(gs_jval_string(gs_jval_get(c, "kind"), ""), "cleanup") ? EH_MODEL_CLEANUP : EH_MODEL_STT;
        eh_hf_repo r = { 0 };
        SDL_strlcpy(r.owner, gs_jval_string(gs_jval_get(c, "owner"), ""), sizeof r.owner);
        SDL_strlcpy(r.repo, gs_jval_string(gs_jval_get(c, "repo"), ""), sizeof r.repo);
        SDL_strlcpy(r.ref, gs_jval_string(gs_jval_get(c, "ref"), ""), sizeof r.ref);
        char error[1400] = "";
        gs_jval *got = eh_hf_listing(kind, &r, (int)gs_jval_number(gs_jval_get(status, "info"), 200), gs_jval_get(c, "info"), (int)gs_jval_number(gs_jval_get(status, "tree"), 200), gs_jval_get(c, "tree"), error, sizeof error);
        if (!want) {
            const char *expected = gs_jval_string(gs_jval_get(c, "error"), "");
            if (got || strcmp(error, expected)) wrong++, failures++, printf("  repository %d (%s/%s): refusal differs:\n    got      %s\n    expected %s\n", i, r.owner, r.repo, got ? "(a listing)" : error, expected);
        } else if (!got || !gs_jval_equal(got, want)) {
            wrong++, failures++;
            if (got) show("listing", got, want);
            else printf("  repository %d (%s/%s) refused: %s\n", i, r.owner, r.repo, error);
        } else {
            usable++;
            const gs_jval *variants = gs_jval_get(got, "variants"), *models = gs_jval_get(c, "models");
            for (int v = 0; v < gs_jval_count(variants); v++) {
                gs_jval *model = eh_hf_model(kind, gs_jval_string(gs_jval_get(got, "repo"), ""), gs_jval_at(variants, v));
                if (!gs_jval_equal(model, gs_jval_at(models, v))) wrong++, failures++, show("model", model, gs_jval_at(models, v));
                gs_jval_free(model);
            }
        }
        gs_jval_free(got);
    }
    CHECK(!strcmp(eh_hf_search_url(EH_MODEL_CLEANUP), gs_jval_string(gs_jval_path(doc, "search.cleanup"), "")));
    CHECK(!strcmp(eh_hf_search_url(EH_MODEL_STT), gs_jval_string(gs_jval_path(doc, "search.stt"), "")));
    printf("  %d addresses and %d repositories compared, %d of them usable\n", gs_jval_count(parses), gs_jval_count(repos), usable);
    gs_jval_free(doc);
    SDL_free(text);
}
