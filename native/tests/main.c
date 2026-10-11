// Tests: earheart-tests TESTS_DIR [ENGINES_DIR]. The prompt test needs the cleanup models and runs
// only when EARHEART_MODELS names their folder and the engines are there.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "catalog.h"
#include "cleanup.h"
#include "engine.h"
#include "paths.h"
#include "test.h"
#include "wav.h"

int failures;

static size_t make_wav(uint8_t *buf, int channels, int rate, const int16_t *samples, int count, bool extra_chunk) {
    uint8_t *p = buf;
    uint32_t data = (uint32_t)count * 2;
    memcpy(p, "RIFF", 4); p += 8;
    memcpy(p, "WAVE", 4); p += 4;
    if (extra_chunk) { memcpy(p, "LIST", 4); p[4] = 3; p[5] = p[6] = p[7] = 0; p += 8 + 4; }  // odd size, padded
    memcpy(p, "fmt ", 4); p[4] = 16; p[5] = p[6] = p[7] = 0; p += 8;
    p[0] = 1; p[1] = 0; p[2] = (uint8_t)channels; p[3] = 0;
    p[4] = (uint8_t)rate; p[5] = (uint8_t)(rate >> 8); p[6] = (uint8_t)(rate >> 16); p[7] = 0;
    memset(p + 8, 0, 6); p[14] = 16; p[15] = 0; p += 16;
    memcpy(p, "data", 4); p[4] = (uint8_t)data; p[5] = (uint8_t)(data >> 8); p[6] = p[7] = 0; p += 8;
    memcpy(p, samples, data); p += data;
    return (size_t)(p - buf);
}

static void test_wav(void) {
    uint8_t buf[256];
    const int16_t mono[] = { 0, 16384, -32768, 32767 };
    const char *error = NULL;
    int frames, rate;
    size_t len = make_wav(buf, 1, 16000, mono, 4, false);
    float *f = eh_wav_floats(buf, len, &frames, &rate, &error);
    CHECK(f && frames == 4 && rate == 16000);
    CHECK(f && f[0] == 0 && f[1] == 0.5f && f[2] == -1.0f && f[3] == 32767.0f / 32768.0f);
    SDL_free(f);

    // Other chunks are skipped, with the pad byte after an odd-sized one.
    len = make_wav(buf, 1, 48000, mono, 4, true);
    f = eh_wav_floats(buf, len, &frames, &rate, &error);
    CHECK(f && frames == 4 && rate == 48000 && f[1] == 0.5f);
    SDL_free(f);

    // Stereo is averaged to mono.
    const int16_t stereo[] = { 16384, 0, -32768, -32768 };
    len = make_wav(buf, 2, 16000, stereo, 4, false);
    f = eh_wav_floats(buf, len, &frames, &rate, &error);
    CHECK(f && frames == 2 && f[0] == 0.25f && f[1] == -1.0f);
    SDL_free(f);
    CHECK(eh_wav_seconds(buf, len) == 0.01);  // 2 frames: the floor

    CHECK(!eh_wav_floats((const uint8_t *)"RIFFxxxxWAVX", 12, &frames, &rate, &error) && !strcmp(error, "Not a RIFF/WAVE file"));
    len = make_wav(buf, 1, 16000, mono, 4, false);
    memcpy(buf + 36, "junk", 4);  // no data chunk
    CHECK(!eh_wav_floats(buf, len, &frames, &rate, &error) && !strcmp(error, "WAV has no data chunk"));
    len = make_wav(buf, 1, 16000, mono, 4, false);
    buf[34] = 8;  // 8 bits a sample
    CHECK(!eh_wav_floats(buf, len, &frames, &rate, &error) && strstr(error, "Unsupported"));
}

static void test_jw(void) {
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "text", "a \"quoted\"\\ line\n\twith tab");
    eh_jw_str(&w, "absent", NULL);
    eh_jw_num(&w, "n", 16000);
    eh_jw_num(&w, "f", 0.2);
    eh_jw_bool(&w, "ok", true);
    char *json = eh_jw_end(&w);
    CHECK(!strcmp(json, "{\"text\":\"a \\\"quoted\\\"\\\\ line\\n\\u0009with tab\",\"n\":16000,\"f\":0.2,\"ok\":true}"));
    gs_json *doc = gs_json_parse(json, strlen(json));
    CHECK(doc && !strcmp(gs_json_str(gs_json_get(gs_json_root(doc), "text"), ""), "a \"quoted\"\\ line\n\twith tab"));
    gs_json_free(doc);
    SDL_free(json);
    eh_jw_begin(&w);
    json = eh_jw_end(&w);
    CHECK(!strcmp(json, "{}"));
    SDL_free(json);
}

static void test_turn(void) {
    char *turn = eh_cleanup_turn("RULES", "some words"), *prefix = eh_cleanup_turn_prefix("RULES", "some");
    CHECK(!strcmp(turn, "RULES\n\nTranscript:\nsome words\n\nCleaned transcript:"));
    CHECK(!strncmp(turn, prefix, strlen(prefix)));  // priming relies on the prefix
    SDL_free(turn);
    SDL_free(prefix);
}

static eh_engine_status request(eh_engine *e, const char *type, eh_engine_reply *r) {
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", type);
    return eh_engine_call(e, &w, NULL, 0, 0, NULL, NULL, r);
}

static int slow_ping(void *user) {
    eh_engine_reply r;
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", "ping");
    eh_jw_num(&w, "delayMs", 400);
    eh_engine_status s = eh_engine_call(user, &w, NULL, 0, 0, NULL, NULL, &r);
    eh_engine_reply_free(&r);
    return (int)s;
}

static void test_engine(const char *kind) {
    eh_engine *e = eh_engine_new(kind);
    eh_engine_reply r;
    CHECK(!eh_engine_running(e) && !eh_engine_busy(e));
    CHECK(request(e, "ping", &r) == EH_ENGINE_OK && gs_json_bool(gs_json_get(r.v, "pong"), false));
    eh_engine_reply_free(&r);
    CHECK(eh_engine_running(e));

    // An unknown request is answered with an error and the worker lives on.
    CHECK(request(e, "no-such-request", &r) == EH_ENGINE_FAILED && !strcmp(r.error, "Unknown request"));
    eh_engine_reply_free(&r);
    CHECK(eh_engine_running(e));

    // A worker that dies answers what was waiting, and the next request starts another.
    unsigned before = eh_engine_generation(e);
    CHECK(request(e, "exit", &r) == EH_ENGINE_EXITED && strstr(r.error, "engine stopped"));
    CHECK(eh_engine_generation(e) != before);
    CHECK(request(e, "ping", &r) == EH_ENGINE_OK);
    eh_engine_reply_free(&r);

    // A request without what it needs is refused before any library is opened.
    CHECK(request(e, !strcmp(kind, "stt") ? "load-stt" : "clean", &r) == EH_ENGINE_FAILED);
    eh_engine_reply_free(&r);

    // No answer within the deadline: the caller gets a timeout and the worker is retired, which
    // answers a second caller waiting on it as exited.
    SDL_Thread *other = SDL_CreateThread(slow_ping, "second caller", e);
    SDL_Delay(50);
    eh_jw w;
    eh_jw_begin(&w);
    eh_jw_str(&w, "type", "ping");
    eh_jw_num(&w, "delayMs", 5000);
    Uint64 t0 = SDL_GetTicks();
    CHECK(eh_engine_call(e, &w, NULL, 0, 200, NULL, NULL, &r) == EH_ENGINE_TIMEOUT);
    CHECK(SDL_GetTicks() - t0 < 2000);
    int status;
    SDL_WaitThread(other, &status);
    CHECK(status == EH_ENGINE_EXITED || status == EH_ENGINE_OK);
    CHECK(request(e, "ping", &r) == EH_ENGINE_OK);
    eh_engine_reply_free(&r);
    eh_engine_free(e);
}

// The prompt each catalogue model is given, against what the Electron build's library renders
// (tests/prompts.json, written from tools/prompt-oracle.mjs's output).
static void test_prompts(const char *models, const char *fixture) {
    size_t len;
    char *text = SDL_LoadFile(fixture, &len);
    gs_json *doc = text ? gs_json_parse(text, len) : NULL;
    CHECK(doc);
    for (gs_jv entry = gs_json_first(gs_json_root(doc)); entry; entry = gs_json_next(entry)) {
        const eh_model *m = eh_catalog_find(EH_MODEL_CLEANUP, gs_json_key(entry));
        CHECK(m);
        char path[1024];
        SDL_snprintf(path, sizeof path, "%s/cleanup/%s/%s", models, m->id, m->gguf);
        if (!SDL_GetPathInfo(path, NULL)) { printf("  %s: not downloaded, skipped\n", m->id); continue; }
        if (!eh_cleanup_load(eh_engines_dir(), path, 0, true)) { printf("  %s: %s\n", m->id, eh_cleanup_error()); failures++; continue; }
        int n;
        int *got = eh_cleanup_prompt_tokens(gs_json_str(gs_json_get(entry, "turn"), ""), &n);
        gs_jv want = gs_json_get(entry, "tokens");
        bool same = n == gs_json_count(want);
        for (int i = 0; same && i < n; i++) same = got[i] == (int)gs_json_num(gs_json_at(want, i), -1);
        if (!same) { printf("  %s: prompt tokens differ from the Electron build's\n", m->id); failures++; }
        else printf("  %s: %d prompt tokens identical\n", m->id, n);
        SDL_free(got);
    }
    eh_cleanup_unload();
    gs_json_free(doc);
    SDL_free(text);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--engine")) return eh_worker_main(argv[2]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_CRITICAL);  // the failures the tests provoke are logged by design
    const struct { const char *name; void (*fn)(void); } tests[] = { { "wav", test_wav }, { "jw", test_jw }, { "turn", test_turn }, { "overlay", test_overlay } };
    for (size_t i = 0; i < SDL_arraysize(tests); i++) {
        int before = failures;
        tests[i].fn();
        printf("%-12s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
    }
    const char *kinds[] = { "stt", "cleanup" };
    for (int i = 0; i < 2; i++) {
        int before = failures;
        test_engine(kinds[i]);
        printf("engine %-5s %s\n", kinds[i], failures == before ? "ok" : "FAILED");
    }
    if (argc >= 3 && !SDL_getenv("EARHEART_ENGINES")) SDL_setenv_unsafe("EARHEART_ENGINES", argv[2], 1);
    char path[1024];
    if (argc >= 2) {
        int before = failures;
        SDL_snprintf(path, sizeof path, "%s/vectors/audio.json", argv[1]);
        test_audio(path);
        printf("%-12s %s\n", "audio", failures == before ? "ok" : "FAILED");
        before = failures;
        SDL_snprintf(path, sizeof path, "%s/vectors/chunked.json", argv[1]);
        test_chunked(path);
        printf("%-12s %s\n", "chunked", failures == before ? "ok" : "FAILED");
        before = failures;
        SDL_snprintf(path, sizeof path, "%s/vectors/text.json", argv[1]);
        test_text(path);
        printf("%-12s %s\n", "text", failures == before ? "ok" : "FAILED");
        before = failures;
#if defined(__APPLE__)
        SDL_snprintf(path, sizeof path, "%s/vectors/hotkeys-darwin.json", argv[1]);
#elif defined(_WIN32)
        SDL_snprintf(path, sizeof path, "%s/vectors/hotkeys-win32.json", argv[1]);
#else
        SDL_snprintf(path, sizeof path, "%s/vectors/hotkeys-linux.json", argv[1]);
#endif
        test_hotkeys(path);
        printf("%-12s %s\n", "hotkeys", failures == before ? "ok" : "FAILED");
        before = failures;
        SDL_snprintf(path, sizeof path, "%s/vectors/settings.json", argv[1]);
        test_settings(path);
        printf("%-12s %s\n", "settings", failures == before ? "ok" : "FAILED");
    }
    const char *models = SDL_getenv("EARHEART_MODELS");
    if (models && argc >= 2) {
        int before = failures;
        SDL_snprintf(path, sizeof path, "%s/prompts.json", argv[1]);
        test_prompts(models, path);
        printf("%-12s %s\n", "prompts", failures == before ? "ok" : "FAILED");
    } else {
        printf("%-12s skipped (set EARHEART_MODELS)\n", "prompts");
    }
    return failures ? 1 : 0;
}
