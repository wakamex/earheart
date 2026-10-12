// Which global hotkeys are held, and changing them as one step: the record and pause hotkeys are
// held all the time, the discard hotkey only while a dictation is live, the correction hotkey all
// the time. A change that cannot be made whole leaves the hotkeys as they were, and says for each
// one why. Ported from main/hotkeys.js, whose rules and messages these are. The keys themselves
// are grabbed by a backend, so the rules are tested without a desktop. Call everything with the
// loop's lock held.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "gs_json.h"

enum { EH_HK_RECORD, EH_HK_PAUSE, EH_HK_DISCARD, EH_HK_CORRECT, EH_HK_COUNT };

typedef struct {
    // Grabs the accelerator for the hotkey named. 0: held. 1: refused (taken by another app, or the
    // desktop does not allow it). 2: not a key combination, with `error` saying why.
    int (*add)(const char *accelerator, int name, char *error, size_t error_size);
    void (*remove)(const char *accelerator);
} eh_hotkey_backend;

typedef struct {
    bool ok;
    bool empty;     // no key is set for it
    bool unbound;   // a failed change could not be undone: the hotkey is held by nothing now
    char error[400];
    char accelerator[64];  // what was asked for
} eh_hotkey_result;
typedef struct { eh_hotkey_result r[EH_HK_COUNT]; } eh_hotkey_results;  // in the order of the enum

void eh_hotkeys_backend(const eh_hotkey_backend *backend);
// Makes the held hotkeys those of the settings given ("hotkey", "pauseHotkey", "discardHotkey",
// "correctHotkey"), and remembers the outcome for eh_hotkeys_status.
void eh_hotkeys_apply(const gs_jval *settings, eh_hotkey_results *out);
eh_hotkey_result eh_hotkeys_status(int name);
void eh_hotkeys_arm_discard(bool live);  // a dictation began or ended
// Lets every hotkey go, or takes them back. Returns how many could not be taken back.
int eh_hotkeys_suspend(bool on);
void eh_hotkeys_release_all(void);

// The hotkeys in the terms of main/hotkeys.js, for the tests that replay its behaviour.
void eh_hotkeys_apply_raw(const char *record, const char *pause, const char *discard, const char *correct, eh_hotkey_result out[EH_HK_COUNT]);
const char *eh_hotkeys_held(int name);  // the accelerator remembered for the name, or ""
bool eh_hotkeys_discard_armed(void);
void eh_hotkeys_reset(void);            // forgets everything, without telling the backend
