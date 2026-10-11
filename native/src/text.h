// Transcript text: the deterministic stumble removal behind the cleanup model, and the helpers the
// live preview compares words with. UTF-8 in and out; characters are classified and re-cased by
// the tables of src/unicode_tables.h, so the results match the JavaScript they are ported from
// (main/util/stumble-strip.js, main/live-preview.js). SDL_free every result.
#pragma once
#include <stdbool.h>

// Removes standalone "um", "uh" and "erm" fillers and repairs the punctuation and spacing their
// removal leaves; then collapses an immediately re-spoken word ("the the file") into one. Both are
// narrow on purpose: never words that can be meaningful, never a doubling that can be deliberate
// ("had had", "two two", "Walla Walla"). The input comes back unchanged when there is nothing to do.
char *eh_strip_fillers(const char *text);
char *eh_collapse_repeats(const char *text);
char *eh_strip_stumbles(const char *text);  // fillers first: removing one can leave a repeat behind

// The part of the in-progress decode `live` that a commit of `committed` did not take over: the
// words past the committed ones when one word sequence is a prefix of the other, compared without
// case or punctuation, and "" on any disagreement.
char *eh_residual_tail(const char *live, const char *committed);

// A word for comparison: lowercase, letters and numbers only.
char *eh_norm_word(const char *from, const char *to);
char *eh_trim(const char *text);  // without the whitespace JavaScript's trim() removes

// One code point from UTF-8 at *s, which is advanced past it. Malformed bytes come back one by one.
unsigned eh_utf8_next(const char **s);
bool eh_is_letter(unsigned cp);
bool eh_is_number(unsigned cp);
bool eh_is_space(unsigned cp);
unsigned eh_to_lower(unsigned cp);
