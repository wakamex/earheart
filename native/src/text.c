#include "text.h"
#include <SDL3/SDL.h>
#include "unicode_tables.h"

/* ---------------- characters ---------------- */

unsigned eh_utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = p[0];
    int n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
    for (int i = 1; i < n; i++)
        if ((p[i] & 0xc0) != 0x80) n = 1;
    if (n == 2) c = (c & 0x1f) << 6 | (p[1] & 0x3f);
    else if (n == 3) c = (c & 0x0f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f);
    else if (n == 4) c = (c & 0x07) << 18 | (p[1] & 0x3f) << 12 | (p[2] & 0x3f) << 6 | (p[3] & 0x3f);
    *s += n;
    return c;
}

static int utf8_put(char *out, unsigned c) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xc0 | c >> 6); out[1] = (char)(0x80 | (c & 0x3f)); return 2; }
    if (c < 0x10000) { out[0] = (char)(0xe0 | c >> 12); out[1] = (char)(0x80 | (c >> 6 & 0x3f)); out[2] = (char)(0x80 | (c & 0x3f)); return 3; }
    out[0] = (char)(0xf0 | c >> 18); out[1] = (char)(0x80 | (c >> 12 & 0x3f)); out[2] = (char)(0x80 | (c >> 6 & 0x3f)); out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

// The code point that ends just before `at`, and where it starts.
static unsigned utf8_before(const char *start, const char *at, const char **from) {
    const char *p = at;
    do p--; while (p > start && ((unsigned char)*p & 0xc0) == 0x80);
    *from = p;
    const char *q = p;
    return eh_utf8_next(&q);
}

static bool in_ranges(const unsigned (*t)[2], size_t count, unsigned cp) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < t[mid][0]) hi = mid;
        else if (cp > t[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

static unsigned mapped(const unsigned (*t)[2], size_t count, unsigned cp) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < t[mid][0]) hi = mid;
        else if (cp > t[mid][0]) lo = mid + 1;
        else return t[mid][1];
    }
    return cp;
}

bool eh_is_letter(unsigned cp) { return in_ranges(unicode_letter, SDL_arraysize(unicode_letter), cp); }
bool eh_is_number(unsigned cp) { return in_ranges(unicode_number, SDL_arraysize(unicode_number), cp); }
bool eh_is_space(unsigned cp) { return in_ranges(unicode_space, SDL_arraysize(unicode_space), cp); }
unsigned eh_to_lower(unsigned cp) { return mapped(unicode_to_lower, SDL_arraysize(unicode_to_lower), cp); }
static unsigned to_upper(unsigned cp) { return mapped(unicode_to_upper, SDL_arraysize(unicode_to_upper), cp); }
static bool is_lowercase_letter(unsigned cp) { return in_ranges(unicode_lowercase_letter, SDL_arraysize(unicode_lowercase_letter), cp); }
static bool is_hspace(unsigned cp) { return cp != '\n' && eh_is_space(cp); }  // whitespace that stays on its line
// How a case-insensitive match compares two characters: by what they share once case is set aside.
static unsigned fold(unsigned cp) { return eh_to_lower(to_upper(cp)); }

typedef struct { char *s; size_t n, cap; } buf;

static void put(buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) b->s = SDL_realloc(b->s, b->cap = (b->cap + n) * 2 + 64);
    SDL_memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void put_cp(buf *b, unsigned cp) {
    char tmp[4];
    put(b, tmp, (size_t)utf8_put(tmp, cp));
}

char *eh_trim(const char *text) {
    const char *from = text, *p = text, *to = text;
    for (;;) {  // past the leading whitespace
        const char *q = p;
        if (!*p || !eh_is_space(eh_utf8_next(&q))) break;
        p = q;
    }
    from = to = p;
    while (*p) {  // to the end of the last character that is not whitespace
        if (!eh_is_space(eh_utf8_next(&p))) to = p;
    }
    return SDL_strndup(from, (size_t)(to - from));
}

/* ---------------- fillers ---------------- */

#define CUT '\x01'  // stands in for a removed filler
#define CAP '\x02'  // marks a sentence that lost its opening word

static bool wordish(unsigned cp) { return eh_is_letter(cp) || eh_is_number(cp) || cp == '_' || cp == '-'; }

// The length of a filler at p ("um", "umm", "uh", "uhm", "erm", in any case), or 0.
static size_t filler_at(const char *p) {
    size_t n = 0;
    if ((p[0] | 0x20) == 'u') {
        for (n = 1; (p[n] | 0x20) == 'm' || (p[n] | 0x20) == 'h';) n++;
        return n > 1 ? n : 0;
    }
    if ((p[0] | 0x20) == 'e' && (p[1] | 0x20) == 'r' && (p[2] | 0x20) == 'm') {
        for (n = 3; (p[n] | 0x20) == 'm';) n++;
        return n;
    }
    return 0;
}

// Fillers replaced by CUT. In capitals ("UM") it reads as an acronym and stays.
static char *mark_fillers(const char *text, bool *hit) {
    buf out = { 0 };
    put(&out, "", 0);
    for (const char *p = text; *p;) {
        size_t n = filler_at(p);
        const char *before, *after = p + n;
        bool free_left = p == text || !wordish(utf8_before(text, p, &before));
        if (n && free_left && !(*after && wordish(eh_utf8_next(&after)))) {
            bool capitals = true;
            for (size_t i = 0; i < n; i++) capitals = capitals && !(p[i] >= 'a' && p[i] <= 'z');
            if (!capitals) {
                char cut = CUT;
                put(&out, &cut, 1);
                *hit = true;
                p += n;
                continue;
            }
        }
        const char *q = p;
        eh_utf8_next(&q);
        put(&out, p, (size_t)(q - p));
        p = q;
    }
    return out.s;
}

static const char *skip_hspace(const char *p) {
    for (;;) {
        const char *q = p;
        if (!*p || !is_hspace(eh_utf8_next(&q))) return p;
        p = q;
    }
}

static const char *back_hspace(const char *start, const char *p) {
    while (p > start) {
        const char *from;
        if (!is_hspace(utf8_before(start, p, &from))) break;
        p = from;
    }
    return p;
}

// Whether the text up to `end` leaves the next word opening a sentence: it is empty, or ends with
// sentence punctuation or a newline, an optional closing quote or bracket, and whitespace.
static bool opens_sentence(const char *start, const char *end) {
    if (end == start) return true;
    const char *p = end;
    while (p > start) {
        const char *from;
        unsigned cp = utf8_before(start, p, &from);
        if (!eh_is_space(cp)) break;
        if (cp == '\n') return true;
        p = from;
    }
    if (p == start) return false;
    char c = p[-1];
    if (c == '.' || c == '!' || c == '?') return true;
    if ((c == '"' || c == '\'' || c == ')' || c == ']') && p - 1 > start) {
        char d = p[-2];
        return d == '.' || d == '!' || d == '?' || d == '\n';
    }
    return false;
}

char *eh_strip_fillers(const char *text) {
    bool hit = false;
    char *marked = mark_fillers(text, &hit);
    if (!hit) { SDL_free(marked); return SDL_strdup(text); }

    // Each run of fillers is one cut, taken with the comma that only existed to fence it off, and
    // the seam is repaired once. Spacing anywhere else is the user's and is never reflowed.
    buf out = { 0 };
    put(&out, "", 0);
    const char *copied = marked;
    for (const char *cut = SDL_strchr(marked, CUT); cut; cut = SDL_strchr(copied, CUT)) {
        // Back to where the match starts: the spacing before the filler, and a comma before that
        // with its own spacing. Never back into what an earlier cut already replaced.
        const char *start = back_hspace(copied, cut);
        bool before = start > copied && start[-1] == ',';
        if (before) start = back_hspace(copied, start - 1);
        // Forward over the run: fillers, each with its spacing and an optional comma.
        const char *end = cut;
        while (*end == CUT) {
            end = skip_hspace(end + 1);
            if (*end == ',') end = skip_hspace(end + 1);
        }
        // The sentence-ending punctuation the filler sat in front of, with the spacing after it.
        const char *tail = end, *tail_end = end;
        while (*tail_end == '.' || *tail_end == '!' || *tail_end == '?') tail_end++;
        bool has_tail = tail_end > tail;
        if (has_tail) end = tail_end = skip_hspace(tail_end);

        put(&out, copied, (size_t)(start - copied));
        bool opens = opens_sentence(marked, start);
        bool starts_line = start == marked || start[-1] == '\n';
        char next = *end;
        bool ends_line = !next || next == '\n';
        char cap = CAP;
        if (has_tail) {
            if (!opens) {
                // The punctuation ends the running clause and stays, without the fencing comma.
                const char *keep = ends_line ? back_hspace(tail, tail_end) : tail_end;
                put(&out, tail, (size_t)(keep - tail));
            } else if (!ends_line) {
                // A sentence that was nothing but the filler takes its punctuation with it, and
                // the next word takes over the capital it was holding.
                if (!starts_line) put(&out, " ", 1);
                put(&out, &cap, 1);
            }
        } else if (before) {
            // The clause keeps its comma, but not a second one, and no space in front of
            // punctuation that follows. With nothing left to fence off, the comma goes too.
            if (starts_line) {}
            else if (ends_line) put(&out, ",", 1);
            else if (next == ',') {}
            else if (next == '.' || next == ';' || next == ':' || next == '!' || next == '?') put(&out, ",", 1);
            else put(&out, ", ", 2);
        } else if (ends_line) {
        } else if (starts_line) {
            put(&out, &cap, 1);
        } else if (opens) {
            put(&out, " ", 1);
            put(&out, &cap, 1);
        } else if (!(next == ',' || next == '.' || next == ';' || next == ':' || next == '!' || next == '?')) {
            put(&out, " ", 1);
        }
        copied = end;
    }
    put(&out, copied, SDL_strlen(copied));
    SDL_free(marked);

    // The word after each CAP takes the capital, when it starts with a lowercase letter.
    buf done = { 0 };
    put(&done, "", 0);
    for (const char *p = out.s; *p;) {
        if (*p != CAP) {
            const char *q = p;
            eh_utf8_next(&q);
            put(&done, p, (size_t)(q - p));
            p = q;
            continue;
        }
        const char *q = ++p;
        if (!*p || *p == CAP) continue;
        unsigned cp = eh_utf8_next(&q);
        if (is_lowercase_letter(cp)) {
            const char *expanded = NULL;
            for (size_t i = 0; i < SDL_arraysize(unicode_upper_long) && !expanded; i++)
                if (unicode_upper_long[i].cp == cp) expanded = unicode_upper_long[i].upper;
            if (expanded) put(&done, expanded, SDL_strlen(expanded));
            else put_cp(&done, to_upper(cp));
            p = q;
        }
    }
    SDL_free(out.s);
    if (!done.n) { SDL_free(done.s); return SDL_strdup(text); }
    return done.s;
}

/* ---------------- repeats ---------------- */

static bool repeat_char(unsigned cp) { return eh_is_letter(cp) || eh_is_number(cp) || cp == '\'' || cp == 0x2019 || cp == '-'; }

// Doubled words that are grammatical in ordinary speech, and number words, whose repeat is data.
static const char *const KEEP_DOUBLED[] = {
    "had", "that", "very", "really", "so", "no", "yes", "yeah", "ha", "hah", "bye", "night", "sorry", "hey", "well", "much", "many", "long", "far",
    "more", "less", "big", "little", "good", "bad", "again", "nous", "vous", "dat", "die", "der", "das", "den", "dem", "que",
    "zero", "oh", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
    "sixteen", "seventeen", "eighteen", "nineteen", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety", "hundred", "thousand",
    "million", "billion", "double", "triple",
};

static bool collapsible(const char *word, size_t len) {
    buf key = { 0 };
    put(&key, "", 0);
    int units = 0;
    bool number = false;
    for (const char *p = word; p < word + len;) {
        unsigned cp = eh_utf8_next(&p);
        units += cp > 0xffff ? 2 : 1;  // counted as JavaScript counts a string's length
        number = number || eh_is_number(cp);
        put_cp(&key, eh_to_lower(cp));
    }
    bool keep = number;
    for (size_t i = 0; i < SDL_arraysize(KEEP_DOUBLED) && !keep; i++) keep = !SDL_strcmp(key.s, KEEP_DOUBLED[i]);
    // Single letters are usually spelled out ("V V" in a serial number); "I" and "a" stutter.
    if (units == 1 && SDL_strcmp(key.s, "i") && SDL_strcmp(key.s, "a")) keep = true;
    SDL_free(key.s);
    return !keep;
}

// Whether the text at p starts with `word`, compared without case; *end is then where it stops.
static bool echoes(const char *p, const char *word, size_t len, const char **end) {
    const char *w = word;
    while (w < word + len) {
        if (!*p || fold(eh_utf8_next(&p)) != fold(eh_utf8_next(&w))) return false;
    }
    *end = p;
    return true;
}

char *eh_collapse_repeats(const char *text) {
    buf out = { 0 };
    put(&out, "", 0);
    for (const char *p = text; *p;) {
        const char *q = p, *before;
        unsigned cp = eh_utf8_next(&q);
        bool starts_word = (eh_is_letter(cp) || eh_is_number(cp)) && (p == text || !repeat_char(utf8_before(text, p, &before)));
        if (!starts_word) { put(&out, p, (size_t)(q - p)); p = q; continue; }
        // The word, then as many echoes of it as leave no word character right after the last.
        const char *word_end = q;
        for (const char *r = q; *r && repeat_char(eh_utf8_next(&r)); ) word_end = r;
        size_t len = (size_t)(word_end - p);
        const char *match_end = NULL, *at = word_end;
        bool stutter = true, counted = true;
        for (;;) {
            const char *gap = skip_hspace(at), *echo_end;
            if (gap == at || !echoes(gap, p, len, &echo_end)) break;
            // Only a lowercase echo reads as a stutter: one that keeps its own capital is a name
            // ("Walla Walla"). "I I" is the exception.
            bool lower = true;
            for (const char *e = gap; e < echo_end;) {
                unsigned c = eh_utf8_next(&e);
                lower = lower && eh_to_lower(c) == c;
            }
            bool is_i = len == 1 && *p == 'I' && *gap == 'I';
            counted = counted && (lower || is_i);
            at = echo_end;
            const char *after = at;
            if (!*at || !repeat_char(eh_utf8_next(&after))) { match_end = at; stutter = counted; }
        }
        if (!match_end) { put(&out, p, len); p = word_end; continue; }
        if (collapsible(p, len) && stutter) put(&out, p, len);
        else put(&out, p, (size_t)(match_end - p));
        p = match_end;
    }
    if (!out.n) { SDL_free(out.s); return SDL_strdup(text); }
    return out.s;
}

char *eh_strip_stumbles(const char *text) {
    char *without = eh_strip_fillers(text), *out = eh_collapse_repeats(without);
    SDL_free(without);
    return out;
}

/* ---------------- words ---------------- */

// A word for comparison: lowercase, letters and numbers only.
static char *norm_word(const char *from, const char *to) {
    buf b = { 0 };
    put(&b, "", 0);
    for (const char *p = from; p < to;) {
        unsigned cp = eh_utf8_next(&p);
        if (eh_is_letter(cp) || eh_is_number(cp)) put_cp(&b, eh_to_lower(cp));
    }
    return b.s;
}

// The next word of *s as split(/\s+/) would give it: the text up to the next whitespace run. A
// string that starts or ends with whitespace has an empty word there, as in JavaScript.
static bool next_word(const char **s, const char **from, const char **to, bool *more) {
    if (!*more) return false;
    const char *p = *s;
    *from = p;
    for (;;) {
        const char *q = p;
        if (!*p) { *to = p; *more = false; *s = p; return true; }
        if (eh_is_space(eh_utf8_next(&q))) break;
        p = q;
    }
    *to = p;
    for (;;) {
        const char *q = p;
        if (!*p || !eh_is_space(eh_utf8_next(&q))) break;
        p = q;
    }
    *s = p;
    return true;
}

char *eh_residual_tail(const char *live, const char *committed) {
    if (!live || !*live) return SDL_strdup("");
    const char *l = live, *c = committed ? committed : "";
    bool l_more = true, c_more = *c != 0;
    buf out = { 0 };
    put(&out, "", 0);
    bool first = true;
    const char *lf, *lt, *cf, *ct;
    while (next_word(&l, &lf, &lt, &l_more)) {
        if (next_word(&c, &cf, &ct, &c_more)) {
            char *a = norm_word(lf, lt), *b = norm_word(cf, ct);
            bool same = !SDL_strcmp(a, b);
            SDL_free(a);
            SDL_free(b);
            if (!same) { out.n = 0; out.s[0] = 0; return out.s; }
            continue;
        }
        // Past the committed words: this is the tail, joined by single spaces.
        if (!first) put(&out, " ", 1);
        put(&out, lf, (size_t)(lt - lf));
        first = false;
    }
    return out.s;
}
