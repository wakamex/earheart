#include "audio.h"
#include <SDL3/SDL.h>
#include <math.h>

// The arithmetic here follows the JavaScript it is ported from operation for operation, in
// doubles, because the cut points are compared exactly in the tests.

#define WINDOW_SEC 0.05      // pauses are scored with a window this long,
#define HOP_SEC 0.005        // sliding this far, so a pause is found wherever it falls
#define NOISE_FLANK_SEC 0.1  // quiet a short sound needs on each side to count as noise

static double js_round(double x) { return floor(x + 0.5); }
static double sq(float s) { return (double)s * (double)s; }

typedef struct {
    bool quiet;
    int from, to, windows;
    double sec;
    eh_span *parts;
    int part_count;
} run;

typedef struct { run *v; int n, cap; } runs;

static run *push_run(runs *r) {
    if (r->n == r->cap) r->v = SDL_realloc(r->v, sizeof(run) * (size_t)(r->cap = r->cap * 2 + 32));
    r->v[r->n] = (run){ 0 };
    return &r->v[r->n++];
}

static void add_parts(run *to, const eh_span *parts, int count) {
    if (!count) return;
    to->parts = SDL_realloc(to->parts, sizeof(eh_span) * (size_t)(to->part_count + count));
    SDL_memcpy(to->parts + to->part_count, parts, sizeof(eh_span) * (size_t)count);
    to->part_count += count;
}

static void free_runs(runs *r) {
    for (int i = 0; i < r->n; i++) SDL_free(r->v[i].parts);
    SDL_free(r->v);
}

// The buffer as alternating quiet and loud runs of the sliding window. A run spans the windows it
// is made of, and `sec` is its length measured in hops.
static runs scan_runs(const float *s, int count, int rate, double quiet_rms, int *hop_out) {
    int win = SDL_max(1, (int)js_round(WINDOW_SEC * rate)), hop = SDL_max(1, (int)js_round(HOP_SEC * rate));
    double quiet_sum = quiet_rms * quiet_rms * win, sum = 0;
    runs out = { 0 };
    for (int i = 0; i < SDL_min(win, count); i++) sum += sq(s[i]);
    for (int start = 0; start + win <= count; start += hop) {
        if (start > 0) {
            for (int i = start - hop; i < start; i++) sum -= sq(s[i]);
            for (int i = start + win - hop; i < start + win; i++) sum += sq(s[i]);
        }
        bool quiet = sum < quiet_sum;
        run *last = out.n ? &out.v[out.n - 1] : NULL;
        if (last && last->quiet == quiet) {
            last->to = start + win;
            last->windows++;
        } else {
            run *r = push_run(&out);
            r->quiet = quiet;
            r->from = start;
            r->to = start + win;
            r->windows = 1;
        }
    }
    for (int i = 0; i < out.n; i++) out.v[i].sec = (double)(out.v[i].windows * hop) / rate;
    *hop_out = hop;
    return out;
}

// Folds every loud run shorter than min_sound_sec that has quiet on both sides (or the buffer's
// edge) into the quiet around it. Each quiet run of the result lists its truly quiet parts.
static runs absorb_noise(const runs *in, int rate, double min_sound_sec) {
    runs out = { 0 };
    double flank = NOISE_FLANK_SEC * rate;
    for (int i = 0; i < in->n; i++) {
        const run *r = &in->v[i], *prev = i ? &in->v[i - 1] : NULL, *next = i + 1 < in->n ? &in->v[i + 1] : NULL;
        bool noise = min_sound_sec > 0 && !r->quiet && r->sec < min_sound_sec &&
                     (!prev || (prev->quiet && prev->to - prev->from >= flank)) && (!next || (next->quiet && next->to - next->from >= flank));
        run *last = out.n ? &out.v[out.n - 1] : NULL;
        eh_span own = { r->from, r->to };
        if (min_sound_sec > 0 && last && last->quiet && (r->quiet || noise)) {
            last->to = r->to;
            last->windows += r->windows;
            last->sec += r->sec;
            if (r->quiet) add_parts(last, &own, 1);
        } else {
            run *n = push_run(&out);
            *n = *r;
            n->parts = NULL;
            n->part_count = 0;
            n->quiet = r->quiet || noise;
            if (r->quiet) add_parts(n, &own, 1);
        }
    }
    return out;
}

eh_pause *eh_pause_spans(const float *samples, int count, int rate, const eh_split *o, int *pause_count) {
    int hop;
    runs raw = scan_runs(samples, count, rate, o->quiet_rms, &hop);
    runs merged = absorb_noise(&raw, rate, o->min_sound_sec);
    // A quiet run within one hop of min_pause_sec counts, since it sits within a hop of the true
    // silence on each side. One touching the buffer's start or end separates nothing.
    double min_span = o->min_pause_sec * rate - hop;
    eh_pause *out = NULL;
    int n = 0;
    for (int i = 1; i < merged.n - 1; i++) {
        run *r = &merged.v[i];
        if (!r->quiet || !(r->to - r->from >= min_span)) continue;
        out = SDL_realloc(out, sizeof(eh_pause) * (size_t)(n + 1));
        out[n++] = (eh_pause){ r->from, r->to, r->parts, r->part_count };
        r->parts = NULL;
    }
    free_runs(&raw);
    free_runs(&merged);
    *pause_count = n;
    return out;
}

void eh_pauses_free(eh_pause *pauses, int count) {
    for (int i = 0; i < count; i++) SDL_free(pauses[i].parts);
    SDL_free(pauses);
}

// Where a pause is cut: the middle of its longest truly quiet part.
static int pause_cut(const eh_pause *p) {
    eh_span best = p->parts[0];
    for (int i = 1; i < p->part_count; i++)
        if (p->parts[i].to - p->parts[i].from > best.to - best.from) best = p->parts[i];
    return (int)js_round((best.from + best.to) / 2.0);
}

// The frames squeezing drops from a pause to bring it down to `keep`: the middle of every quiet
// part longer than that, leaving half of keep on either side. Returns how many spans it wrote.
static int pause_drops(const eh_pause *p, double keep, eh_span *out) {
    if (!isfinite(keep)) return 0;
    double half = floor(keep / 2);
    int n = 0;
    for (int i = 0; i < p->part_count; i++) {
        int a = p->parts[i].from, b = p->parts[i].to;
        if (b - a > keep) out[n++] = (eh_span){ (int)(a + half), (int)(b - (keep - half)) };
    }
    return n;
}

// How many frames squeezing would take out of a pause; fractional when `keep` is.
static double excess(const eh_pause *p, double keep) {
    if (!isfinite(keep)) return 0;
    double half = floor(keep / 2), total = 0;
    for (int i = 0; i < p->part_count; i++) {
        int a = p->parts[i].from, b = p->parts[i].to;
        if (b - a > keep) total += (b - (keep - half)) - (a + half);
    }
    return total;
}

float *eh_squeeze_pauses(const float *samples, int count, int rate, const eh_split *o, int *out_count) {
    *out_count = count;
    if (!isfinite(o->keep_pause_sec)) return (float *)samples;
    double keep = js_round(o->keep_pause_sec * rate);
    int pause_count, drop_count = 0;
    eh_pause *pauses = eh_pause_spans(samples, count, rate, o, &pause_count);
    int parts = 0;
    for (int i = 0; i < pause_count; i++) parts += pauses[i].part_count;
    eh_span *drops = SDL_malloc(sizeof(eh_span) * (size_t)SDL_max(1, parts));
    for (int i = 0; i < pause_count; i++) drop_count += pause_drops(&pauses[i], keep, drops + drop_count);
    eh_pauses_free(pauses, pause_count);
    if (!drop_count) { SDL_free(drops); return (float *)samples; }
    int dropped = 0;
    for (int i = 0; i < drop_count; i++) dropped += drops[i].to - drops[i].from;
    float *out = SDL_malloc(sizeof(float) * (size_t)SDL_max(1, count - dropped));
    int at = 0, from = 0;
    for (int i = 0; i < drop_count; i++) {
        SDL_memcpy(out + at, samples + from, sizeof(float) * (size_t)(drops[i].from - from));
        at += drops[i].from - from;
        from = drops[i].to;
    }
    SDL_memcpy(out + at, samples + from, sizeof(float) * (size_t)(count - from));
    SDL_free(drops);
    *out_count = count - dropped;
    return out;
}

double eh_sound_seconds(const float *samples, int count, int rate, const eh_split *o) {
    int hop;
    runs raw = scan_runs(samples, count, rate, o->quiet_rms, &hop);
    runs merged = absorb_noise(&raw, rate, o->min_sound_sec);
    double total = 0;
    for (int i = 0; i < merged.n; i++)
        if (!merged.v[i].quiet) total += merged.v[i].sec;
    free_runs(&raw);
    free_runs(&merged);
    return total;
}

int eh_quietest_offset(const float *samples, int count, int window, int hop) {
    if (!(window > 0) || !(hop > 0) || count < window) return count;
    double best = INFINITY;
    int best_end = count;
    for (int start = 0; start + window <= count; start += hop) {
        double sum = 0;
        for (int i = start; i < start + window; i++) sum += sq(samples[i]);
        double rms = sqrt(sum / window);
        // <= so a later window of equal energy wins: a forced cut stays as late as it can.
        if (rms <= best) { best = rms; best_end = start + window; }
    }
    return best_end;
}

typedef struct { int *v; int n, cap; } ints;

static void push_int(ints *a, int x) {
    if (a->n == a->cap) a->v = SDL_realloc(a->v, sizeof(int) * (size_t)(a->cap = a->cap * 2 + 16));
    a->v[a->n++] = x;
}

// Cut points inside one stretch [from, to) so no piece exceeds `max` samples: each at the end of
// the quietest window in the `lookback` before the ceiling.
static void cap_cuts(const float *samples, int from, int to, int max, int lookback, int window, int hop, ints *cuts) {
    while (to - from > max) {
        int ceiling = from + max;
        int search_from = SDL_max(from, ceiling - lookback);  // never back past the piece's own start
        int cut = search_from + eh_quietest_offset(samples + search_from, ceiling - search_from, window, hop);
        if (cut <= from) cut = ceiling;
        push_int(cuts, cut);
        from = cut;
    }
}

static void fit_cap(const float *samples, int rate, int from, int to, const eh_split *o, ints *cuts) {
    int max = (int)floor(o->max_sec * rate);
    if (to - from <= max) return;
    int squeezed;
    float *piece = eh_squeeze_pauses(samples + from, to - from, rate, o, &squeezed);
    if (piece != samples + from) SDL_free(piece);
    if (squeezed <= max) return;
    cap_cuts(samples, from, to, max, (int)floor(o->lookback_sec * rate), SDL_max(1, (int)floor(o->window_sec * rate)),
             SDL_max(1, (int)floor(o->hop_sec * rate)), cuts);
}

int *eh_fit_cap(const float *samples, int rate, int from, int to, const eh_split *o, int *cut_count) {
    ints cuts = { 0 };
    fit_cap(samples, rate, from, to, o, &cuts);
    *cut_count = cuts.n;
    return cuts.v;
}

typedef struct { int first, last, from, to; double speech, squeezed; bool done; } group;

// Groups the stretches between pauses so none holds less than min_utterance samples of speech
// while it can still join a neighbour. The smallest fragment goes first, to the neighbour across
// the shorter pause, and a merge is refused when the decoded piece would exceed max. Returns the
// pauses that stay cuts, in place at the front of `pauses`.
static int keep_group_cuts(eh_pause *pauses, int pause_count, int count, double min_utterance, int max, double keep) {
    int n = pause_count + 1;
    group *g = SDL_malloc(sizeof(group) * (size_t)n);
    for (int i = 0; i < n; i++) {
        int from = i == 0 ? 0 : pause_cut(&pauses[i - 1]), to = i == pause_count ? count : pause_cut(&pauses[i]);
        int speech_from = i == 0 ? 0 : pauses[i - 1].to, speech_to = i == pause_count ? count : pauses[i].from;
        g[i] = (group){ i, i, from, to, SDL_max(0, speech_to - speech_from), 0, false };
    }
    for (;;) {
        int gi = -1;
        for (int i = 0; i < n; i++)
            if (g[i].speech < min_utterance && !g[i].done && (gi < 0 || g[i].speech < g[gi].speech)) gi = i;
        if (gi < 0) break;
        // The neighbour across the shorter pause is tried first; the earlier one on a tie.
        int options[2], option_count = 0;
        if (gi > 0) options[option_count++] = gi - 1;
        if (gi < n - 1) options[option_count++] = gi + 1;
        if (option_count == 2) {
            const eh_pause *before = &pauses[g[gi].first - 1], *after = &pauses[g[gi].last];
            if (after->to - after->from < before->to - before->from) { options[0] = gi + 1; options[1] = gi - 1; }
        }
        int fit = -1;
        for (int k = 0; k < option_count && fit < 0; k++) {
            int j = options[k];
            const eh_pause *pause = j < gi ? &pauses[g[gi].first - 1] : &pauses[g[gi].last];
            double decoded = SDL_max(g[gi].to, g[j].to) - SDL_min(g[gi].from, g[j].from) - g[gi].squeezed - g[j].squeezed - excess(pause, keep);
            if (decoded <= max) fit = j;
        }
        if (fit < 0) { g[gi].done = true; continue; }
        const eh_pause *pause = fit < gi ? &pauses[g[gi].first - 1] : &pauses[g[gi].last];
        int a = SDL_min(gi, fit), b = SDL_max(gi, fit);
        g[a] = (group){ g[a].first, g[b].last, g[a].from, g[b].to, g[a].speech + g[b].speech, g[a].squeezed + g[b].squeezed + excess(pause, keep), false };
        SDL_memmove(&g[b], &g[b + 1], sizeof(group) * (size_t)(n - b - 1));
        n--;
    }
    // The pause after each group but the last stays a cut. Parts of the pauses dropped are freed.
    bool *kept = SDL_calloc((size_t)SDL_max(1, pause_count), sizeof(bool));
    for (int i = 0; i < n - 1; i++) kept[g[i].last] = true;
    int out = 0;
    for (int i = 0; i < pause_count; i++) {
        if (kept[i]) pauses[out++] = pauses[i];
        else SDL_free(pauses[i].parts);
    }
    SDL_free(kept);
    SDL_free(g);
    return out;
}

int *eh_split_points(const float *samples, int count, int rate, const eh_split *o, int *cut_count) {
    int max = (int)floor(o->max_sec * rate), pause_count = 0;
    eh_pause *pauses = isfinite(o->min_pause_sec) ? eh_pause_spans(samples, count, rate, o, &pause_count) : NULL;
    if (o->min_utterance_sec > 0 && pause_count)
        pause_count = keep_group_cuts(pauses, pause_count, count, o->min_utterance_sec * rate, max,
                                      isfinite(o->keep_pause_sec) ? o->keep_pause_sec * rate : INFINITY);
    ints cuts = { 0 };
    int from = 0;
    for (int i = 0; i <= pause_count; i++) {
        int to = i == pause_count ? count : pause_cut(&pauses[i]);
        if (i > 0) push_int(&cuts, from);
        fit_cap(samples, rate, from, to, o, &cuts);
        from = to;
    }
    eh_pauses_free(pauses, pause_count);
    *cut_count = cuts.n;
    return cuts.v;
}

/* ---------------- speech probe ---------------- */

#define SPEECH_WINDOW_SEC 0.3
#define SPEECH_RMS 0.012        // loud enough to be a voice on its own terms
#define SPEECH_OVER_FLOOR 3     // a voice sits about 10 dB over the room it is spoken in
#define SILENCE_RMS 0.0015      // below this everything is dither and dead air
#define SPEECH_MIN_SEC 0.6      // a word, not a transient
#define FLOOR_QUIET_WINDOWS 3   // the floor looks past this many of the quietest windows

static double window_rms(const float *s, int from, int to) {
    double sum = 0;
    for (int i = from; i < to; i++) sum += sq(s[i]);
    return sqrt(sum / (to - from));
}

static int compare_doubles(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

bool eh_contains_speech(const float *samples, int count, int rate) {
    int size = SDL_max(1, (int)floor(SPEECH_WINDOW_SEC * rate)), n = 0;
    double *series = SDL_malloc(sizeof(double) * (size_t)(count / size + 1));
    for (int start = 0; start + size <= count; start += size) series[n++] = window_rms(samples, start, start + size);
    // A buffer too short for one window is measured whole: "no room to look" must not read as silence.
    if (!n && count) series[n++] = window_rms(samples, 0, count);
    if (!n) { SDL_free(series); return false; }
    double *sorted = SDL_malloc(sizeof(double) * (size_t)n);
    SDL_memcpy(sorted, series, sizeof(double) * (size_t)n);
    SDL_qsort(sorted, (size_t)n, sizeof(double), compare_doubles);
    double floor_rms = sorted[SDL_min(FLOOR_QUIET_WINDOWS - 1, n - 1)];
    SDL_free(sorted);
    // Whichever bar is easier to clear.
    double threshold = SDL_min(SPEECH_RMS, SDL_max(SILENCE_RMS, floor_rms * SPEECH_OVER_FLOOR));
    int needed = SDL_max(1, SDL_min(n, (int)js_round(SPEECH_MIN_SEC / SPEECH_WINDOW_SEC))), loud = 0;
    bool speech = false;
    for (int i = 0; i < n && !speech; i++)
        if (series[i] >= threshold && ++loud >= needed) speech = true;
    SDL_free(series);
    return speech;
}
