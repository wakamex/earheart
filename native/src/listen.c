#include "listen.h"
#include <SDL3/SDL.h>
#include "gs_voice.h"
#include "loop.h"
#include "recorder.h"
#include "wav.h"

#define FRAME 160                 // 10 ms at 16 kHz: what the detector takes
#define LEAD_FRAMES 40            // audio kept from before speech was noticed, so its first sound is whole
#define TAIL_FRAMES 25            // quiet kept after the last speech
#define MIN_SPEECH_FRAMES 25      // a stretch with less speech than this was a click or a cough
#define WAITING 8                 // stretches that may wait for the pipeline; the oldest goes first

int eh_turn_step(eh_turn *t, float probability) {
    bool speech = probability >= 0.5f;
    if (!t->speaking) {
        t->run = speech ? t->run + 1 : 0;
        if (t->run < EH_TURN_START_FRAMES) return 0;
        t->speaking = true;
        t->quiet = 0;
        return 1;
    }
    t->quiet = speech ? 0 : t->quiet + 1;
    if (t->quiet < EH_TURN_STOP_FRAMES) return 0;
    t->speaking = false;
    t->run = 0;
    return -1;
}

typedef struct { uint8_t *wav; size_t len; } stretch;

static struct {
    SDL_Thread *thread;
    SDL_AtomicInt stop, held_off;
    eh_mic *mic;
    char device[256];
    int max_frames;
    stretch waiting[WAITING];
    int waiting_count;
} L;

bool eh_listen_on(void) { return L.thread != NULL; }

// With the lock held: the oldest waiting stretch goes to the pipeline, when it is free.
static void submit(void *user) {
    if (!L.waiting_count || !eh_pipeline_utterance(L.waiting[0].wav, L.waiting[0].len)) return;
    SDL_memmove(L.waiting, L.waiting + 1, sizeof(stretch) * (size_t)--L.waiting_count);
}

void eh_listen_state(eh_state state) {
    SDL_SetAtomicInt(&L.held_off, state == EH_RECORDING);
    // Not from inside the pipeline's own change of state: as a task of its own, after it.
    if (state == EH_IDLE && L.waiting_count) eh_spawn(submit, NULL);
}

static void warm(void *user) { eh_pipeline_warm(); }

typedef struct { float *samples; int frames, cap, speech; } take;

static void take_add(take *t, const float *frame) {
    if (t->frames == t->cap) t->samples = SDL_realloc(t->samples, sizeof(float) * FRAME * (size_t)(t->cap = t->cap ? t->cap * 2 : 512));
    SDL_memcpy(t->samples + (size_t)t->frames++ * FRAME, frame, sizeof(float) * FRAME);
}

// A finished stretch joins the queue, without the quiet that ended it.
static void take_finish(take *t, int trailing_quiet) {
    int frames = t->frames - SDL_max(0, trailing_quiet - TAIL_FRAMES);
    if (t->speech >= MIN_SPEECH_FRAMES && frames > 0) {
        SDL_Log("always-on: %.1f s of speech", frames / 100.0);
        stretch s;
        s.wav = eh_wav_encode_recorded(t->samples, frames * FRAME, EH_SAMPLE_RATE, &s.len);
        eh_lock();
        if (L.waiting_count == WAITING) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "always-on: speech is arriving faster than it is transcribed; the oldest waiting stretch was dropped");
            SDL_free(L.waiting[0].wav);
            SDL_memmove(L.waiting, L.waiting + 1, sizeof(stretch) * (size_t)--L.waiting_count);
        }
        L.waiting[L.waiting_count++] = s;
        submit(NULL);
        eh_unlock();
    }
    t->frames = t->speech = 0;
}

static int listen_main(void *user) {
    gs_voice *voice = gs_voice_new(EH_SAMPLE_RATE, GS_VOICE_SPEECH);
    eh_turn turn = { 0 };
    take t = { 0 };
    float lead[LEAD_FRAMES * FRAME], batch[512], pending[512 + FRAME];
    int lead_at = 0, lead_count = 0, held = 0;
    while (!SDL_GetAtomicInt(&L.stop)) {
        int n = eh_mic_read(L.mic, batch, SDL_arraysize(batch), false);
        if (!n) { SDL_Delay(4); continue; }
        if (SDL_GetAtomicInt(&L.held_off)) {
            // A dictation by hotkey is recording these same words.
            turn = (eh_turn){ 0 };
            t.frames = t.speech = lead_count = held = 0;
            continue;
        }
        SDL_memcpy(pending + held, batch, sizeof(float) * (size_t)n);
        held += n;
        int used = 0;
        for (; held - used >= FRAME; used += FRAME) {
            const float *frame = pending + used;
            float heard[FRAME];
            SDL_memcpy(heard, frame, sizeof heard);
            gs_voice_frame(voice, heard);
            float probability = gs_voice_speech(voice);
            int step = eh_turn_step(&turn, probability);
            if (!turn.speaking && step == 0) {
                SDL_memcpy(lead + (size_t)lead_at * FRAME, frame, sizeof(float) * FRAME);
                lead_at = (lead_at + 1) % LEAD_FRAMES;
                lead_count = SDL_min(lead_count + 1, LEAD_FRAMES);
                continue;
            }
            if (step > 0) {
                // The stretch begins with what was heard just before it was noticed.
                for (int i = 0; i < lead_count; i++) take_add(&t, lead + (size_t)((lead_at + LEAD_FRAMES - lead_count + i) % LEAD_FRAMES) * FRAME);
                t.speech = EH_TURN_START_FRAMES - 1;
                lead_count = 0;
                eh_lock();
                eh_spawn(warm, NULL);
                eh_unlock();
            }
            take_add(&t, frame);
            if (probability >= 0.5f) t.speech++;
            if (step < 0) {
                take_finish(&t, EH_TURN_STOP_FRAMES);
            } else if (t.frames >= L.max_frames) {
                // Speech without a pause for as long as a dictation may last: cut here, and go on.
                take_finish(&t, 0);
                turn = (eh_turn){ 0 };
            }
        }
        held -= used;
        SDL_memmove(pending, pending + used, sizeof(float) * (size_t)held);
    }
    SDL_free(t.samples);
    gs_voice_free(voice);
    return 0;
}

void eh_listen_stop(void) {
    if (!L.thread) return;
    SDL_Thread *thread = L.thread;
    L.thread = NULL;
    SDL_SetAtomicInt(&L.stop, 1);
    EH_AWAIT(SDL_WaitThread(thread, NULL));
    eh_mic_close(L.mic);
    L.mic = NULL;
    SDL_Log("always-on: stopped listening");
    for (int i = 0; i < L.waiting_count; i++) SDL_free(L.waiting[i].wav);
    L.waiting_count = 0;
}

bool eh_listen_start(const char *device_id, double max_seconds, char *error, size_t error_size) {
    if (!device_id) device_id = "";
    if (L.thread && !SDL_strcmp(L.device, device_id)) return true;
    eh_listen_stop();
    L.mic = eh_mic_open(device_id, true, NULL, 0, error, error_size);
    if (!L.mic) return false;
    SDL_strlcpy(L.device, device_id, sizeof L.device);
    L.max_frames = (int)(SDL_clamp(max_seconds, 10, 3600) * 100);
    SDL_SetAtomicInt(&L.stop, 0);
    L.thread = SDL_CreateThread(listen_main, "listener", NULL);
    SDL_Log("always-on: listening on %s", *device_id ? device_id : "the default microphone");
    return true;
}
