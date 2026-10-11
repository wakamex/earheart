// The hotkey manager against the JavaScript it was ported from: tools/gen-hotkey-vectors.js ran
// main/hotkeys.js through random sequences of changes with a stand-in for the system's key grabs,
// and each step is replayed here with the same stand-in. After every step the outcome reported
// for each hotkey and the set of keys grabbed must be the same.
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include "gs_json.h"
#include "hotkeys.h"
#include "test.h"

#define MAX_KEYS 16
static char held[MAX_KEYS][64], taken[MAX_KEYS][64];
static const char *invalid, *invalid_message;

static bool in(char set[MAX_KEYS][64], const char *key) {
    for (int i = 0; i < MAX_KEYS; i++)
        if (set[i][0] && !strcmp(set[i], key)) return true;
    return false;
}

static int fake_add(const char *accelerator, int name, char *error, size_t size) {
    if (!strcmp(accelerator, invalid)) { SDL_strlcpy(error, invalid_message, size); return 2; }
    if (in(taken, accelerator) || in(held, accelerator)) return 1;
    for (int i = 0; i < MAX_KEYS; i++)
        if (!held[i][0]) { SDL_strlcpy(held[i], accelerator, 64); return 0; }
    return 1;
}

static void fake_remove(const char *accelerator) {
    for (int i = 0; i < MAX_KEYS; i++)
        if (!strcmp(held[i], accelerator)) held[i][0] = 0;
}

static int by_name(const void *a, const void *b) { return strcmp(a, b); }

void test_hotkeys(const char *vectors) {
    size_t len = 0;
    char *text = SDL_LoadFile(vectors, &len);
    gs_json *doc = text ? gs_json_parse(text, len) : NULL;
    if (!doc) { printf("  cannot read %s\n", vectors); failures++; SDL_free(text); return; }
    gs_jv root = gs_json_root(doc), cases = gs_json_get(root, "cases");
    invalid = gs_json_str(gs_json_get(root, "invalid"), "");
    invalid_message = gs_json_str(gs_json_get(root, "invalidMessage"), "");
    static const eh_hotkey_backend fake = { fake_add, fake_remove };
    eh_hotkeys_backend(&fake);
    int steps_run = 0, wrong = 0;
    for (int c = 0; c < gs_json_count(cases) && wrong < 5; c++) {
        gs_jv steps = gs_json_at(cases, c);
        eh_hotkeys_reset();
        memset(held, 0, sizeof held);
        memset(taken, 0, sizeof taken);
        for (int s = 0; s < gs_json_count(steps) && wrong < 5; s++, steps_run++) {
            gs_jv step = gs_json_at(steps, s), apply = gs_json_get(step, "apply"), arm = gs_json_get(step, "arm"), suspend = gs_json_get(step, "suspend"), other = gs_json_get(step, "taken");
            bool same = true;
            if (apply) {
                eh_hotkey_result out[EH_HK_COUNT];
                eh_hotkeys_apply_raw(gs_json_str(gs_json_at(apply, 0), ""), gs_json_str(gs_json_at(apply, 1), ""), gs_json_str(gs_json_at(apply, 2), ""), gs_json_str(gs_json_at(apply, 3), ""), out);
                gs_jv results = gs_json_get(step, "results");
                for (int n = 0; n < EH_HK_COUNT; n++) {
                    gs_jv r = gs_json_at(results, n);
                    bool ok = gs_json_num(gs_json_at(r, 0), 0) != 0, empty = gs_json_num(gs_json_at(r, 1), 0) != 0, unbound = gs_json_num(gs_json_at(r, 2), 0) != 0;
                    const char *error = gs_json_str(gs_json_at(r, 3), "");
                    if (out[n].ok != ok || out[n].empty != empty || out[n].unbound != unbound || strcmp(out[n].error, error)) {
                        same = false;
                        printf("  case %d step %d hotkey %d: got ok=%d empty=%d \"%s\", expected ok=%d empty=%d \"%s\"\n", c, s, n, out[n].ok, out[n].empty, out[n].error, ok, empty, error);
                    }
                }
            } else if (arm) {
                eh_hotkeys_arm_discard(gs_json_num(arm, 0) != 0);
            } else if (suspend) {
                int lost = eh_hotkeys_suspend(gs_json_num(suspend, 0) != 0);
                if (lost != (int)gs_json_num(gs_json_get(step, "lost"), 0)) same = false, printf("  case %d step %d: %d hotkeys lost on resume\n", c, s, lost);
            } else if (other) {
                memset(taken, 0, sizeof taken);
                for (int i = 0; i < gs_json_count(other) && i < MAX_KEYS; i++) SDL_strlcpy(taken[i], gs_json_str(gs_json_at(other, i), ""), 64);
            }
            // The keys grabbed, in order.
            char now[MAX_KEYS][64];
            int n = 0;
            for (int i = 0; i < MAX_KEYS; i++)
                if (held[i][0]) SDL_strlcpy(now[n++], held[i], 64);
            qsort(now, (size_t)n, 64, by_name);
            gs_jv expect = gs_json_get(step, "held");
            bool held_same = n == gs_json_count(expect);
            for (int i = 0; held_same && i < n; i++) held_same = !strcmp(now[i], gs_json_str(gs_json_at(expect, i), ""));
            if (!held_same) {
                same = false;
                printf("  case %d step %d: %d keys grabbed, expected %d\n", c, s, n, gs_json_count(expect));
            }
            if (!same) wrong++, failures++;
        }
    }
    printf("  %d steps of %d sequences compared\n", steps_run, gs_json_count(cases));
    eh_hotkeys_backend(NULL);
    eh_hotkeys_reset();
    gs_json_free(doc);
    SDL_free(text);
}
