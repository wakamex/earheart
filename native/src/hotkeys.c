#include "hotkeys.h"
#include <SDL3/SDL.h>
#include "activate.h"
#include "gs_desk.h"
#include "loop.h"

#define ACC 64
typedef eh_hotkey_result res;

static const char *const NAME[EH_HK_COUNT] = { "record", "pause", "discard", "correction" };

static struct {
    const eh_hotkey_backend *backend;
    char held[EH_HK_COUNT][ACC];  // record, pause and correction: what is registered. Discard: what is configured
    bool armed, live;             // the discard hotkey is grabbed; a dictation is live
    bool suspended;
    bool busy;                    // a change is under way: the next waits its turn
    eh_hotkey_results last;
} H;

typedef struct { bool ok, invalid; char message[160]; } attempt;

static attempt try_add(const char *accelerator, int name) {
    attempt a = { 0 };
    int code = H.backend ? H.backend->add(accelerator, name, a.message, sizeof a.message) : 1;
    a.ok = code == 0;
    a.invalid = code == 2;
    return a;
}

static void drop(const char *accelerator) {
    if (H.backend && accelerator[0]) H.backend->remove(accelerator);
}

static const char *hint(void) {
#ifdef __linux__
    return "already in use, or your desktop blocks global shortcuts — see the Wayland note in Settings";
#else
    return "already in use by another app";
#endif
}

static void failure(char *out, size_t size, const char *accelerator, const attempt *a) {
    char pretty[96];
    gs_accelerator_label(accelerator, pretty, sizeof pretty);
    if (a->invalid) SDL_snprintf(out, size, "Invalid hotkey \"%s\": %s", pretty, a->message);
    else SDL_snprintf(out, size, "Could not register \"%s\" (%s).", pretty, hint());
}

static void used_by(char *out, size_t size, const char *name, const char *accelerator) {
    char pretty[96];
    gs_accelerator_label(accelerator, pretty, sizeof pretty);
    SDL_snprintf(out, size, "\"%s\" is already used by the %s hotkey", pretty, name);
}

static res ok_for(const char *accelerator) { return (res){ .ok = true, .empty = !accelerator[0] }; }

static res fail(const char *fmt, ...) {
    res r = { 0 };
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(r.error, sizeof r.error, fmt, ap);
    va_end(ap);
    return r;
}

static bool ends_sentence(const char *s) {
    size_t n = SDL_strlen(s);
    return n && (s[n - 1] == '.' || s[n - 1] == '!' || s[n - 1] == '?');
}

/* ---------------- record and pause, as a pair ---------------- */

static void apply_pair(const char *record, const char *pause, res out[2]) {
    const char *target[2] = { record ? record : "", pause ? pause : "" };
    char previous[2][ACC];
    bool changed[2];
    for (int n = 0; n < 2; n++) {
        SDL_strlcpy(previous[n], H.held[n], ACC);
        changed[n] = SDL_strcmp(previous[n], target[n]) != 0;
        out[n] = ok_for(target[n]);
    }
    // Both on one key.
    if (target[0][0] && !SDL_strcmp(target[0], target[1])) {
        if (!previous[0][0] && !previous[1][0]) {
            // Nothing held yet: the record hotkey is what the app cannot do without.
            res only[2];
            apply_pair(record, "", only);
            out[0] = only[0];
            if (only[0].ok) {
                out[1] = (res){ 0 };
                used_by(out[1].error, sizeof out[1].error, "record", target[1]);
            } else {
                out[1] = fail("Not changed: the record hotkey could not be registered");
            }
            return;
        }
        for (int n = 0; n < 2; n++) {
            if (!changed[n]) continue;
            out[n] = (res){ 0 };
            used_by(out[n].error, sizeof out[n].error, NAME[!n], target[n]);
        }
        return;
    }
    // A new key that neither hotkey holds now can be grabbed before anything is let go. One that
    // the other hotkey holds ("crossed") has to wait for that one's release.
    bool is_free[2] = { false, false }, crossed[2] = { false, false }, added[2] = { false, false }, released[2] = { false, false };
    int added_count = 0, released_count = 0, added_first = -1;
    for (int n = 0; n < 2; n++) {
        if (!changed[n] || !target[n][0]) continue;
        bool current = (previous[0][0] && !SDL_strcmp(previous[0], target[n])) || (previous[1][0] && !SDL_strcmp(previous[1], target[n]));
        if (current) crossed[n] = true;
        else is_free[n] = true;
    }
    int failed = -1;
    char why[400] = "";
    for (int pass = 0; pass < 2 && failed < 0; pass++) {
        if (pass == 1) {
            for (int n = 0; n < 2; n++) {
                if (!previous[n][0]) continue;
                bool wanted = (crossed[0] && !SDL_strcmp(target[0], previous[n])) || (crossed[1] && !SDL_strcmp(target[1], previous[n]));
                if (!wanted) continue;
                drop(previous[n]);
                released[n] = true;
                released_count++;
            }
        }
        for (int n = 0; n < 2 && failed < 0; n++) {
            if (!(pass == 0 ? is_free[n] : crossed[n])) continue;
            attempt a = try_add(target[n], n);
            if (!a.ok) {
                failure(why, sizeof why, target[n], &a);
                failed = n;
            } else {
                added[n] = true;
                if (!added_count++) added_first = n;
            }
        }
    }
    if (failed >= 0) {
        // Starting cold, a record hotkey that registered is kept though the pause one did not.
        if (!previous[0][0] && !previous[1][0] && failed == EH_HK_PAUSE && added_count == 1 && added_first == EH_HK_RECORD && !released_count) {
            SDL_strlcpy(H.held[EH_HK_RECORD], target[EH_HK_RECORD], ACC);
            out[EH_HK_PAUSE] = fail("%s", why);
            return;
        }
        for (int n = 0; n < 2; n++)
            if (added[n]) drop(target[n]);
        for (int n = 0; n < 2; n++) {
            if (!changed[n]) continue;
            out[n] = n == failed ? fail("%s", why) : fail("Not changed: the %s hotkey could not be registered", NAME[failed]);
        }
        for (int n = 0; n < 2; n++) {
            if (!released[n]) continue;
            attempt a = try_add(previous[n], n);
            if (a.ok) {
                SDL_strlcpy(H.held[n], previous[n], ACC);
                continue;
            }
            char pretty[96], restore[300], unbound[400], prior[400];
            gs_accelerator_label(previous[n], pretty, sizeof pretty);
            SDL_snprintf(restore, sizeof restore, "Could not restore \"%s\" after rollback%s%s", pretty, a.invalid ? ": " : "", a.invalid ? a.message : "");
            H.held[n][0] = 0;
            SDL_snprintf(unbound, sizeof unbound, "%s. The %s hotkey is now unbound until you save again or restart.", restore, NAME[n]);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", unbound);
            const char *before = out[n].error, *lead = "Not changed: the ";
            if (!SDL_strncmp(before, lead, SDL_strlen(lead))) SDL_snprintf(prior, sizeof prior, "The %s", before + SDL_strlen(lead));
            else SDL_strlcpy(prior, before, sizeof prior);
            res r = { .unbound = true };
            SDL_snprintf(r.error, sizeof r.error, "%s%s%s", prior, ends_sentence(prior) ? " " : ". ", unbound);
            out[n] = r;
        }
        return;
    }
    for (int n = 0; n < 2; n++) {
        if (!changed[n]) continue;
        if (previous[n][0] && !released[n]) drop(previous[n]);
        SDL_strlcpy(H.held[n], target[n], ACC);
    }
}

/* ---------------- discard: configured always, grabbed while a dictation is live ---------------- */

static res set_discard(const char *accelerator) {
    char *held = H.held[EH_HK_DISCARD];
    if (!SDL_strcmp(accelerator, held)) return ok_for(accelerator);
    if (!accelerator[0]) {
        if (H.armed) drop(held);
        H.armed = false;
        held[0] = 0;
        return ok_for("");
    }
    // Grabbed once to learn whether it can be, then let go again unless a dictation is live.
    attempt a = try_add(accelerator, EH_HK_DISCARD);
    if (!a.ok) {
        res r = { 0 };
        failure(r.error, sizeof r.error, accelerator, &a);
        return r;
    }
    if (H.armed) drop(held);
    SDL_strlcpy(held, accelerator, ACC);
    H.armed = H.live && !H.suspended;
    if (!H.armed) drop(accelerator);
    return ok_for(accelerator);
}

static void apply_live(const char *record, const char *pause, const char *discard, res out[EH_HK_COUNT]) {
    const char *target[3] = { record, pause, discard };
    char previous[3][ACC];
    for (int n = 0; n < 3; n++) SDL_strlcpy(previous[n], H.held[n], ACC);
    char charged[3][400] = { "", "", "" };
    if (discard[0]) {
        for (int n = 0; n < 2; n++) {
            if (SDL_strcmp(target[n], discard)) continue;
            // Whichever of the two was changed onto the other's key is the one refused.
            bool discard_changed = SDL_strcmp(discard, previous[EH_HK_DISCARD]) != 0, other_changed = SDL_strcmp(target[n], previous[n]) != 0;
            if (other_changed && !discard_changed) used_by(charged[n], sizeof charged[n], "discard", target[n]);
            else used_by(charged[EH_HK_DISCARD], sizeof charged[EH_HK_DISCARD], NAME[n], discard);
        }
    }
    apply_pair(charged[0][0] ? previous[0] : record, charged[1][0] ? previous[1] : pause, out);
    for (int n = 0; n < 2; n++)
        if (charged[n][0]) out[n] = fail("%s", charged[n]);
    int holder = -1;
    for (int n = 0; n < 2 && holder < 0; n++)
        if (discard[0] && H.held[n][0] && !SDL_strcmp(H.held[n], discard)) holder = n;
    char error[400] = "";
    if (charged[EH_HK_DISCARD][0]) SDL_strlcpy(error, charged[EH_HK_DISCARD], sizeof error);
    else if (holder >= 0) used_by(error, sizeof error, NAME[holder], discard);
    if (!error[0]) {
        out[EH_HK_DISCARD] = set_discard(discard);
        return;
    }
    // The discard key in force may now belong to record or pause: it is given up.
    bool pair_holds = false;
    for (int n = 0; n < 2; n++) pair_holds = pair_holds || (H.held[n][0] && !SDL_strcmp(H.held[n], H.held[EH_HK_DISCARD]));
    if (H.held[EH_HK_DISCARD][0] && pair_holds) set_discard("");
    out[EH_HK_DISCARD] = fail("%s", error);
}

/* ---------------- with the correction hotkey ---------------- */

// Which of record, pause and discard has the accelerator now.
static int holder_of(const char *accelerator) {
    for (int n = 0; n < 3; n++)
        if (accelerator[0] && !SDL_strcmp(H.held[n], accelerator)) return n;
    return -1;
}

static void apply_all(const char *record, const char *pause, const char *discard, const char *correct, res out[EH_HK_COUNT]) {
    const char *target[3] = { record, pause, discard }, *desired = correct;
    char held[ACC], before[3][ACC], dictation[3][ACC], charged[3][400] = { "", "", "" };
    SDL_strlcpy(held, H.held[EH_HK_CORRECT], ACC);
    for (int n = 0; n < 3; n++) {
        SDL_strlcpy(before[n], H.held[n], ACC);
        SDL_strlcpy(dictation[n], target[n], ACC);
    }
#define KEEP(n, ...) (SDL_strlcpy(dictation[n], before[n], ACC), SDL_snprintf(charged[n], sizeof charged[n], __VA_ARGS__))
#define FINISH(correct_result) do { \
        out[EH_HK_CORRECT] = (correct_result); \
        for (int n_ = 0; n_ < 3; n_++) if (charged[n_][0]) out[n_] = fail("%s", charged[n_]); \
        return; \
    } while (0)
    char correct_error[400] = "", text[400];
    for (int n = 0; n < 3; n++) {
        if (!desired[0] || SDL_strcmp(target[n], desired)) continue;
        // The correction hotkey stays where it is, and a dictation hotkey moved onto it is refused;
        // moved onto a dictation hotkey, the correction hotkey is.
        if (!SDL_strcmp(desired, held) && SDL_strcmp(target[n], before[n])) {
            used_by(text, sizeof text, "correction", desired);
            KEEP(n, "%s", text);
        } else if (!correct_error[0]) {
            used_by(correct_error, sizeof correct_error, NAME[n], desired);
        }
    }
    const char *goal = correct_error[0] ? held : desired;
    static const char not_changed[] = "Not changed: the correction hotkey could not be registered";
    if (!SDL_strcmp(goal, held)) {
        for (int n = 0; n < 3; n++) {
            if (held[0] && !SDL_strcmp(target[n], held) && !charged[n][0]) {
                used_by(text, sizeof text, "correction", held);
                KEEP(n, "%s", text);
            }
        }
        apply_live(dictation[0], dictation[1], dictation[2], out);
        FINISH(correct_error[0] ? fail("%s", correct_error) : ok_for(held));
    }
    // The dictation hotkeys that want the key the correction hotkey is leaving.
    bool takers[3], any_takers = false;
    for (int n = 0; n < 3; n++) {
        takers[n] = held[0] && !SDL_strcmp(target[n], held) && !charged[n][0];
        any_takers = any_takers || takers[n];
    }
    if (holder_of(goal) < 0) {
        attempt a = goal[0] ? try_add(goal, EH_HK_CORRECT) : (attempt){ .ok = true };
        if (!a.ok) {
            for (int n = 0; n < 3; n++)
                if (takers[n]) KEEP(n, "%s", not_changed);
            apply_live(dictation[0], dictation[1], dictation[2], out);
            res r = { 0 };
            failure(r.error, sizeof r.error, goal, &a);
            FINISH(r);
        }
        if (held[0]) drop(held);
        SDL_strlcpy(H.held[EH_HK_CORRECT], goal, ACC);
        apply_live(dictation[0], dictation[1], dictation[2], out);
        FINISH(ok_for(goal));
    }
    // The key wanted is a dictation hotkey's now: that one has to move off it first.
    bool released_early = any_takers;
    if (released_early) drop(held);
    apply_live(dictation[0], dictation[1], dictation[2], out);
    int blocker = holder_of(goal);
    attempt a = { 0 };
    if (blocker < 0) a = try_add(goal, EH_HK_CORRECT);
    if (blocker < 0 && a.ok) {
        if (!released_early) drop(held);
        SDL_strlcpy(H.held[EH_HK_CORRECT], goal, ACC);
        FINISH(ok_for(goal));
    }
    char error[400];
    if (blocker >= 0) {
        if (!out[blocker].ok) SDL_snprintf(error, sizeof error, "Not changed: the %s hotkey could not be registered", NAME[blocker]);
        else used_by(error, sizeof error, NAME[blocker], goal);
    } else {
        failure(error, sizeof error, goal, &a);
    }
    if (!released_early) FINISH(fail("%s", error));
    if (holder_of(held) >= 0) {
        // A dictation hotkey took the key the correction hotkey had: everything goes back.
        if (H.armed && (!SDL_strcmp(H.held[EH_HK_DISCARD], before[0]) || !SDL_strcmp(H.held[EH_HK_DISCARD], before[1]))) {
            drop(H.held[EH_HK_DISCARD]);
            H.armed = false;
        }
        res reverted[EH_HK_COUNT];
        apply_live(before[0], before[1], before[2], reverted);
        if (!reverted[EH_HK_DISCARD].ok && SDL_strcmp(H.held[EH_HK_DISCARD], before[EH_HK_DISCARD])) set_discard("");
        for (int n = 0; n < 3; n++) out[n] = reverted[n];
        for (int n = 0; n < 3; n++) {
            if (SDL_strcmp(H.held[n], before[n])) {
                if (n == EH_HK_DISCARD) set_discard("");
                else {
                    if (H.held[n][0]) drop(H.held[n]);
                    H.held[n][0] = 0;
                }
                if (before[n][0]) {
                    char pretty[96];
                    gs_accelerator_label(before[n], pretty, sizeof pretty);
                    res r = { .unbound = true };
                    SDL_snprintf(r.error, sizeof r.error, "Could not restore \"%s\" after rollback. The saved %s hotkey is now unbound until you save again or restart.", pretty, n == EH_HK_DISCARD ? "discard" : NAME[n]);
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", r.error);
                    out[n] = r;
                } else {
                    out[n] = fail("%s", not_changed);
                }
                continue;
            }
            if (SDL_strcmp(target[n], before[n]) && !charged[n][0]) {
                if (reverted[n].ok) out[n] = fail("%s", not_changed);
                else out[n] = reverted[n];
            }
        }
    } else {
        for (int n = 0; n < 3; n++)
            if (takers[n] && out[n].ok) out[n] = fail("%s", not_changed);
    }
    attempt back = try_add(held, EH_HK_CORRECT);
    if (!back.ok) {
        char pretty[96], unbound[300];
        gs_accelerator_label(held, pretty, sizeof pretty);
        H.held[EH_HK_CORRECT][0] = 0;
        SDL_snprintf(unbound, sizeof unbound, "Could not restore \"%s\" after rollback. The correction hotkey is now unbound until you save again or restart.", pretty);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", unbound);
        res r = { .unbound = true };
        SDL_snprintf(r.error, sizeof r.error, "%s%s%s", error, ends_sentence(error) ? " " : ". ", unbound);
        FINISH(r);
    }
    FINISH(fail("%s", error));
#undef KEEP
#undef FINISH
}

/* ---------------- arming, suspending ---------------- */

static void arm_discard(bool live) {
    H.live = live;
    if (H.suspended) return;
    char *held = H.held[EH_HK_DISCARD];
    if (live && !H.armed && held[0]) {
        attempt a = try_add(held, EH_HK_DISCARD);
        if (a.ok) H.armed = true;
        else {
            char why[400];
            failure(why, sizeof why, held, &a);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "discard hotkey: %s", why);
        }
    } else if (!live && H.armed) {
        drop(held);
        H.armed = false;
    }
}

static int suspend(bool on) {
    if (on == H.suspended) return 0;
    H.suspended = on;
    static const int always[3] = { EH_HK_RECORD, EH_HK_PAUSE, EH_HK_CORRECT };
    if (on) {
        for (int i = 0; i < 3; i++) drop(H.held[always[i]]);
        if (H.armed) drop(H.held[EH_HK_DISCARD]);
        H.armed = false;
        return 0;
    }
    int lost = 0;
    for (int i = 0; i < 3; i++) {
        int n = always[i];
        if (!H.held[n][0]) continue;
        attempt a = try_add(H.held[n], n);
        if (a.ok) continue;
        char why[400];
        failure(why, sizeof why, H.held[n], &a);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s hotkey: %s after capture", n == EH_HK_CORRECT ? "correct" : NAME[n], why);
        H.held[n][0] = 0;
        lost++;
    }
    arm_discard(H.live);
    return lost;
}

/* ---------------- the interface ---------------- */

// One change at a time: grabbing a key can wait for another thread, which lets other tasks run.
static void enter(void) {
    while (H.busy) EH_AWAIT(SDL_Delay(2));
    H.busy = true;
}

void eh_hotkeys_backend(const eh_hotkey_backend *backend) { H.backend = backend; }

void eh_hotkeys_apply_raw(const char *record, const char *pause, const char *discard, const char *correct, res out[EH_HK_COUNT]) {
    bool was = H.suspended;
    suspend(false);
    apply_all(record ? record : "", pause ? pause : "", discard ? discard : "", correct ? correct : "", out);
    if (was) suspend(true);
}

void eh_hotkeys_apply(const gs_jval *settings, eh_hotkey_results *out) {
    static const char *const keys[EH_HK_COUNT] = { "hotkey", "pauseHotkey", "discardHotkey", "correctHotkey" };
    char want[EH_HK_COUNT][ACC];
    for (int n = 0; n < EH_HK_COUNT; n++) SDL_strlcpy(want[n], gs_jval_string(gs_jval_get(settings, keys[n]), ""), ACC);
    enter();
    eh_hotkey_results r;
    eh_hotkeys_apply_raw(want[0], want[1], want[2], want[3], r.r);
    // A record hotkey that is not set is a misconfiguration, where the others are simply unbound.
    if (r.r[EH_HK_RECORD].empty) {
        r.r[EH_HK_RECORD] = fail("No hotkey configured");
        r.r[EH_HK_RECORD].empty = true;
    }
    for (int n = 0; n < EH_HK_COUNT; n++) SDL_strlcpy(r.r[n].accelerator, want[n], ACC);
    H.last = r;
    H.busy = false;
    if (out) *out = r;
}

eh_hotkey_result eh_hotkeys_status(int name) { return H.last.r[name]; }

void eh_hotkeys_arm_discard(bool live) {
    enter();
    arm_discard(live);
    H.busy = false;
}

int eh_hotkeys_suspend(bool on) {
    enter();
    int lost = suspend(on);
    H.busy = false;
    return lost;
}

void eh_hotkeys_release_all(void) {
    enter();
    for (int n = 0; n < EH_HK_COUNT; n++) {
        if (n != EH_HK_DISCARD || H.armed) drop(H.held[n]);
        H.held[n][0] = 0;
    }
    H.armed = H.suspended = false;
    H.busy = false;
}

const char *eh_hotkeys_held(int name) { return H.held[name]; }
bool eh_hotkeys_discard_armed(void) { return H.armed; }

void eh_hotkeys_reset(void) {
    const eh_hotkey_backend *b = H.backend;
    SDL_memset(&H, 0, sizeof H);
    H.backend = b;
}
