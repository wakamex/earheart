// The splitter and the speech probe against the JavaScript they were ported from: each recording
// in tests/vectors/audio.json is rebuilt from its seed and every result compared exactly.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "gs_json.h"
#include "gs_rand.h"
#include "test.h"

static double js_round(double x) { return floor(x + 0.5); }

static float *render(gs_jv parts, int rate, uint32_t seed, int *count) {
    gs_mulberry rng = { seed ^ 0x5eed };
    int total = 0;
    for (gs_jv p = gs_json_first(parts); p; p = gs_json_next(p)) total += (int)js_round(gs_json_num(gs_json_at(p, 0), 0) * rate);
    float *s = SDL_malloc(sizeof(float) * (size_t)(total + 1));
    int at = 0;
    for (gs_jv p = gs_json_first(parts); p; p = gs_json_next(p)) {
        int n = (int)js_round(gs_json_num(gs_json_at(p, 0), 0) * rate);
        double amp = gs_json_num(gs_json_at(p, 1), 0);
        for (int i = 0; i < n; i++) s[at++] = (float)((gs_mulberry_next(&rng) * 2 - 1) * amp);
    }
    *count = total;
    return s;
}

static uint32_t fingerprint(const float *s, int count) {
    uint32_t sum = 0, bits;
    for (int i = 0; i < count; i++) { memcpy(&bits, &s[i], 4); sum += bits; }
    return sum;
}

static double option(gs_jv opts, const char *key, double fallback) {
    gs_jv v = gs_json_get(opts, key);
    if (!v) return fallback;
    return gs_json_kind(v) == GS_JSON_STRING ? INFINITY : gs_json_num(v, fallback);
}

static bool same_ints(const int *got, int n, gs_jv want) {
    if (n != gs_json_count(want)) return false;
    for (int i = 0; i < n; i++)
        if (got[i] != (int)gs_json_num(gs_json_at(want, i), -1)) return false;
    return true;
}

void test_audio(const char *vectors) {
    size_t len;
    char *text = SDL_LoadFile(vectors, &len);
    gs_json *doc = text ? gs_json_parse(text, len) : NULL;
    CHECK(doc);
    int checked = 0;
    for (gs_jv c = gs_json_first(gs_json_get(gs_json_root(doc), "cases")); c; c = gs_json_next(c)) {
        int rate = (int)gs_json_num(gs_json_get(c, "sampleRate"), 0), count, seed = (int)gs_json_num(gs_json_get(c, "seed"), 0);
        float *s = render(gs_json_get(c, "parts"), rate, (uint32_t)seed, &count);
        // The recording itself first: everything after depends on it being the same samples.
        CHECK(count == (int)gs_json_num(gs_json_get(c, "frames"), -1));
        CHECK(fingerprint(s, count) == (uint32_t)gs_json_num(gs_json_get(c, "fingerprint"), -1));
        for (gs_jv r = gs_json_first(gs_json_get(c, "results")); r; r = gs_json_next(r)) {
            gs_jv opts = gs_json_get(r, "opts");
            eh_split o = EH_SPLIT_DEFAULTS;
            o.max_sec = option(opts, "maxSec", 0);
            o.min_pause_sec = option(opts, "minPauseSec", o.min_pause_sec);
            o.min_sound_sec = option(opts, "minSoundSec", 0);
            o.min_utterance_sec = option(opts, "minUtteranceSec", 0);
            o.keep_pause_sec = option(opts, "keepPauseSec", INFINITY);
            int failures_before = failures, n;

            int *cuts = eh_split_points(s, count, rate, &o, &n);
            CHECK(same_ints(cuts, n, gs_json_get(r, "cuts")));
            SDL_free(cuts);

            if (isfinite(o.min_pause_sec)) {
                eh_pause *pauses = eh_pause_spans(s, count, rate, &o, &n);
                gs_jv want = gs_json_get(r, "pauses");
                bool same = n == gs_json_count(want);
                for (int i = 0; same && i < n; i++) {
                    gs_jv w = gs_json_at(want, i);
                    same = pauses[i].from == (int)gs_json_num(gs_json_at(w, 0), -1) && pauses[i].to == (int)gs_json_num(gs_json_at(w, 1), -1) &&
                           pauses[i].part_count == (int)gs_json_num(gs_json_at(w, 2), -1);
                }
                CHECK(same);
                eh_pauses_free(pauses, n);
            }
            CHECK(eh_sound_seconds(s, count, rate, &o) == gs_json_num(gs_json_get(r, "soundSeconds"), -1));

            gs_jv sq = gs_json_get(r, "squeezed");
            int from = (int)gs_json_num(gs_json_get(sq, "from"), 0), to = (int)gs_json_num(gs_json_get(sq, "to"), 0);
            float *piece = eh_squeeze_pauses(s + from, to - from, rate, &o, &n);
            CHECK((piece == s + from) == gs_json_bool(gs_json_get(sq, "same"), false));
            CHECK(n == (int)gs_json_num(gs_json_get(sq, "length"), -1));
            CHECK(fingerprint(piece, n) == (uint32_t)gs_json_num(gs_json_get(sq, "fingerprint"), -1));
            if (piece != s + from) SDL_free(piece);

            gs_jv fc = gs_json_get(r, "fitCap");
            cuts = eh_fit_cap(s, rate, (int)gs_json_num(gs_json_get(fc, "from"), 0), (int)gs_json_num(gs_json_get(fc, "to"), 0), &o, &n);
            CHECK(same_ints(cuts, n, gs_json_get(fc, "cuts")));
            SDL_free(cuts);
            if (failures != failures_before) printf("  seed %d, \"%s\"\n", seed, gs_json_str(gs_json_get(r, "name"), ""));
            checked++;
        }
        int third = count / 3;
        gs_jv speech = gs_json_get(c, "speech"), quietest = gs_json_get(c, "quietest");
        CHECK(eh_contains_speech(s, count, rate) == gs_json_bool(gs_json_at(speech, 0), false));
        CHECK(eh_contains_speech(s, third, rate) == gs_json_bool(gs_json_at(speech, 1), false));
        CHECK(eh_contains_speech(s + third, (int)js_round(0.4 * rate), rate) == gs_json_bool(gs_json_at(speech, 2), false));
        CHECK(eh_quietest_offset(s, third, (int)floor(0.3 * rate), (int)floor(0.05 * rate)) == (int)gs_json_num(gs_json_at(quietest, 0), -1));
        CHECK(eh_quietest_offset(s, 100, 4800, 800) == (int)gs_json_num(gs_json_at(quietest, 1), -1));
        SDL_free(s);
    }
    for (gs_jv p = gs_json_first(gs_json_get(gs_json_root(doc), "probes")); p; p = gs_json_next(p)) {
        int count;
        float *s = render(gs_json_get(p, "parts"), 16000, (uint32_t)gs_json_num(gs_json_get(p, "seed"), 0), &count);
        CHECK(eh_contains_speech(s, count, 16000) == gs_json_bool(gs_json_get(p, "speech"), false));
        SDL_free(s);
    }
    printf("  %d recordings and option sets compared\n", checked);
    gs_json_free(doc);
    SDL_free(text);
}
