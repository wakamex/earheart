#include "jw.h"

static void put(eh_jw *w, const char *s, size_t n) {
    if (w->n + n + 1 > w->cap) w->s = SDL_realloc(w->s, w->cap = (w->cap + n) * 2 + 64);
    SDL_memcpy(w->s + w->n, s, n);
    w->n += n;
    w->s[w->n] = 0;
}

static void put_string(eh_jw *w, const char *s) {
    put(w, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        if (*p == '"' || *p == '\\') put(w, esc, (size_t)SDL_snprintf(esc, sizeof esc, "\\%c", *p));
        else if (*p == '\n') put(w, "\\n", 2);
        else if (*p < 0x20) put(w, esc, (size_t)SDL_snprintf(esc, sizeof esc, "\\u%04x", *p));
        else put(w, (const char *)p, 1);
    }
    put(w, "\"", 1);
}

static void key(eh_jw *w, const char *k) {
    put(w, w->any ? "," : "{", 1);
    w->any = true;
    put_string(w, k);
    put(w, ":", 1);
}

void eh_jw_begin(eh_jw *w) { *w = (eh_jw){ 0 }; }

void eh_jw_str(eh_jw *w, const char *k, const char *value) {
    if (!value) return;
    key(w, k);
    put_string(w, value);
}

void eh_jw_num(eh_jw *w, const char *k, double value) {
    char buf[40];
    key(w, k);
    put(w, buf, (size_t)SDL_snprintf(buf, sizeof buf, "%.9g", value));
}

void eh_jw_bool(eh_jw *w, const char *k, bool value) {
    key(w, k);
    put(w, value ? "true" : "false", value ? 4 : 5);
}

char *eh_jw_end(eh_jw *w) {
    put(w, w->any ? "}" : "{}", w->any ? 1 : 2);
    return w->s;
}
