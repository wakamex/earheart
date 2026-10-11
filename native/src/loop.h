// How the app's logic runs: on any number of threads, one at a time. Everything the Electron build
// does on its main thread (the pipeline, the live preview, timers) is done here by tasks that hold
// one lock while they run and let go of it only while they wait: for an engine, a download, a
// sleep. Between two waits a task sees the app's state as nobody else changes it, as JavaScript
// between two awaits does, so the logic ports line for line and its interleavings stay the ones
// the Electron build was tested with. The window's thread takes the same lock to read state for a
// frame and to hand events in.
#pragma once
#include <stdbool.h>

void eh_loop_init(void);
void eh_lock(void);
void eh_unlock(void);
// A wait: the lock is let go for the length of `call`.
#define EH_AWAIT(call) do { eh_unlock(); call; eh_lock(); } while (0)

typedef void eh_task_fn(void *arg);
// Runs fn(arg) as a task: on a thread of its own, holding the lock. Returns at once.
void eh_spawn(eh_task_fn *fn, void *arg);
// Runs fn(arg) as a task after `ms`, once, or every `ms` until cancelled. The number returned
// cancels it; cancelling one that already ran, or 0, does nothing. Call with the lock held.
unsigned eh_after(int ms, eh_task_fn *fn, void *arg);
unsigned eh_every(int ms, eh_task_fn *fn, void *arg);
void eh_cancel(unsigned timer);
// Waits until every task spawned has finished and no one-shot timer is pending; for tests and for
// quitting. Call without the lock.
void eh_loop_drain(void);
