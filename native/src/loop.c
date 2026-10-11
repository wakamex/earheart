#include "loop.h"
#include <SDL3/SDL.h>

typedef struct timer {
    unsigned id;
    Uint64 due;
    int every;  // 0: once
    eh_task_fn *fn;
    void *arg;
    struct timer *next;
} timer;

static struct {
    // The app's state is under a lock that is taken in turns: whoever asked first runs first, and
    // a task's turn is taken when it is spawned. So tasks start in the order they were spawned,
    // and work handed over in order reaches an engine in that order, as on a JavaScript event loop.
    SDL_Mutex *turns;
    SDL_Condition *turn_changed;
    unsigned next_turn, serving;
    SDL_Mutex *timers_lock; // the timer list, never held while a task runs
    SDL_Condition *timers_changed;
    timer *timers;
    unsigned next_id;
    unsigned starting[64];  // timers that came due and whose task has not begun; a cancel still stops them
    int starting_count;
    SDL_AtomicInt running;  // tasks spawned and not yet finished
} L;

static unsigned take_turn(void) {
    SDL_LockMutex(L.turns);
    unsigned turn = L.next_turn++;
    SDL_UnlockMutex(L.turns);
    return turn;
}

static void wait_turn(unsigned turn) {
    SDL_LockMutex(L.turns);
    while (L.serving != turn) SDL_WaitCondition(L.turn_changed, L.turns);
    SDL_UnlockMutex(L.turns);
}

void eh_lock(void) { wait_turn(take_turn()); }

void eh_unlock(void) {
    SDL_LockMutex(L.turns);
    L.serving++;
    SDL_BroadcastCondition(L.turn_changed);
    SDL_UnlockMutex(L.turns);
}

typedef struct { eh_task_fn *fn; void *arg; unsigned turn; } task;

static int task_main(void *user) {
    task t = *(task *)user;
    SDL_free(user);
    wait_turn(t.turn);
    t.fn(t.arg);
    eh_unlock();
    SDL_AddAtomicInt(&L.running, -1);
    return 0;
}

void eh_spawn(eh_task_fn *fn, void *arg) {
    task *t = SDL_malloc(sizeof *t);
    *t = (task){ fn, arg, take_turn() };
    SDL_AddAtomicInt(&L.running, 1);
    SDL_DetachThread(SDL_CreateThread(task_main, "task", t));
}

typedef struct { unsigned id; eh_task_fn *fn; void *arg; } due;

// A timer's task, begun: with the app's lock held, it runs unless it was cancelled since it came
// due. So a cancel made under the lock is final, as clearTimeout is.
static void due_task(void *user) {
    due d = *(due *)user;
    SDL_free(user);
    bool live = false;
    SDL_LockMutex(L.timers_lock);
    for (int i = 0; i < L.starting_count && !live; i++) {
        if (L.starting[i] != d.id) continue;
        L.starting[i] = L.starting[--L.starting_count];
        live = true;
    }
    SDL_UnlockMutex(L.timers_lock);
    if (live) d.fn(d.arg);
}

static int timers_main(void *user) {
    SDL_LockMutex(L.timers_lock);
    for (;;) {
        timer *first = NULL;
        for (timer *t = L.timers; t; t = t->next)
            if (!first || t->due < first->due) first = t;
        Uint64 now = SDL_GetTicks();
        if (!first) { SDL_WaitCondition(L.timers_changed, L.timers_lock); continue; }
        if (first->due > now) { SDL_WaitConditionTimeout(L.timers_changed, L.timers_lock, (Sint32)(first->due - now)); continue; }
        due *d = SDL_malloc(sizeof *d);
        *d = (due){ first->id, first->fn, first->arg };
        if (L.starting_count < (int)SDL_arraysize(L.starting)) L.starting[L.starting_count++] = first->id;
        if (first->every) {
            first->due = now + (Uint64)first->every;
        } else {
            for (timer **at = &L.timers; *at; at = &(*at)->next)
                if (*at == first) { *at = first->next; break; }
            SDL_free(first);
        }
        // A task of its own, so a slow one holds up neither the other timers nor a cancel.
        eh_spawn(due_task, d);
    }
}

void eh_loop_init(void) {
    if (L.turns) return;
    L.turns = SDL_CreateMutex();
    L.turn_changed = SDL_CreateCondition();
    L.timers_lock = SDL_CreateMutex();
    L.timers_changed = SDL_CreateCondition();
    SDL_DetachThread(SDL_CreateThread(timers_main, "timers", NULL));
}

static unsigned add_timer(int ms, int every, eh_task_fn *fn, void *arg) {
    timer *t = SDL_calloc(1, sizeof *t);
    SDL_LockMutex(L.timers_lock);
    t->id = ++L.next_id ? L.next_id : ++L.next_id;
    t->due = SDL_GetTicks() + (Uint64)SDL_max(0, ms);
    t->every = every;
    t->fn = fn;
    t->arg = arg;
    t->next = L.timers;
    L.timers = t;
    unsigned id = t->id;
    SDL_SignalCondition(L.timers_changed);
    SDL_UnlockMutex(L.timers_lock);
    return id;
}

unsigned eh_after(int ms, eh_task_fn *fn, void *arg) { return add_timer(ms, 0, fn, arg); }
unsigned eh_every(int ms, eh_task_fn *fn, void *arg) { return add_timer(ms, SDL_max(1, ms), fn, arg); }

void eh_cancel(unsigned id) {
    if (!id) return;
    SDL_LockMutex(L.timers_lock);
    for (timer **at = &L.timers; *at; at = &(*at)->next) {
        if ((*at)->id != id) continue;
        timer *t = *at;
        *at = t->next;
        SDL_free(t);
        break;
    }
    for (int i = 0; i < L.starting_count;) {
        if (L.starting[i] == id) L.starting[i] = L.starting[--L.starting_count];
        else i++;
    }
    SDL_UnlockMutex(L.timers_lock);
}

void eh_loop_drain(void) {
    for (;;) {
        SDL_LockMutex(L.timers_lock);
        bool pending = false;
        for (timer *t = L.timers; t; t = t->next) pending = pending || !t->every;
        SDL_UnlockMutex(L.timers_lock);
        if (!pending && !SDL_GetAtomicInt(&L.running)) return;
        SDL_Delay(5);
    }
}
