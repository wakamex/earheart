#include "commit.h"
#include <SDL3/SDL.h>
#include "settings.h"
#include "text.h"

const char *eh_displayed_output_mode(const gs_jval *settings) {
    const char *mode = gs_jval_string(gs_jval_path(settings, "output.mode"), "paste");
    if (!SDL_strcmp(mode, "paste") && !gs_jval_boolean(gs_jval_path(settings, "output.restoreClipboard"), true)) return "paste-copy";
    return !SDL_strcmp(mode, "clipboard") ? "clipboard" : !SDL_strcmp(mode, "paste-copy") ? "paste-copy" : "paste";
}

void eh_form_baseline_take(eh_form_baseline *b, const gs_jval *settings) {
    SDL_strlcpy(b->output_mode, eh_displayed_output_mode(settings), sizeof b->output_mode);
    gs_jval_free(b->dictionary);
    const gs_jval *words = gs_jval_path(settings, "cleanup.dictionary");
    b->dictionary = words ? gs_jval_clone(words) : gs_jval_array();
}

static bool has_term(const gs_jval *list, const char *term) {
    for (int i = 0; i < gs_jval_count(list); i++)
        if (!SDL_strcmp(gs_jval_string(gs_jval_at(list, i), ""), term)) return true;
    return false;
}

gs_jval *eh_vocabulary_terms(const gs_jval *terms) {
    gs_jval *out = gs_jval_array();
    for (int i = 0; i < gs_jval_count(terms); i++) {
        const gs_jval *t = gs_jval_at(terms, i);
        if (!t || gs_jval_kind(t) != GS_JSON_STRING) continue;
        char *trimmed = eh_trim(gs_jval_string(t, ""));
        if (*trimmed && !has_term(out, trimmed)) gs_jval_push(out, gs_jval_str(trimmed));
        SDL_free(trimmed);
    }
    return out;
}

gs_jval *eh_vocabulary_merge(const gs_jval *baseline, const gs_jval *edited, const gs_jval *live) {
    gs_jval *before = eh_vocabulary_terms(baseline), *wanted = eh_vocabulary_terms(edited), *now = eh_vocabulary_terms(live), *out = gs_jval_array();
    for (int i = 0; i < gs_jval_count(now); i++) {
        const char *term = gs_jval_string(gs_jval_at(now, i), "");
        bool removed = has_term(before, term) && !has_term(wanted, term);
        if (!removed && !has_term(out, term)) gs_jval_push(out, gs_jval_str(term));
    }
    for (int i = 0; i < gs_jval_count(wanted); i++) {
        const char *term = gs_jval_string(gs_jval_at(wanted, i), "");
        if (!has_term(before, term) && !has_term(out, term)) gs_jval_push(out, gs_jval_str(term));
    }
    gs_jval_free(before);
    gs_jval_free(wanted);
    gs_jval_free(now);
    return out;
}

gs_jval *eh_vocabulary_remember(const gs_jval *terms, const char *spelling, char *error, size_t error_size) {
    char *term = eh_trim(spelling ? spelling : "");
    if (!*term) {
        SDL_strlcpy(error, "Enter a spelling to remember.", error_size);
        SDL_free(term);
        return NULL;
    }
    bool control = false;
    for (const unsigned char *p = (const unsigned char *)term; *p; p++) control = control || *p < 0x20 || *p == 0x7f;
    if (SDL_utf8strlen(term) > 120 || control) {
        SDL_strlcpy(error, "Use a word or phrase of up to 120 characters on one line.", error_size);
        SDL_free(term);
        return NULL;
    }
    gs_jval *list = eh_vocabulary_terms(terms);
    if (!has_term(list, term)) {
        gs_jval_push(list, gs_jval_str(term));
        size_t joined = 0;
        for (int i = 0; i < gs_jval_count(list); i++) joined += SDL_utf8strlen(gs_jval_string(gs_jval_at(list, i), "")) + (i ? 1 : 0);
        if (gs_jval_count(list) > 256 || joined > 8192) {
            SDL_strlcpy(error, "The dictionary is full. Remove some words in Settings before adding another.", error_size);
            gs_jval_free(list);
            list = NULL;
        }
    }
    SDL_free(term);
    return list;
}

bool eh_commit_settings(gs_jval *next, const eh_form_baseline *baseline, eh_hotkey_results *results, char *error, size_t error_size) {
    static const char *const keys[EH_HK_COUNT] = { "hotkey", "pauseHotkey", "discardHotkey", "correctHotkey" };
    gs_jval *previous = gs_jval_clone(eh_settings());
    // What the app itself owns is never the form's to change.
    static const char *const owned[] = { "overlay", "customModels" };
    for (int i = 0; i < 2; i++) {
        const gs_jval *v = gs_jval_get(previous, owned[i]);
        if (v) gs_jval_set(next, owned[i], gs_jval_clone(v));
        else gs_jval_remove(next, owned[i]);
    }
    static const char *const versions[] = { "updates.skippedVersion", "updates.lastSeenVersion" };
    for (int i = 0; i < 2; i++) {
        const gs_jval *v = gs_jval_path(previous, versions[i]);
        if (v) gs_jval_set_path(next, versions[i], gs_jval_clone(v));
    }
    if (baseline) {
        // The dictionary is merged, so spellings remembered while the form was open are kept. The
        // output mode set from the tray since the form read it wins over the form's radio.
        if (baseline->dictionary) gs_jval_set_path(next, "cleanup.dictionary", eh_vocabulary_merge(baseline->dictionary, gs_jval_path(next, "cleanup.dictionary"), gs_jval_path(previous, "cleanup.dictionary")));
        if (baseline->output_mode[0] && SDL_strcmp(eh_displayed_output_mode(previous), baseline->output_mode)) {
            gs_jval_set_path(next, "output.mode", gs_jval_clone(gs_jval_path(previous, "output.mode")));
            gs_jval_set_path(next, "output.restoreClipboard", gs_jval_clone(gs_jval_path(previous, "output.restoreClipboard")));
        }
    }
    eh_hotkey_results r;
    eh_hotkeys_apply(next, &r);
    for (int n = 0; n < EH_HK_COUNT; n++)
        if (!r.r[n].ok && !r.r[n].empty) gs_jval_set(next, keys[n], gs_jval_clone(gs_jval_get(previous, keys[n])));
    bool ok = eh_settings_save(next, error, error_size);
    if (!ok) {
        // The hotkeys go back to what the file still says.
        eh_hotkey_results back;
        eh_hotkeys_apply(previous, &back);
        for (int n = 0; n < EH_HK_COUNT; n++)
            if (!back.r[n].ok && !back.r[n].empty) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "could not restore hotkeys after settings save failed: %s", back.r[n].error);
    }
    if (results) *results = r;
    gs_jval_free(previous);
    return ok;
}
