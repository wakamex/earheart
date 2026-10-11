#include "chunked.h"
#include <SDL3/SDL.h>
#include <math.h>
#include "audio.h"
#include "wav.h"

#define MAX_DECODE_SECONDS 20
#define MIN_SOUND_SEC 0.15     // shorter sound with quiet around it is a click, a tap, the hotkey
#define MIN_UTTERANCE_SEC 2    // a stretch with less speech joins its nearest neighbour
#define KEEP_PAUSE_SEC 0.5     // inside a merged piece, pauses reach the decoder this long at most
#define MAX_CONSECUTIVE_FAILURES 2  // after this many dead pieces in a row the worker is not coming back
#define EMPTY_RETRY_PAD_SEC 0.25    // silence put around a piece that heard speech and decoded to nothing
#define EMPTY_SPEECH_MAX_SEC 2      // a longer empty piece is a failure at once

char *eh_join_text(const char *a, const char *b) {
    if (!a || !*a) return SDL_strdup(b ? b : "");
    if (!b || !*b) return SDL_strdup(a);
    char *s = NULL;
    SDL_asprintf(&s, "%s %s", a, b);
    return s;
}

static char *trimmed(char *s) {
    if (!s) return SDL_strdup("");
    char *from = s;
    while (SDL_isspace((unsigned char)*from)) from++;
    size_t n = SDL_strlen(from);
    while (n && SDL_isspace((unsigned char)from[n - 1])) n--;
    char *out = SDL_strndup(from, n);
    SDL_free(s);
    return out;
}

static int compare_ints(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

typedef struct { int *v; int n, cap; } ints;

static void add_cut(ints *a, int x) {
    for (int i = 0; i < a->n; i++)
        if (a->v[i] == x) return;
    if (a->n == a->cap) a->v = SDL_realloc(a->v, sizeof(int) * (size_t)(a->cap = a->cap * 2 + 16));
    a->v[a->n++] = x;
}

static bool retryable(eh_decode_code c) { return c == EH_DECODE_EXITED || c == EH_DECODE_TIMEOUT; }
static bool is_gap(const eh_piece *p) { return !p->ok || p->unconfirmed; }

// The salvage chunk that covers a piece, among the chunks in recording order without overlaps (an
// overlapping one, only possible from out-of-order commits, is dropped and not repeated).
static const eh_salvage *owner_of(const eh_piece *p, const eh_salvage **ordered, int count) {
    for (int i = 0; i < count; i++)
        if (ordered[i]->from <= p->from && p->to <= ordered[i]->to) return ordered[i];
    return NULL;
}

static int compare_salvage(const void *a, const void *b) { return (*(const eh_salvage *const *)a)->from - (*(const eh_salvage *const *)b)->from; }

// The pieces' text in recording order. A chunk that covers a gap stands in for every piece under
// it: the plan cut at its boundaries, so those pieces lie inside it and its words replace theirs.
static char *assemble(const eh_piece *pieces, int count, const eh_salvage **owners) {
    char *text = SDL_strdup("");
    for (int i = 0; i < count; i++) {
        const eh_salvage *c = owners[i];
        bool stands_in = false;
        for (int j = 0; c && j < count && !stands_in; j++) stands_in = owners[j] == c && is_gap(&pieces[j]);
        const char *add = NULL;
        if (stands_in) {
            if (i == 0 || owners[i - 1] != c) add = c->text;
        } else if (!is_gap(&pieces[i])) {
            add = pieces[i].text;
        }
        if (add) {
            char *joined = eh_join_text(text, add);
            SDL_free(text);
            text = joined;
        }
    }
    return text;
}

void eh_chunked_result_free(eh_chunked_result *r) {
    for (int i = 0; i < r->piece_count; i++) SDL_free(r->pieces[i].text);
    SDL_free(r->pieces);
    SDL_free(r->text);
    *r = (eh_chunked_result){ 0 };
}

bool eh_transcribe_chunked(const uint8_t *wav, size_t len, const eh_chunked *o, eh_chunked_result *r) {
    *r = (eh_chunked_result){ 0 };
    eh_split shape = EH_SPLIT_DEFAULTS;
    shape.max_sec = o->max_sec > 0 ? o->max_sec : MAX_DECODE_SECONDS;
    if (o->min_pause_sec != 0) shape.min_pause_sec = o->min_pause_sec;
    shape.min_sound_sec = o->min_sound_sec < 0 ? MIN_SOUND_SEC : o->min_sound_sec;
    shape.min_utterance_sec = o->min_utterance_sec < 0 ? MIN_UTTERANCE_SEC : o->min_utterance_sec;
    shape.keep_pause_sec = o->keep_pause_sec != 0 ? o->keep_pause_sec : KEEP_PAUSE_SEC;
    bool stale = false;
#define STALE() (o->stale && o->stale(o->user))

    // The plan: one range per stretch of speech, and the samples they index. Committed chunk
    // boundaries are cut points too, so every salvage chunk covers whole pieces. Such a cut can
    // land inside a merged piece's long pause, where the decoder will not squeeze the half left at
    // the piece's edge, so every range is held to the cap again.
    int frames = eh_wav_frames(wav, len), count = 0, rate = EH_SAMPLE_RATE;
    float *samples = NULL;
    ints natural = { 0 }, cuts = { 0 };
    if (frames) {
        const char *error = NULL;
        samples = eh_wav_floats(wav, len, &count, &rate, &error);
        if (!samples) {
            r->code = EH_DECODE_FAILED;
            SDL_strlcpy(r->error, error, sizeof r->error);
            return false;
        }
        natural.v = eh_split_points(samples, count, rate, &shape, &natural.n);
        for (int i = 0; i < natural.n; i++) add_cut(&cuts, natural.v[i]);
        for (int i = 0; i < o->salvage_count; i++) {
            if (o->salvage[i].from > 0 && o->salvage[i].from < frames) add_cut(&cuts, o->salvage[i].from);
            if (o->salvage[i].to > 0 && o->salvage[i].to < frames) add_cut(&cuts, o->salvage[i].to);
        }
        if (o->salvage_count) {
            SDL_qsort(cuts.v, (size_t)cuts.n, sizeof(int), compare_ints);
            int before = cuts.n, from = 0;
            for (int i = 0; i <= before; i++) {
                int to = i == before ? frames : cuts.v[i], n;
                int *more = eh_fit_cap(samples, rate, from, to, &shape, &n);
                for (int k = 0; k < n; k++) add_cut(&cuts, more[k]);
                SDL_free(more);
                from = to;
            }
        }
        SDL_qsort(cuts.v, (size_t)cuts.n, sizeof(int), compare_ints);
    }
    int n = cuts.n + 1;
    eh_piece *pieces = SDL_calloc((size_t)n, sizeof(eh_piece));
    for (int i = 0; i < n; i++) {
        pieces[i].from = i ? cuts.v[i - 1] : 0;
        pieces[i].to = i == cuts.n ? frames : cuts.v[i];
        // The pause or cap range a piece came from: a dead worker is counted once per group, so
        // salvage cuts cannot make one bad stretch look like two.
        for (int k = 0; k < natural.n; k++) pieces[i].group += natural.v[k] <= pieces[i].from;
    }
    SDL_free(natural.v);
    SDL_free(cuts.v);
    r->pieces = pieces;
    r->piece_count = n;

    int consecutive_failures = 0, last_failed_group = -1;
    bool have_error = false;
    for (int i = 0; i < n && !stale; i++) {
        eh_piece *p = &pieces[i];
        if (STALE()) { stale = true; break; }
        if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) { p->skipped = true; continue; }
        const float *piece_samples = samples ? samples + p->from : NULL;
        int piece_count = p->to - p->from;
        // Nothing a word could be made of, only noise or silence: the model would invent a word
        // from it, so it is not decoded at all. The speech probe has the last say, so a low-gain
        // voice still reaches the decoder. A recording that is one piece is always decoded: its
        // answer, or its error (a model not yet downloaded), is the dictation's.
        if (shape.min_sound_sec > 0 && n > 1 && eh_sound_seconds(piece_samples, piece_count, EH_SAMPLE_RATE, &shape) < shape.min_sound_sec &&
            !eh_contains_speech(piece_samples, piece_count, EH_SAMPLE_RATE)) {
            p->ok = p->noise = true;
            p->text = SDL_strdup("");
            continue;
        }
        // A merged piece's long inner pauses are shortened for the decoder; any other goes as recorded.
        int decode_count = piece_count;
        float *decode_samples = samples ? eh_squeeze_pauses(piece_samples, piece_count, EH_SAMPLE_RATE, &shape, &decode_count) : NULL;
        bool squeezed = decode_samples != piece_samples;
        uint8_t *owned = NULL;
        const uint8_t *piece_wav = wav;
        size_t piece_len = len;
        if (squeezed) {
            int16_t *pcm = eh_wav_pcm16(decode_samples, decode_count, 0);
            piece_wav = owned = eh_wav_encode(pcm, decode_count, EH_SAMPLE_RATE, &piece_len);
            SDL_free(pcm);
        } else if (n > 1) {
            piece_wav = owned = eh_wav_slice(wav, len, p->from, p->to, &piece_len);
        }
        for (;;) {
            p->attempts++;
            char *text = NULL, error[256] = "";
            int ms = -1;
            eh_decode_code code = o->run(o->user, piece_wav, piece_len, &text, &ms, error, sizeof error);
            if (code == EH_DECODE_OK && ms >= 0 && o->on_decode_ms) o->on_decode_ms(o->user, ms);
            if (code == EH_DECODE_OK) text = trimmed(text);
            if (code == EH_DECODE_OK && !*text && eh_contains_speech(piece_samples, piece_count, EH_SAMPLE_RATE)) {
                // Speech in, nothing out: one more try with room around it. A fragment cut out
                // mid-flow can come back once it has room to start and stop.
                int room = (int)floor((shape.max_sec * EH_SAMPLE_RATE - decode_count) / 2);
                int pad = SDL_min((int)floor(EMPTY_RETRY_PAD_SEC * EH_SAMPLE_RATE + 0.5), room);
                if (pad > 0) {
                    int16_t *pcm = eh_wav_pcm16(decode_samples, decode_count, pad);
                    size_t padded_len;
                    uint8_t *padded = eh_wav_encode(pcm, decode_count + 2 * pad, EH_SAMPLE_RATE, &padded_len);
                    SDL_free(pcm);
                    p->attempts++;
                    SDL_free(text);
                    text = NULL;
                    ms = -1;
                    code = o->run(o->user, padded, padded_len, &text, &ms, error, sizeof error);
                    SDL_free(padded);
                    if (code == EH_DECODE_OK && ms >= 0 && o->on_decode_ms) o->on_decode_ms(o->user, ms);
                    if (code == EH_DECODE_OK) text = trimmed(text);
                }
                if (code == EH_DECODE_OK && !*text) {
                    // Still empty: lost if it is long, else unconfirmed until a covering salvage
                    // chunk supplies its words during assembly.
                    if ((double)(p->to - p->from) / EH_SAMPLE_RATE > EMPTY_SPEECH_MAX_SEC) {
                        code = EH_DECODE_EMPTY_SPEECH;
                        SDL_strlcpy(error, "speech decoded to no text", sizeof error);
                    } else {
                        p->unconfirmed = true;
                        SDL_Log("STT decode: piece %.1f-%.1fs heard speech but decoded to no text; awaiting covering salvage",
                                (double)p->from / EH_SAMPLE_RATE, (double)p->to / EH_SAMPLE_RATE);
                    }
                }
            }
            if (code == EH_DECODE_OK) {
                p->ok = true;
                p->text = text;
                break;
            }
            SDL_free(text);
            if (STALE()) { stale = true; break; }
            SDL_Log("STT decode: piece %.1f-%.1fs attempt %d failed: %s", (double)p->from / EH_SAMPLE_RATE, (double)p->to / EH_SAMPLE_RATE, p->attempts, error);
            // A timed-out worker is still stuck in the native decode: it must be retired, or the
            // retry, and after a last attempt the next piece, queues behind it.
            if (code == EH_DECODE_TIMEOUT && o->restart_stt) o->restart_stt(o->user);
            if (p->attempts < 2 && retryable(code)) continue;
            p->code = code;
            SDL_strlcpy(p->error, error, sizeof p->error);
            if (!have_error) {
                have_error = true;
                r->code = code;
                SDL_strlcpy(r->error, error, sizeof r->error);
            }
            break;
        }
        if (squeezed) SDL_free(decode_samples);
        SDL_free(owned);
        // Only a worker that keeps dying stops the run, counted once per group.
        if (p->ok || !retryable(p->code)) {
            consecutive_failures = 0;
            last_failed_group = -1;
        } else if (p->group != last_failed_group) {
            consecutive_failures++;
            last_failed_group = p->group;
        }
    }
    SDL_free(samples);
    if (stale) {
        r->stale = true;
        r->text = SDL_strdup("");
        return true;
    }

    const eh_salvage **ordered = SDL_malloc(sizeof(*ordered) * (size_t)SDL_max(1, o->salvage_count));
    int ordered_count = 0;
    for (int i = 0; i < o->salvage_count; i++) ordered[i] = &o->salvage[i];
    SDL_qsort((void *)ordered, (size_t)o->salvage_count, sizeof(*ordered), compare_salvage);
    for (int i = 0; i < o->salvage_count; i++)
        if (!ordered_count || ordered[i]->from >= ordered[ordered_count - 1]->to) ordered[ordered_count++] = ordered[i];
    const eh_salvage **owners = SDL_malloc(sizeof(*owners) * (size_t)n);
    for (int i = 0; i < n; i++) owners[i] = owner_of(&pieces[i], ordered, ordered_count);

    char *decoded = assemble(pieces, n, owners);
    bool any_unconfirmed = false;
    for (int i = 0; i < n; i++) any_unconfirmed |= pieces[i].unconfirmed;
    if (!*decoded && any_unconfirmed) {
        // Speech heard and no text anywhere: that is not an empty dictation.
        for (int i = 0; i < n; i++) {
            if (!pieces[i].unconfirmed) continue;
            pieces[i].ok = false;
            pieces[i].code = EH_DECODE_EMPTY_SPEECH;
            SDL_strlcpy(pieces[i].error, "speech decoded to no text", sizeof pieces[i].error);
        }
        if (!have_error) {
            have_error = true;
            r->code = EH_DECODE_EMPTY_SPEECH;
            SDL_strlcpy(r->error, "speech decoded to no text", sizeof r->error);
        }
        SDL_free(decoded);
        decoded = assemble(pieces, n, owners);
    }
    for (int i = 0; i < n; i++) r->partial |= !pieces[i].ok || (pieces[i].unconfirmed && !owners[i]);
    SDL_free((void *)owners);
    SDL_free((void *)ordered);
    r->text = eh_join_text(o->prefix_text, r->partial && !*decoded ? o->salvage_text : decoded);
    SDL_free(decoded);
    return !(r->partial && !*r->text);
#undef STALE
}
