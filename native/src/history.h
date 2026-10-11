// The local history of transcriptions: newest first, capped, in a JSON file of this build's own.
// Written as soon as the final text exists, before delivery, so a paste that fails or is cancelled
// still leaves the words here. Called with the loop's lock held.
#pragma once
#include <stdbool.h>
#include "jv.h"

void eh_history_init(const char *path);
// Adds an entry stamped with the time now and writes the file. `limit` is the most entries kept
// (0: 100). A failed write is logged and the entry stays in memory.
void eh_history_add(const char *raw, const char *text, bool cleaned, bool incomplete, int limit);
const eh_jv *eh_history_list(void);  // an array of {raw, text, cleaned, incomplete?, at}
void eh_history_clear(void);
