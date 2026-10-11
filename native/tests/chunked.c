// The chunked decoder against main/chunked-decode.js: each dictation in tests/vectors/chunked.json
// is rebuilt from its seed and decoded by a fake engine that follows the recorded script. The text,
// every piece's fate, the audio of each call and the restarts must all match.
#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>
#include "chunked.h"
#include "gs_json.h"
#include "gs_rand.h"
#include "test.h"
#include "wav.h"

static double js_round(double x) { return floor(x + 0.5); }

typedef struct { gs_jv script, calls; int at, restarts, decode_ms; bool mismatch; } fake;

static uint32_t pcm_fingerprint(const uint8_t *wav, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 44; i + 1 < len; i += 2) sum += (uint32_t)(wav[i] | wav[i + 1] << 8) * (uint32_t)((i >> 1) % 251 + 1);
    return sum;
}

static eh_decode_code fake_run(void *user, const uint8_t *wav, size_t len, char **text, int *decode_ms, char *error, size_t error_size) {
    fake *f = user;
    int n = eh_wav_frames(wav, len);
    gs_jv want = gs_json_at(f->calls, f->at);
    if (!want || (int)gs_json_num(gs_json_at(want, 0), -1) != n || (uint32_t)gs_json_num(gs_json_at(want, 1), -1) != pcm_fingerprint(wav, len)) f->mismatch = true;
    const char *outcome = gs_json_str(gs_json_at(f->script, f->at), "ok");
    f->at++;
    if (!strcmp(outcome, "exit")) { SDL_strlcpy(error, "engine process exited", error_size); return EH_DECODE_EXITED; }
    if (!strcmp(outcome, "timeout")) { SDL_strlcpy(error, "engine request timed out", error_size); return EH_DECODE_TIMEOUT; }
    if (!strcmp(outcome, "error")) { SDL_strlcpy(error, "model exploded", error_size); return EH_DECODE_FAILED; }
    *decode_ms = n % 97;
    if (!strcmp(outcome, "empty")) *text = SDL_strdup("");
    else if (!strcmp(outcome, "space")) *text = SDL_strdup("  ");
    else SDL_asprintf(text, " t%d ", n);
    return EH_DECODE_OK;
}

static void fake_restart(void *user) { ((fake *)user)->restarts++; }
static void fake_decode_ms(void *user, int ms) { ((fake *)user)->decode_ms += ms; }

static const char *code_name(eh_decode_code c) {
    return c == EH_DECODE_EXITED ? "exit" : c == EH_DECODE_TIMEOUT ? "timeout" : c == EH_DECODE_EMPTY_SPEECH ? "empty_speech" : c == EH_DECODE_FAILED ? "failed" : "";
}

void test_chunked(const char *vectors) {
    size_t len;
    char *text = SDL_LoadFile(vectors, &len);
    gs_json *doc = text ? gs_json_parse(text, len) : NULL;
    CHECK(doc);
    int checked = 0;
    for (gs_jv c = gs_json_first(gs_json_get(gs_json_root(doc), "cases")); c; c = gs_json_next(c)) {
        int seed = (int)gs_json_num(gs_json_get(c, "seed"), 0), before = failures;
        // The recording, as test/audio.c builds it, through PCM16 as a recorded WAV is.
        gs_mulberry rng = { (uint32_t)seed ^ 0x5eed };
        int frames = (int)gs_json_num(gs_json_get(c, "frames"), 0), at = 0;
        float *s = SDL_malloc(sizeof(float) * (size_t)(frames + 1));
        for (gs_jv p = gs_json_first(gs_json_get(c, "parts")); p; p = gs_json_next(p)) {
            int n = (int)js_round(gs_json_num(gs_json_at(p, 0), 0) * 16000);
            double amp = gs_json_num(gs_json_at(p, 1), 0);
            for (int i = 0; i < n; i++) s[at++] = (float)((gs_mulberry_next(&rng) * 2 - 1) * amp);
        }
        CHECK(at == frames);
        int16_t *pcm = eh_wav_pcm16(s, frames, 0);
        size_t wav_len;
        uint8_t *wav = eh_wav_encode(pcm, frames, 16000, &wav_len);
        SDL_free(pcm);
        SDL_free(s);

        gs_jv opts = gs_json_get(c, "opts"), chunks = gs_json_get(c, "salvageChunks");
        int salvage_count = gs_json_count(chunks);
        eh_salvage *salvage = SDL_calloc((size_t)salvage_count + 1, sizeof *salvage);
        for (int i = 0; i < salvage_count; i++) {
            gs_jv k = gs_json_at(chunks, i);
            salvage[i] = (eh_salvage){ (int)gs_json_num(gs_json_get(k, "from"), 0), (int)gs_json_num(gs_json_get(k, "to"), 0), gs_json_str(gs_json_get(k, "text"), "") };
        }
        bool every_pause = gs_json_bool(gs_json_get(opts, "everyPause"), false);
        fake f = { gs_json_get(c, "script"), gs_json_get(c, "calls") };
        eh_chunked o = {
            .run = fake_run, .user = &f, .restart_stt = fake_restart, .on_decode_ms = fake_decode_ms,
            .max_sec = gs_json_num(gs_json_get(opts, "maxSec"), 20),
            .min_sound_sec = every_pause ? 0 : -1, .min_utterance_sec = every_pause ? 0 : -1, .keep_pause_sec = every_pause ? INFINITY : 0,
            .prefix_text = gs_json_str(gs_json_get(opts, "prefixText"), ""), .salvage_text = gs_json_str(gs_json_get(opts, "salvageText"), ""),
            .salvage = salvage, .salvage_count = salvage_count,
        };
        eh_chunked_result r;
        bool ok = eh_transcribe_chunked(wav, wav_len, &o, &r);
        gs_jv threw = gs_json_get(c, "threw"), want = gs_json_get(c, "result");
        CHECK(ok == (gs_json_kind(threw) == GS_JSON_NULL));
        CHECK(!f.mismatch && f.at == gs_json_count(f.calls));
        CHECK(f.restarts == (int)gs_json_num(gs_json_get(c, "restarts"), -1));
        if (ok) {
            CHECK(!strcmp(r.text, gs_json_str(gs_json_get(want, "text"), "?")));
            CHECK(r.partial == gs_json_bool(gs_json_get(want, "partial"), false));
            CHECK(f.decode_ms == (int)gs_json_num(gs_json_get(c, "decodeMs"), -1));
            gs_jv pieces = gs_json_get(want, "pieces");
            CHECK(r.piece_count == gs_json_count(pieces));
            for (int i = 0; i < r.piece_count && i < gs_json_count(pieces); i++) {
                gs_jv w = gs_json_at(pieces, i);
                const eh_piece *p = &r.pieces[i];
                bool same = p->from == (int)gs_json_num(gs_json_at(w, 0), -1) && p->to == (int)gs_json_num(gs_json_at(w, 1), -1) &&
                            p->group == (int)gs_json_num(gs_json_at(w, 2), -1) && p->attempts == (int)gs_json_num(gs_json_at(w, 3), -1) &&
                            p->ok == gs_json_bool(gs_json_at(w, 4), false) && p->skipped == gs_json_bool(gs_json_at(w, 5), false) &&
                            p->noise == gs_json_bool(gs_json_at(w, 6), false) && p->unconfirmed == gs_json_bool(gs_json_at(w, 7), false) &&
                            !strcmp(p->text ? p->text : "", gs_json_str(gs_json_at(w, 8), "?")) && !strcmp(p->ok ? "" : code_name(p->code), gs_json_str(gs_json_at(w, 9), "?"));
                CHECK(same);
            }
        } else {
            CHECK(!strcmp(code_name(r.code), gs_json_str(gs_json_get(threw, "code"), "?")));
            CHECK(!strcmp(r.error, gs_json_str(gs_json_get(threw, "message"), "?")));
        }
        if (failures != before) printf("  seed %d\n", seed);
        eh_chunked_result_free(&r);
        SDL_free(salvage);
        SDL_free(wav);
        checked++;
    }
    printf("  %d dictations compared\n", checked);
    gs_json_free(doc);
    SDL_free(text);
}
