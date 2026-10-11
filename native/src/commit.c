#include "commit.h"
#include <SDL3/SDL.h>
#include "settings.h"
#include "text.h"

const char *eh_displayed_output_mode(const eh_jv *settings) {
    const char *mode = eh_jv_string(eh_jv_path(settings, "output.mode"), "paste");
    if (!SDL_strcmp(mode, "paste") && !eh_jv_boolean(eh_jv_path(settings, "output.restoreClipboard"), true)) return "paste-copy";
    return !SDL_strcmp(mode, "clipboard") ? "clipboard" : !SDL_strcmp(mode, "paste-copy") ? "paste-copy" : "paste";
}

void eh_form_baseline_take(eh_form_baseline *b, const eh_jv *settings) {
    SDL_strlcpy(b->output_mode, eh_displayed_output_mode(settings), sizeof b->output_mode);
    eh_jv_free(b->dictionary);
    const eh_jv *words = eh_jv_path(settings, "cleanup.dictionary");
    b->dictionary = words ? eh_jv_clone(words) : eh_jv_array();
}

static bool has_term(const eh_jv *list, const char *term) {
    for (int i = 0; i < eh_jv_count(list); i++)
        if (!SDL_strcmp(eh_jv_string(eh_jv_at(list, i), ""), term)) return true;
    return false;
}

eh_jv *eh_vocabulary_terms(const eh_jv *terms) {
    eh_jv *out = eh_jv_array();
    for (int i = 0; i < eh_jv_count(terms); i++) {
        const eh_jv *t = eh_jv_at(terms, i);
        if (!t || t->type != EH_JV_STRING) continue;
        char *trimmed = eh_trim(t->string);
        if (*trimmed && !has_term(out, trimmed)) eh_jv_push(out, eh_jv_str(trimmed));
        SDL_free(trimmed);
    }
    return out;
}

eh_jv *eh_vocabulary_merge(const eh_jv *baseline, const eh_jv *edited, const eh_jv *live) {
    eh_jv *before = eh_vocabulary_terms(baseline), *wanted = eh_vocabulary_terms(edited), *now = eh_vocabulary_terms(live), *out = eh_jv_array();
    for (int i = 0; i < eh_jv_count(now); i++) {
        const char *term = eh_jv_string(eh_jv_at(now, i), "");
        bool removed = has_term(before, term) && !has_term(wanted, term);
        if (!removed && !has_term(out, term)) eh_jv_push(out, eh_jv_str(term));
    }
    for (int i = 0; i < eh_jv_count(wanted); i++) {
        const char *term = eh_jv_string(eh_jv_at(wanted, i), "");
        if (!has_term(before, term) && !has_term(out, term)) eh_jv_push(out, eh_jv_str(term));
    }
    eh_jv_free(before);
    eh_jv_free(wanted);
    eh_jv_free(now);
    return out;
}

eh_jv *eh_vocabulary_remember(const eh_jv *terms, const char *spelling, char *error, size_t error_size) {
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
    eh_jv *list = eh_vocabulary_terms(terms);
    if (!has_term(list, term)) {
        eh_jv_push(list, eh_jv_str(term));
        size_t joined = 0;
        for (int i = 0; i < eh_jv_count(list); i++) joined += SDL_utf8strlen(eh_jv_string(eh_jv_at(list, i), "")) + (i ? 1 : 0);
        if (eh_jv_count(list) > 256 || joined > 8192) {
            SDL_strlcpy(error, "The dictionary is full. Remove some words in Settings before adding another.", error_size);
            eh_jv_free(list);
            list = NULL;
        }
    }
    SDL_free(term);
    return list;
}

bool eh_commit_settings(eh_jv *next, const eh_form_baseline *baseline, eh_hotkey_results *results, char *error, size_t error_size) {
    static const char *const keys[EH_HK_COUNT] = { "hotkey", "pauseHotkey", "discardHotkey", "correctHotkey" };
    eh_jv *previous = eh_jv_clone(eh_settings());
    // What the app itself owns is never the form's to change.
    static const char *const owned[] = { "overlay", "customModels" };
    for (int i = 0; i < 2; i++) {
        const eh_jv *v = eh_jv_get(previous, owned[i]);
        if (v) eh_jv_set(next, owned[i], eh_jv_clone(v));
        else eh_jv_remove(next, owned[i]);
    }
    static const char *const versions[] = { "updates.skippedVersion", "updates.lastSeenVersion" };
    for (int i = 0; i < 2; i++) {
        const eh_jv *v = eh_jv_path(previous, versions[i]);
        if (v) eh_jv_set_path(next, versions[i], eh_jv_clone(v));
    }
    if (baseline) {
        // The dictionary is merged, so spellings remembered while the form was open are kept. The
        // output mode set from the tray since the form read it wins over the form's radio.
        if (baseline->dictionary) eh_jv_set_path(next, "cleanup.dictionary", eh_vocabulary_merge(baseline->dictionary, eh_jv_path(next, "cleanup.dictionary"), eh_jv_path(previous, "cleanup.dictionary")));
        if (baseline->output_mode[0] && SDL_strcmp(eh_displayed_output_mode(previous), baseline->output_mode)) {
            eh_jv_set_path(next, "output.mode", eh_jv_clone(eh_jv_path(previous, "output.mode")));
            eh_jv_set_path(next, "output.restoreClipboard", eh_jv_clone(eh_jv_path(previous, "output.restoreClipboard")));
        }
    }
    eh_hotkey_results r;
    eh_hotkeys_apply(next, &r);
    for (int n = 0; n < EH_HK_COUNT; n++)
        if (!r.r[n].ok && !r.r[n].empty) eh_jv_set(next, keys[n], eh_jv_clone(eh_jv_get(previous, keys[n])));
    bool ok = eh_settings_save(next, error, error_size);
    if (!ok) {
        // The hotkeys go back to what the file still says.
        eh_hotkey_results back;
        eh_hotkeys_apply(previous, &back);
        for (int n = 0; n < EH_HK_COUNT; n++)
            if (!back.r[n].ok && !back.r[n].empty) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "could not restore hotkeys after settings save failed: %s", back.r[n].error);
    }
    if (results) *results = r;
    eh_jv_free(previous);
    return ok;
}
