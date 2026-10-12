// The overlay, played to as the app plays to it: a view at each moment of a dictation, stepped
// through eh_overlay_step and drawn without a window. Ported from scripts/overlay-smoke.js, the
// checks that are the overlay's own; the microphone's are the recorder's, tested by tests/e2e.sh.
#include <SDL3/SDL.h>
#include <string.h>
#include "overlay.h"
#include "test.h"

static eh_view V;
static Uint64 now = 1000;
static eh_overlay_model M;
static eh_overlay_layout L;
static double captured;
static int cap;

static void say(const char *status, const char *detail_json) {
    SDL_strlcpy(V.status, status, sizeof V.status);
    gs_jval_free(V.detail);
    V.detail = detail_json ? gs_jval_parse(detail_json, strlen(detail_json)) : NULL;
    V.phase[0] = 0;
    V.fraction = 0;
}

// One frame, `ms` later.
static int step(int ms) {
    now += (Uint64)ms;
    eh_overlay_input in = { &V, captured, cap, 0.08f };
    return eh_overlay_step(&in, now, &M);
}

// Drawn at one pixel a point; returns how many pixels of the accent colour `box` holds.
static int draw(const SDL_FRect *box, int *height) {
    SDL_Surface *s = eh_overlay_render(&M, 1, &L);
    int lit = 0;
    if (!s) { CHECK(s != NULL); return 0; }
    if (height) *height = s->h;
    for (int y = box ? (int)box->y : 0; box && y < (int)(box->y + box->h) && y < s->h; y++) {
        const Uint8 *row = (const Uint8 *)s->pixels + y * s->pitch;
        for (int x = (int)box->x; x < (int)(box->x + box->w) && x < s->w; x++) {
            const Uint8 *p = row + x * 4;
            lit += p[0] > 180 && p[1] < 130 && p[2] < 140 && p[3] > 100;
        }
    }
    SDL_DestroySurface(s);
    return lit;
}

static bool inside(SDL_FRect in, SDL_FRect out) { return in.x >= out.x && in.y >= out.y && in.x + in.w <= out.x + out.w + 0.01f && in.y + in.h <= out.y + out.h + 0.01f; }
static bool centred(SDL_FRect in, SDL_FRect out) { return SDL_fabsf((in.y + in.h / 2) - (out.y + out.h / 2)) < 0.01f; }

void test_overlay(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { printf("  no video: %s\n", SDL_GetError()); failures++; return; }
    eh_overlay_reset();
    V = (eh_view){ .visible = true, .sid = 1 };
    int base_height = 0;

    // The microphone starting: Done can already be pressed (it abandons the attempt), pause cannot.
    say("starting", NULL);
    CHECK(step(16) == 16 && !strcmp(M.title, "Starting mic…"));
    CHECK(!eh_overlay_key_enabled(&M, 1) && eh_overlay_key_enabled(&M, 2) && eh_overlay_key_enabled(&M, 3));
    draw(NULL, &base_height);
    CHECK(base_height == EH_OVERLAY_HEIGHT);
    // The keys: 32 points each, in the card, pause then done then discard.
    for (int i = 0; i < 3; i++) CHECK(L.keys[i].w == 32 && inside(L.keys[i], L.card));
    CHECK(L.keys[0].x + 32 < L.keys[1].x && L.keys[1].x + 32 < L.keys[2].x);

    // Live: the waveform's write clock runs, a column every 80 ms, and the waveform paints.
    say("recording", NULL);
    step(16);
    CHECK(!strcmp(M.title, "Listening…") && eh_overlay_key_enabled(&M, 1) && eh_overlay_key_enabled(&M, 2));
    int before = M.level_count;
    for (int i = 0; i < 50; i++) { captured += 0.016; step(16); }
    CHECK(M.level_count - before == 10);
    CHECK(draw(&L.wave, NULL) > 0);
    CHECK(L.detail_lines == 0 && L.transcript_lines == 0);

    // A remark about the microphone is fully visible while recording, paused and resumed.
    SDL_strlcpy(V.notice, "Saved mic missing — using the system default", sizeof V.notice);
    step(16);
    draw(NULL, NULL);
    CHECK(L.detail_lines >= 1 && !L.detail_clipped && inside(L.detail, L.wave));
    say("paused", NULL);
    step(16);
    draw(NULL, NULL);
    CHECK(!strcmp(M.detail, V.notice) && L.detail_lines >= 1 && !L.detail_clipped);
    say("recording", NULL);
    step(16);
    CHECK(!strcmp(M.detail, V.notice));
    V.notice[0] = 0;

    // Paused: the hint takes the detail line and fits it, the waveform holds, stop and pause stay
    // usable, and the timer counts captured audio only.
    step(16);
    say("paused", NULL);
    CHECK(step(16) == 250 && !strcmp(M.title, "Paused") && !strcmp(M.detail, "Press play to resume"));
    draw(NULL, NULL);
    CHECK(L.detail_lines == 1 && !L.detail_clipped && centred(L.detail, L.wave));
    CHECK(eh_overlay_key_enabled(&M, 1) && eh_overlay_key_enabled(&M, 2));
    before = M.level_count;
    step(2000);
    CHECK(M.level_count == before);
    captured = 6.4;
    step(16);
    CHECK(!strcmp(M.timer, "0:06"));
    // Resumed: the detail line clears and the write clock runs again.
    say("recording", NULL);
    step(16);
    CHECK(!M.detail[0]);
    step(800);
    CHECK(M.level_count == before + 10);

    // The live transcript: the cleaned text and the raw tail past it, and the card grows for them.
    V.cleaned = SDL_strdup("Port the overlay to the native build,");
    V.raw = SDL_strdup("port the overlay to the native build and then check that the card grows upward when the words no longer fit on a single line of it");
    step(16);
    int grown = 0;
    draw(NULL, &grown);
    CHECK(L.transcript_lines >= 2 && grown > base_height && L.card.h > 80);

    // A take with no cap, or a bad one, never warns. With one, the last minute says so and counts down.
    cap = -1;
    captured = 290.5;
    step(16);
    CHECK(!M.cap_warning && !strcmp(M.title, "Listening…") && !strcmp(M.timer, "4:50"));
    cap = 300;
    step(16);
    CHECK(M.cap_warning && !strcmp(M.title, "Stopping soon…") && !strcmp(M.timer, "−0:10"));

    // Stopped: the timer is the take's length again, the transcript retires, pause and done go
    // inert, and discard stays.
    say("transcribing", NULL);
    SDL_free(V.raw);
    SDL_free(V.cleaned);
    V.raw = V.cleaned = NULL;
    step(16);
    CHECK(!M.cap_warning && !strcmp(M.timer, "4:50") && !strcmp(M.title, "Transcribing…"));
    CHECK(!eh_overlay_key_enabled(&M, 1) && !eh_overlay_key_enabled(&M, 2) && eh_overlay_key_enabled(&M, 3));
    CHECK(!M.progress);
    draw(NULL, &grown);
    CHECK(grown == base_height && L.transcript_lines == 0);

    // Progress: the track spans the strip and sits a point above its bottom; a finished bar holds
    // 400 ms, through the change to the next phase, and then goes.
    SDL_strlcpy(V.phase, "transcribing", sizeof V.phase);
    V.fraction = 0.5f;
    step(16);
    CHECK(M.progress && M.fraction == 0.5f);
    draw(NULL, NULL);
    CHECK(L.progress.x == L.wave.x && L.progress.w == L.wave.w && L.progress.y + L.progress.h == L.wave.y + L.wave.h - 1);
    CHECK(draw(&L.progress, NULL) > 0);
    V.fraction = 1;
    step(16);
    say("cleaning", NULL);
    step(100);
    CHECK(M.progress && M.fraction == 1 && !strcmp(M.title, "Cleaning up…"));
    step(400);
    CHECK(!M.progress);
    // Progress reported for another phase than the one showing is not drawn.
    SDL_strlcpy(V.phase, "transcribing", sizeof V.phase);
    V.fraction = 0.3f;
    step(16);
    CHECK(!M.progress);

    say("delivering", NULL);
    CHECK(step(16) == 16 && !strcmp(M.title, "Typing…") && eh_overlay_key_enabled(&M, 3) && !eh_overlay_key_enabled(&M, 2));

    // Done, by how the text got there; an interrupted dictation is named.
    say("done", "{\"method\":\"paste\",\"preview\":\"Port the overlay\"}");
    CHECK(step(16) == -1 && !strcmp(M.title, "Pasted") && !strcmp(M.detail, "Port the overlay"));
    draw(NULL, NULL);
    CHECK(L.detail_lines == 1 && centred(L.detail, L.wave) && L.wave.h == 32);
    say("done", "{\"method\":\"paste\",\"incomplete\":true}");
    step(16);
    CHECK(!strcmp(M.title, "Pasted — incomplete"));
    say("done", "{\"method\":\"clipboard\"}");
    step(16);
    CHECK(!strcmp(M.title, "Copied to clipboard"));
    say("done", "{\"method\":\"paste-copy\"}");
    step(16);
    CHECK(!strcmp(M.title, "Pasted & copied"));
    say("done", "{\"method\":\"clipboard\",\"note\":\"Accessibility permission is off\"}");
    step(16);
    CHECK(!strcmp(M.title, "Copied — auto-paste failed") && !strcmp(M.detail, "Accessibility permission is off"));

    say("empty", NULL);
    step(16);
    draw(NULL, NULL);
    CHECK(!strcmp(M.title, "Nothing heard") && L.detail_lines >= 1 && !L.detail_clipped);

    // A detail of two lines is shown whole and centred; a longer one stays inside the strip.
    char message[400] = "The speech";
    int lines = 1;
    for (int words = 0; words < 40 && lines < 2; words++) {
        SDL_strlcat(message, " engine", sizeof message);
        char json[512];
        SDL_snprintf(json, sizeof json, "{\"message\":\"%s\"}", message);
        say("error", json);
        step(16);
        draw(NULL, NULL);
        lines = L.detail_lines;
    }
    CHECK(!strcmp(M.title, "Failed") && lines == 2 && !L.detail_clipped && inside(L.detail, L.wave) && centred(L.detail, L.wave));
    say("error", "{\"message\":\"The speech engine stopped and could not be started again, and this sentence goes on for much longer than two lines of the strip can hold, so the rest of it has to be cut where the second line ends\"}");
    step(16);
    draw(NULL, NULL);
    CHECK(L.detail_lines == 2 && L.detail_clipped && inside(L.detail, L.wave));

    // A new dictation starts with an empty waveform.
    V.sid = 2;
    captured = 0;
    cap = 0;
    say("starting", NULL);
    step(16);
    CHECK(M.level_count == 0 && !strcmp(M.timer, "0:00"));

    gs_jval_free(V.detail);
    V.detail = NULL;
    eh_overlay_reset();
}
