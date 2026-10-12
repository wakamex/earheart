#include "preview.h"
#include <SDL3/SDL.h>
#include "loop.h"
#include "settings.h"
#include "text.h"
#include "wav.h"

static struct {
    eh_preview_deps deps;
    // A count and not a flag: a committed chunk's decode may overlap one in progress, and two
    // finishing out of order must not free the busy state early.
    int stt_in_flight;
    bool cleanup_busy;
    eh_signal *signal;      // aborts partial work when recording ends; made on the first chunk
    char *last_error;       // the last failure reported, so a persistent one is logged once
    char *committed_raw;    // text of finalized chunks, never decoded again
    char *live_raw;         // decode of the chunk in progress, replaced on each tick
    int last_seq;           // the highest chunk committed
    int decoded_samples;    // committed_raw covers exactly this much of the recording's start,
    bool broken;            // unless broken
    eh_salvage *chunks;
    int chunk_count;
    char *last_cleaned_raw; // the committed text the last cleanup pass consumed
    unsigned pause_timer;   // fires a cleanup pass once a chunk has committed and settled
    void *pending_pass;     // what that timer will run with
} P = { .last_seq = -1 };

void eh_preview_init(const eh_preview_deps *deps) { P.deps = *deps; }

static void set(char **slot, char *value) {
    SDL_free(*slot);
    *slot = value;
}

static const char *str(const char *s) { return s ? s : ""; }

static void reset(void) {
    set(&P.committed_raw, NULL);
    set(&P.live_raw, NULL);
    set(&P.last_cleaned_raw, NULL);
    P.last_seq = -1;
    P.decoded_samples = 0;
    P.broken = false;
    for (int i = 0; i < P.chunk_count; i++) SDL_free((char *)P.chunks[i].text);
    SDL_free(P.chunks);
    P.chunks = NULL;
    P.chunk_count = 0;
}

typedef struct { unsigned sid; gs_jval *settings; eh_signal *signal; } pass;
static void drop_pending_pass(void);

void eh_preview_cancel(void) {
    if (P.signal) {
        eh_signal_abort(P.signal);
        eh_signal_unref(P.signal);
        P.signal = NULL;
    }
    drop_pending_pass();
    // cleanup_busy is not cleared: a cleanup in flight clears it itself, and clearing it here
    // would let a new session start a second one on the worker before the old returns. The decode
    // count is cleared, so the next session's preview is not gated on a slow decode from this one.
    P.stt_in_flight = 0;
    reset();
}

eh_preview_snapshot eh_preview_snapshot_final(void) {
    eh_preview_snapshot s = { eh_trim(str(P.committed_raw)), P.decoded_samples, P.broken, NULL, P.chunk_count };
    s.chunks = SDL_calloc((size_t)P.chunk_count + 1, sizeof *s.chunks);
    for (int i = 0; i < P.chunk_count; i++) s.chunks[i] = (eh_salvage){ P.chunks[i].from, P.chunks[i].to, SDL_strdup(P.chunks[i].text) };
    return s;
}

void eh_preview_snapshot_free(eh_preview_snapshot *s) {
    for (int i = 0; i < s->chunk_count; i++) SDL_free((char *)s->chunks[i].text);
    SDL_free(s->chunks);
    SDL_free(s->committed_raw);
    *s = (eh_preview_snapshot){ 0 };
}

// A partial is stale the moment its session ends, recording stops, or its work is aborted.
static bool stale(unsigned sid, const eh_signal *signal) { return !P.deps.is_current(sid) || eh_signal_aborted(signal); }

static void push_raw(void) {
    char *text = eh_join_text(P.committed_raw, P.live_raw);
    P.deps.send("raw", text);
    SDL_free(text);
}

static void add_chunk(int from, int to, const char *text) {
    P.chunks = SDL_realloc(P.chunks, sizeof *P.chunks * (size_t)(P.chunk_count + 1));
    P.chunks[P.chunk_count++] = (eh_salvage){ from, to, SDL_strdup(text) };
}

/* ---------------- cleanup of the committed text ---------------- */

static void schedule_cleanup(unsigned sid, const gs_jval *settings, eh_signal *signal);

static void free_pass(pass *p) {
    gs_jval_free(p->settings);
    eh_signal_unref(p->signal);
    SDL_free(p);
}

static void drop_pending_pass(void) {
    eh_cancel(P.pause_timer);
    P.pause_timer = 0;
    if (P.pending_pass) free_pass(P.pending_pass);
    P.pending_pass = NULL;
}

// Re-cleans the whole committed transcript and replaces the cleaned line with the result.
static void cleanup_pass(void *user) {
    pass *p = user;
    P.pause_timer = 0;
    P.pending_pass = NULL;
    char *target = eh_trim(str(P.committed_raw));
    if (stale(p->sid, p->signal) || P.cleanup_busy || !*target || !SDL_strcmp(target, str(P.last_cleaned_raw))) {
        SDL_free(target);
        free_pass(p);
        return;
    }
    P.cleanup_busy = true;
    char error[256];
    char *cleaned = P.deps.clean(target, gs_jval_get(p->settings, "cleanup"), p->signal, error, sizeof error);
    // A failed pass is cosmetic: the raw tail stays showing, and the next pause cleans again.
    if (cleaned && !stale(p->sid, p->signal)) {
        char *text = eh_trim(cleaned);
        // Sent first, then marked cleaned. An empty result still counts, so a chunk of nothing
        // but fillers is not cleaned for ever.
        if (*text) P.deps.send("cleaned", text);
        SDL_free(text);
        set(&P.last_cleaned_raw, target);
        target = NULL;
    }
    SDL_free(cleaned);
    SDL_free(target);
    P.cleanup_busy = false;
    // More may have committed meanwhile; if so, clean again after the next pause.
    char *now = eh_trim(str(P.committed_raw));
    if (!stale(p->sid, p->signal) && SDL_strcmp(now, str(P.last_cleaned_raw))) schedule_cleanup(p->sid, p->settings, p->signal);
    SDL_free(now);
    free_pass(p);
}

static void schedule_cleanup(unsigned sid, const gs_jval *settings, eh_signal *signal) {
    // Live cleaning only when cleanup is on and runs on this computer: a remote service is not
    // to be hit over and over mid-dictation. The final pass cleans whatever the engine.
    if (!gs_jval_boolean(gs_jval_path(settings, "cleanup.enabled"), false) || SDL_strcmp(gs_jval_string(gs_jval_path(settings, "cleanup.engine"), ""), "builtin")) return;
    drop_pending_pass();
    pass *p = SDL_calloc(1, sizeof *p);
    p->sid = sid;
    p->settings = gs_jval_clone(settings);
    p->signal = eh_signal_ref(signal);
    double pause_ms = gs_jval_number(gs_jval_path(settings, "stt.livePreview.cleanupPauseMs"), 0);
    P.pending_pass = p;
    P.pause_timer = eh_after(pause_ms > 0 ? (int)pause_ms : 1000, cleanup_pass, p);
}

/* ---------------- chunks ---------------- */

typedef struct {
    unsigned sid;
    int seq, from_sample, has_speech;
    bool final;
    uint8_t *wav;
    size_t len;
    gs_jval *cfg;
    eh_signal *signal;
} chunk;

static void handle_audio(void *user) {
    chunk *c = user;
    gs_jval *cfg = c->cfg;
    eh_signal *signal = c->signal;
    // The display is optional; the chunk decodes that feed the final assembly are not.
    bool display = gs_jval_boolean(gs_jval_path(cfg, "stt.livePreview.enabled"), false);
    char error[256] = "", *partial_text = NULL;
    char *raw = P.deps.transcribe(c->wav, c->len, gs_jval_get(cfg, "stt"), signal, &partial_text, error, sizeof error);
    int frames = eh_wav_frames(c->wav, c->len);
    if (raw && !stale(c->sid, signal)) {
        set(&P.last_error, NULL);  // a decode succeeded; let the next failure surface
        char *text = eh_trim(raw);
        if (c->final && c->seq > P.last_seq) {
            // Freeze this chunk into the committed transcript, keeping any painted words its
            // boundary stopped short of, so the preview only ever grows.
            P.last_seq = c->seq;
            set(&P.committed_raw, eh_join_text(P.committed_raw, text));
            set(&P.live_raw, eh_residual_tail(str(P.live_raw), text));
            // A chunk the recorder heard speech in that decodes to nothing is a hole by another
            // route: its samples would count as decoded and the words in them be gone for good.
            // Only an explicit "no speech" buys that coverage.
            bool lost_words = !*text && c->has_speech != 0;
            if (*text && c->from_sample >= 0 && frames > 0) add_chunk(c->from_sample, c->from_sample + frames, text);
            // Contiguous coverage only: the chunk must start exactly where decoded coverage ends.
            if (!P.broken && !lost_words && c->from_sample == P.decoded_samples && frames > 0) P.decoded_samples = c->from_sample + frames;
            else P.broken = true;
            if (display) {
                push_raw();
                schedule_cleanup(c->sid, cfg, signal);
            }
        } else if (!c->final && c->seq > P.last_seq && SDL_strcmp(text, str(P.live_raw))) {
            // The chunk in progress: replace the live tail. A late result for a chunk already
            // committed must not bring its text back.
            set(&P.live_raw, SDL_strdup(text));
            if (display) push_raw();
        }
        SDL_free(text);
    } else if (!raw) {
        // A failed partial is cosmetic, but a failed final chunk means the committed transcript is
        // missing words, so the final pass must decode everything. Only for a chunk that still
        // belongs to this recording: the state is shared across dictations, and a decode failing
        // after its own session ended would otherwise mark the next one broken for nothing.
        if (c->final && !stale(c->sid, signal)) {
            P.broken = true;
            char *text = eh_trim(str(partial_text));
            // Some words were recovered, but not enough to trust as contiguous coverage: they are
            // kept as salvage for exactly their range, once per chunk.
            if (*text && c->seq > P.last_seq && c->from_sample >= 0 && frames > 0) {
                P.last_seq = c->seq;
                set(&P.committed_raw, eh_join_text(P.committed_raw, text));
                add_chunk(c->from_sample, c->from_sample + frames, text);
                set(&P.live_raw, eh_residual_tail(str(P.live_raw), text));
                if (display) push_raw();
            }
            SDL_free(text);
        }
        // Reported once per distinct message: a blank preview is almost always an error surfaced
        // here (the model still loading, not downloaded, or a decode failing).
        if (SDL_strcmp(error, str(P.last_error))) {
            set(&P.last_error, SDL_strdup(error));
            if (P.deps.on_error) P.deps.on_error(error);
        }
    }
    // A decode from a torn-down session must not lower the next session's count.
    if (!stale(c->sid, signal)) P.stt_in_flight = SDL_max(0, P.stt_in_flight - 1);
    SDL_free(raw);
    SDL_free(partial_text);
    eh_signal_unref(signal);
    gs_jval_free(cfg);
    SDL_free(c->wav);
    SDL_free(c);
}

void eh_preview_audio(unsigned sid, int seq, bool final, int from_sample, int has_speech, uint8_t *wav, size_t len) {
    const gs_jval *live = eh_settings();
    // Drop-if-busy applies to in-progress ticks only. A final chunk is sent exactly once, and
    // dropping it would punch a hole in the committed transcript, so it always decodes.
    if (!P.deps.is_current(sid) || SDL_strcmp(gs_jval_string(gs_jval_path(live, "stt.engine"), ""), "builtin") || (P.stt_in_flight > 0 && !final)) {
        SDL_free(wav);
        return;
    }
    if (!P.signal) P.signal = eh_signal_new();
    P.stt_in_flight++;
    chunk *c = SDL_calloc(1, sizeof *c);
    *c = (chunk){ sid, seq, from_sample, has_speech, final, wav, len, gs_jval_clone(live), eh_signal_ref(P.signal) };
    // The decode is a task of its own; tasks start in the order they are spawned, so chunks reach
    // the engine in the order they were recorded.
    eh_spawn(handle_audio, c);
}
