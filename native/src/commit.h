// Saving what a form holds: the settings window's draft or the wizard's choices. The app's own
// fields and the ones changed elsewhere since the form read them are taken from the live
// settings, the hotkeys are changed first and a refused one keeps its saved value, and the file is
// written. Also the dictionary's merging rules, which the forms and the correction editor share.
// Ported from the commit path of main/ipc.js and from renderer/vocabulary.js and settings-sync.js.
// Call with the loop's lock held.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "hotkeys.h"
#include "gs_json.h"

// The fields other parts of the app change, as a form last saw them saved: the output mode (the
// tray sets it) and the dictionary (the correction editor adds to it).
typedef struct { char output_mode[16]; gs_jval *dictionary; } eh_form_baseline;
void eh_form_baseline_take(eh_form_baseline *b, const gs_jval *settings);

// "paste-copy" also for paste without the clipboard put back, which is what it means.
const char *eh_displayed_output_mode(const gs_jval *settings);

// Takes `next`. False with `error` when the file could not be written, the hotkeys then being as
// they were. `results` says how each hotkey fared; a refused one's saved value is unchanged.
bool eh_commit_settings(gs_jval *next, const eh_form_baseline *baseline, eh_hotkey_results *results, char *error, size_t error_size);

// A list of terms trimmed, without empty ones and repeats. The caller frees every result.
gs_jval *eh_vocabulary_terms(const gs_jval *terms);
// The live list with what the form removed since its baseline taken out and what it added put in.
gs_jval *eh_vocabulary_merge(const gs_jval *baseline, const gs_jval *edited, const gs_jval *live);
// The list with one more spelling, or NULL with the reason it cannot be added.
gs_jval *eh_vocabulary_remember(const gs_jval *terms, const char *spelling, char *error, size_t error_size);
