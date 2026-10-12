#include "recorder.h"
#include <SDL3/SDL.h>
#include <math.h>
#include "audio.h"
#include "loop.h"
#include "wav.h"

#define BATCH 512                   // 32 ms at 16 kHz: how often audio reaches the recording
#define LEVELS 24
#define MIC_START_TIMEOUT_MS 10000  // a microphone that delivers nothing within this is reported
#define QUIET_WINDOW_SEC 0.3        // a chunk is committed when its last 0.3 s are quiet,
#define BOUNDARY_SEARCH_SEC 3       // and a forced boundary moves back to the quietest moment of the last 3 s
#define BOUNDARY_HOP_SEC 0.05

typedef struct recording {
    unsigned sid;
    float *samples;
    int count, cap;
    Uint64 started_at;  // 0 until the first audio arrives
    Uint64 paused_at;   // 0 while not paused
    Uint64 paused_ms;
    int committed;      // samples already sent as committed chunks
    int seq;
    int max_seconds;
    bool chunks, display;
    int interval_ms;
    double chunk_seconds;
    unsigned partial_timer, max_timer, watchdog;
    char notice[96];
    SDL_Thread *source;
    SDL_AtomicInt stop;  // tells the source thread to finish
    float levels[LEVELS];
} recording;

static struct {
    eh_recorder_deps deps;
    recording *rec;
    char *fake_path;
    double fake_speed;
} R;

void eh_recorder_init(const eh_recorder_deps *deps) { R.deps = *deps; }

void eh_recorder_fake_microphone(const char *wav_path, double speed) {
    SDL_free(R.fake_path);
    R.fake_path = wav_path ? SDL_strdup(wav_path) : NULL;
    R.fake_speed = speed > 0 ? speed : 1;
}

bool eh_recorder_active(void) { return R.rec != NULL; }
bool eh_recorder_paused(void) { return R.rec && R.rec->paused_at; }
int eh_recorder_cap_seconds(void) { return R.rec ? R.rec->max_seconds : 0; }

static Uint64 captured_ms(const recording *rec) {
    if (!rec->started_at) return 0;
    return (rec->paused_at ? rec->paused_at : SDL_GetTicks()) - rec->started_at - rec->paused_ms;
}

double eh_recorder_seconds(void) { return R.rec ? (double)captured_ms(R.rec) / 1000 : 0; }

int eh_recorder_levels(float *out, int max) {
    if (!R.rec) return 0;
    int n = SDL_min(max, LEVELS);
    SDL_memcpy(out, R.rec->levels + LEVELS - n, sizeof(float) * (size_t)n);
    return n;
}

/* ---------------- the microphone ---------------- */

static void max_reached(void *user);

// With the lock held: one batch from the microphone joins the recording.
static void take_batch(recording *rec, const float *batch, int n) {
    if (R.rec != rec || rec->paused_at) return;
    if (rec->count + n > rec->cap) rec->samples = SDL_realloc(rec->samples, sizeof(float) * (size_t)(rec->cap = (rec->cap + n) * 2));
    SDL_memcpy(rec->samples + rec->count, batch, sizeof(float) * (size_t)n);
    rec->count += n;
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)batch[i] * batch[i];
    SDL_memmove(rec->levels, rec->levels + 1, sizeof(float) * (LEVELS - 1));
    rec->levels[LEVELS - 1] = (float)sqrt(sum / n);
    if (!rec->started_at) {
        // The microphone is live: the clock starts here, so the timer and the cap count audio.
        rec->started_at = SDL_GetTicks();
        eh_cancel(rec->watchdog);
        rec->watchdog = 0;
        rec->max_timer = eh_after(rec->max_seconds * 1000, max_reached, NULL);
        R.deps.live(rec->sid, rec->notice);
    }
}

struct eh_mic {
    SDL_AudioStream *stream;  // NULL when a file stands in
    float *fake;
    int fake_count, at;
    double speed;
    Uint64 began;
    bool then_silence;
};

eh_mic *eh_mic_open(const char *device_id, bool then_silence, char *notice, size_t notice_size, char *error, size_t error_size) {
    eh_mic *m = SDL_calloc(1, sizeof *m);
    m->speed = R.fake_speed;
    m->then_silence = then_silence;
    if (R.fake_path) {
        size_t len;
        uint8_t *wav = SDL_LoadFile(R.fake_path, &len);
        const char *why = wav ? NULL : SDL_GetError();
        int rate = 0;
        m->fake = wav ? eh_wav_floats(wav, len, &m->fake_count, &rate, &why) : NULL;
        SDL_free(wav);
        if (m->fake && rate != EH_SAMPLE_RATE) { why = "the file is not 16 kHz"; SDL_free(m->fake); m->fake = NULL; }
        if (!m->fake) { SDL_snprintf(error, error_size, "Microphone unavailable: %s", why); SDL_free(m); return NULL; }
    } else {
        if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_snprintf(error, error_size, "Microphone unavailable: %s", SDL_GetError());
            SDL_free(m);
            return NULL;
        }
        // The saved microphone by its name; one that is gone falls back to the system's default,
        // and the overlay says so.
        SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
        if (device_id && *device_id) {
            int count = 0;
            SDL_AudioDeviceID *all = SDL_GetAudioRecordingDevices(&count);
            bool found = false;
            for (int i = 0; all && i < count && !found; i++) {
                const char *name = SDL_GetAudioDeviceName(all[i]);
                if (name && !SDL_strcmp(name, device_id)) { device = all[i]; found = true; }
            }
            SDL_free(all);
            if (!found && notice) SDL_strlcpy(notice, "Saved mic missing — using system default", notice_size);
        }
        const SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, EH_SAMPLE_RATE };
        m->stream = SDL_OpenAudioDeviceStream(device, &spec, NULL, NULL);
        if (!m->stream || !SDL_ResumeAudioStreamDevice(m->stream)) {
            SDL_snprintf(error, error_size, "Microphone unavailable: %s", SDL_GetError());
            if (m->stream) SDL_DestroyAudioStream(m->stream);
            SDL_free(m);
            return NULL;
        }
    }
    m->began = SDL_GetTicksNS();
    return m;
}

int eh_mic_read(eh_mic *m, float *batch, int max, bool stopping) {
    if (m->stream) {
        int got = SDL_GetAudioStreamData(m->stream, batch, (int)sizeof(float) * max);
        return got > 0 ? got / (int)sizeof(float) : 0;
    }
    // The file is played at its own pace, so the recording grows as a spoken one does.
    Uint64 due = m->began + (Uint64)((double)(m->at + max) / EH_SAMPLE_RATE / m->speed * 1e9);
    if (m->at < m->fake_count) {
        if (!stopping && SDL_GetTicksNS() < due) return 0;
        int n = SDL_min(max, m->fake_count - m->at);
        SDL_memcpy(batch, m->fake + m->at, sizeof(float) * (size_t)n);
        m->at += n;
        return stopping ? 0 : n;  // a file has nothing on its way
    }
    if (!m->then_silence || stopping || SDL_GetTicksNS() < due) return 0;
    SDL_memset(batch, 0, sizeof(float) * (size_t)max);
    m->at += max;
    return max;
}

void eh_mic_close(eh_mic *m) {
    if (!m) return;
    if (m->stream) SDL_DestroyAudioStream(m->stream);
    SDL_free(m->fake);
    SDL_free(m);
}

typedef struct { recording *rec; eh_mic *mic; } source;

// Feeds the recording until told to stop, then once more with whatever was still on its way.
static int source_main(void *user) {
    source s = *(source *)user;
    SDL_free(user);
    float batch[BATCH];
    for (;;) {
        bool stopping = SDL_GetAtomicInt(&s.rec->stop);
        int n = eh_mic_read(s.mic, batch, BATCH, stopping);
        if (n) {
            eh_lock();
            take_batch(s.rec, batch, n);
            eh_unlock();
        } else if (stopping) {
            break;
        } else {
            SDL_Delay(4);
        }
    }
    eh_mic_close(s.mic);
    return 0;
}

// Opens the microphone, or the file standing in for it. False with `error` set.
static bool open_source(recording *rec, const char *device_id, char *error, size_t error_size) {
    eh_mic *mic = eh_mic_open(device_id, false, rec->notice, sizeof rec->notice, error, error_size);
    if (!mic) return false;
    source *s = SDL_calloc(1, sizeof *s);
    s->rec = rec;
    s->mic = mic;
    rec->source = SDL_CreateThread(source_main, "microphone", s);
    return true;
}

/* ---------------- chunks for the live preview ---------------- */

static void send_partial(void *user) {
    recording *rec = R.rec;
    if (!rec || !rec->chunks || rec->paused_at) return;
    int total = rec->count, from = rec->committed;
    if (total <= from) return;  // nothing new recorded since the last commit
    int chunk = (int)((rec->chunk_seconds > 0 ? rec->chunk_seconds : 10) * EH_SAMPLE_RATE), live = total - from;
    int window = (int)floor(QUIET_WINDOW_SEC * EH_SAMPLE_RATE);
    // A chunk is committed once it is long enough and the speaker pauses; one that runs to twice
    // the length without a pause is cut anyway.
    double sum = 0;
    int take = SDL_min(window, total);
    for (int i = total - take; i < total; i++) sum += (double)rec->samples[i] * rec->samples[i];
    bool paused = live >= chunk && (take > 0 ? sqrt(sum / take) : 0) < EH_QUIET_RMS;
    bool forced = !paused && live >= chunk * 2;
    bool final = paused || forced;
    if (!final && !rec->display) return;
    int boundary = total;
    if (forced) {
        // A cut through a word costs that word twice, in this chunk and the next, and the
        // committed text is reused verbatim in the final transcript. So the cut goes to the end of
        // the quietest window of the last few seconds.
        int search_from = SDL_max(from + chunk, total - BOUNDARY_SEARCH_SEC * EH_SAMPLE_RATE);
        boundary = search_from + eh_quietest_offset(rec->samples + search_from, total - search_from, window, (int)floor(BOUNDARY_HOP_SEC * EH_SAMPLE_RATE));
    }
    int n = (final ? boundary : total) - from;
    // Only a committed chunk is assembled into the final transcript, so only it is worth probing.
    bool has_speech = final && eh_contains_speech(rec->samples + from, n, EH_SAMPLE_RATE);
    size_t len;
    uint8_t *wav = eh_wav_encode_recorded(rec->samples + from, n, EH_SAMPLE_RATE, &len);
    unsigned sid = rec->sid;
    int seq = rec->seq;
    if (final) {
        rec->committed = boundary;
        rec->seq++;
    }
    R.deps.partial(sid, seq, final, from, has_speech, wav, len);
}

/* ---------------- start, stop, pause ---------------- */

// Ends the recording in progress and hands it back, with the tail the microphone still held when
// `collect_tail`. The caller frees it.
static recording *teardown(bool collect_tail) {
    recording *rec = R.rec;
    if (!rec) return NULL;
    eh_cancel(rec->partial_timer);
    eh_cancel(rec->max_timer);
    eh_cancel(rec->watchdog);
    if (rec->source) {
        // While the source finishes, R.rec still names this recording when its tail is wanted, so
        // the last batches join it; otherwise they are dropped.
        if (!collect_tail) R.rec = NULL;
        SDL_SetAtomicInt(&rec->stop, 1);
        EH_AWAIT(SDL_WaitThread(rec->source, NULL));
    }
    if (R.rec == rec) R.rec = NULL;
    return rec;
}

static void free_recording(recording *rec) {
    SDL_free(rec->samples);
    SDL_free(rec);
}

static void watchdog(void *user) {
    recording *rec = R.rec;
    if (!rec || rec->started_at) return;
    rec->watchdog = 0;
    unsigned sid = rec->sid;
    free_recording(teardown(false));
    R.deps.error(sid, "Microphone did not deliver audio in time — check the input device in Settings");
}

void eh_recorder_start(unsigned sid, const char *device_id, double max_seconds, bool chunks, bool display, const gs_jval *live_preview) {
    recording *old = teardown(false);
    if (old) free_recording(old);
    recording *rec = SDL_calloc(1, sizeof *rec);
    rec->sid = sid;
    double seconds = isfinite(max_seconds) ? floor(max_seconds + 0.5) : 600;
    rec->max_seconds = seconds >= 10 && seconds <= 3600 ? (int)seconds : 600;
    rec->chunks = chunks;
    rec->display = display;
    double interval = gs_jval_number(gs_jval_get(live_preview, "intervalMs"), 0);
    rec->interval_ms = interval > 0 ? (int)interval : 1200;
    rec->chunk_seconds = gs_jval_number(gs_jval_get(live_preview, "chunkSeconds"), 0);
    char error[256];
    R.rec = rec;
    if (!open_source(rec, device_id, error, sizeof error)) {
        R.rec = NULL;
        free_recording(rec);
        R.deps.error(sid, error);
        return;
    }
    rec->watchdog = eh_after(MIC_START_TIMEOUT_MS, watchdog, NULL);
    if (chunks) rec->partial_timer = eh_every(rec->interval_ms, send_partial, NULL);
}

void eh_recorder_stop(void) {
    recording *rec = R.rec;
    if (!rec) return;
    bool was_live = rec->started_at != 0;
    unsigned sid = rec->sid;
    rec = teardown(was_live);
    if (!was_live) {
        // The microphone never went live: nothing captured, nothing to transcribe.
        free_recording(rec);
        R.deps.cancelled(sid);
        return;
    }
    size_t len;
    uint8_t *wav = eh_wav_encode_recorded(rec->samples, rec->count, EH_SAMPLE_RATE, &len);
    free_recording(rec);
    R.deps.captured(sid, wav, len);
}

static void max_reached(void *user) {
    if (R.rec) R.rec->max_timer = 0;
    eh_recorder_stop();
}

void eh_recorder_cancel(void) {
    recording *rec = R.rec;
    if (!rec) return;
    unsigned sid = rec->sid;
    free_recording(teardown(false));
    R.deps.cancelled(sid);
}

void eh_recorder_pause_toggle(void) {
    recording *rec = R.rec;
    if (!rec || !rec->started_at) return;
    if (!rec->paused_at) {
        rec->paused_at = SDL_GetTicks();
        eh_cancel(rec->max_timer);
        rec->max_timer = 0;
        R.deps.paused(rec->sid, true);
        return;
    }
    rec->paused_ms += SDL_GetTicks() - rec->paused_at;
    rec->paused_at = 0;
    Sint64 remaining = (Sint64)rec->max_seconds * 1000 - (Sint64)captured_ms(rec);
    if (remaining <= 0) { eh_recorder_stop(); return; }
    rec->max_timer = eh_after((int)remaining, max_reached, NULL);
    R.deps.paused(rec->sid, false);
}
