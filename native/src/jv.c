#include "jv.h"
#include <SDL3/SDL.h>
#include <math.h>
#include "gs_json.h"

static eh_jv *make(eh_jv_type type) {
    eh_jv *v = SDL_calloc(1, sizeof *v);
    v->type = type;
    return v;
}

eh_jv *eh_jv_null(void) { return make(EH_JV_NULL); }
eh_jv *eh_jv_array(void) { return make(EH_JV_ARRAY); }
eh_jv *eh_jv_object(void) { return make(EH_JV_OBJECT); }

eh_jv *eh_jv_bool(bool b) {
    eh_jv *v = make(EH_JV_BOOL);
    v->boolean = b;
    return v;
}

eh_jv *eh_jv_num(double n) {
    eh_jv *v = make(EH_JV_NUMBER);
    v->number = n;
    return v;
}

eh_jv *eh_jv_str(const char *s) {
    if (!s) return eh_jv_null();
    eh_jv *v = make(EH_JV_STRING);
    v->string = SDL_strdup(s);
    return v;
}

void eh_jv_free(eh_jv *v) {
    if (!v) return;
    for (int i = 0; i < v->count; i++) {
        eh_jv_free(v->items[i]);
        if (v->keys) SDL_free(v->keys[i]);
    }
    SDL_free(v->items);
    SDL_free(v->keys);
    SDL_free(v->string);
    SDL_free(v);
}

static void grow(eh_jv *v) {
    if (v->count < v->cap) return;
    v->cap = v->cap * 2 + 4;
    v->items = SDL_realloc(v->items, sizeof(eh_jv *) * (size_t)v->cap);
    if (v->type == EH_JV_OBJECT) v->keys = SDL_realloc(v->keys, sizeof(char *) * (size_t)v->cap);
}

void eh_jv_push(eh_jv *array, eh_jv *value) {
    grow(array);
    array->items[array->count++] = value;
}

static int find(const eh_jv *object, const char *key) {
    if (!object || object->type != EH_JV_OBJECT) return -1;
    for (int i = 0; i < object->count; i++)
        if (!SDL_strcmp(object->keys[i], key)) return i;
    return -1;
}

void eh_jv_set(eh_jv *object, const char *key, eh_jv *value) {
    int i = find(object, key);
    if (i >= 0) {
        eh_jv_free(object->items[i]);
        object->items[i] = value;
        return;
    }
    grow(object);
    object->keys[object->count] = SDL_strdup(key);
    object->items[object->count++] = value;
}

void eh_jv_remove(eh_jv *object, const char *key) {
    int i = find(object, key);
    if (i < 0) return;
    eh_jv_free(object->items[i]);
    SDL_free(object->keys[i]);
    for (int k = i; k < object->count - 1; k++) {
        object->items[k] = object->items[k + 1];
        object->keys[k] = object->keys[k + 1];
    }
    object->count--;
}

eh_jv *eh_jv_get(const eh_jv *object, const char *key) {
    int i = find(object, key);
    return i < 0 ? NULL : object->items[i];
}

eh_jv *eh_jv_at(const eh_jv *array, int index) {
    return array && array->type == EH_JV_ARRAY && index >= 0 && index < array->count ? array->items[index] : NULL;
}

int eh_jv_count(const eh_jv *v) { return v && (v->type == EH_JV_ARRAY || v->type == EH_JV_OBJECT) ? v->count : 0; }
const char *eh_jv_string(const eh_jv *v, const char *fallback) { return v && v->type == EH_JV_STRING ? v->string : fallback; }
double eh_jv_number(const eh_jv *v, double fallback) { return v && v->type == EH_JV_NUMBER ? v->number : fallback; }
bool eh_jv_boolean(const eh_jv *v, bool fallback) { return v && v->type == EH_JV_BOOL ? v->boolean : fallback; }

eh_jv *eh_jv_path(const eh_jv *v, const char *path) {
    char key[128];
    while (v && *path) {
        const char *dot = SDL_strchr(path, '.');
        size_t n = dot ? (size_t)(dot - path) : SDL_strlen(path);
        if (n >= sizeof key) return NULL;
        SDL_memcpy(key, path, n);
        key[n] = 0;
        v = eh_jv_get(v, key);
        path += n + (dot ? 1 : 0);
    }
    return (eh_jv *)v;
}

void eh_jv_set_path(eh_jv *object, const char *path, eh_jv *value) {
    char key[128];
    for (;;) {
        const char *dot = SDL_strchr(path, '.');
        size_t n = dot ? (size_t)(dot - path) : SDL_strlen(path);
        if (n >= sizeof key) { eh_jv_free(value); return; }
        SDL_memcpy(key, path, n);
        key[n] = 0;
        if (!dot) { eh_jv_set(object, key, value); return; }
        eh_jv *next = eh_jv_get(object, key);
        if (!next || next->type != EH_JV_OBJECT) {
            next = eh_jv_object();
            eh_jv_set(object, key, next);
        }
        object = next;
        path = dot + 1;
    }
}

eh_jv *eh_jv_clone(const eh_jv *v) {
    if (!v) return eh_jv_null();
    eh_jv *out = make(v->type);
    out->boolean = v->boolean;
    out->number = v->number;
    if (v->string) out->string = SDL_strdup(v->string);
    for (int i = 0; i < v->count; i++) {
        if (v->type == EH_JV_OBJECT) eh_jv_set(out, v->keys[i], eh_jv_clone(v->items[i]));
        else eh_jv_push(out, eh_jv_clone(v->items[i]));
    }
    return out;
}

bool eh_jv_equal(const eh_jv *a, const eh_jv *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type || a->count != b->count) return false;
    switch (a->type) {
    case EH_JV_BOOL: return a->boolean == b->boolean;
    case EH_JV_NUMBER: return a->number == b->number;
    case EH_JV_STRING: return !SDL_strcmp(a->string, b->string);
    case EH_JV_ARRAY:
        for (int i = 0; i < a->count; i++)
            if (!eh_jv_equal(a->items[i], b->items[i])) return false;
        return true;
    case EH_JV_OBJECT:
        for (int i = 0; i < a->count; i++)
            if (!eh_jv_equal(a->items[i], eh_jv_get(b, a->keys[i]))) return false;
        return true;
    default: return true;
    }
}

eh_jv *eh_jv_merge(const eh_jv *base, const eh_jv *override) {
    if (!override || override->type == EH_JV_NULL) return eh_jv_clone(base);
    if (!base) return eh_jv_clone(override);
    if (base->type == EH_JV_ARRAY) return eh_jv_clone(override->type == EH_JV_ARRAY ? override : base);
    if (base->type != EH_JV_OBJECT) return eh_jv_clone(override->type == base->type ? override : base);
    if (override->type != EH_JV_OBJECT) return eh_jv_clone(base);
    eh_jv *out = eh_jv_clone(base);
    for (int i = 0; i < override->count; i++) {
        const eh_jv *mine = eh_jv_get(base, override->keys[i]);
        eh_jv_set(out, override->keys[i], mine ? eh_jv_merge(mine, override->items[i]) : eh_jv_clone(override->items[i]));
    }
    return out;
}

static eh_jv *from_gs(gs_jv v) {
    switch (gs_json_kind(v)) {
    case GS_JSON_BOOL: return eh_jv_bool(gs_json_bool(v, false));
    case GS_JSON_NUMBER: return eh_jv_num(gs_json_num(v, 0));
    case GS_JSON_STRING: return eh_jv_str(gs_json_str(v, ""));
    case GS_JSON_ARRAY: {
        eh_jv *a = eh_jv_array();
        for (gs_jv c = gs_json_first(v); c; c = gs_json_next(c)) eh_jv_push(a, from_gs(c));
        return a;
    }
    case GS_JSON_OBJECT: {
        eh_jv *o = eh_jv_object();
        for (gs_jv c = gs_json_first(v); c; c = gs_json_next(c)) eh_jv_set(o, gs_json_key(c), from_gs(c));
        return o;
    }
    default: return eh_jv_null();
    }
}

eh_jv *eh_jv_parse(const char *text, size_t len) {
    gs_json *doc = gs_json_parse(text, len);
    if (!doc) return NULL;
    eh_jv *v = from_gs(gs_json_root(doc));
    gs_json_free(doc);
    return v;
}

typedef struct { char *s; size_t n, cap; } buf;

static void put(buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) b->s = SDL_realloc(b->s, b->cap = (b->cap + n) * 2 + 256);
    SDL_memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void put_string(buf *b, const char *s) {
    put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        const char *named = *p == '"' ? "\\\"" : *p == '\\' ? "\\\\" : *p == '\n' ? "\\n" : *p == '\r' ? "\\r" : *p == '\t' ? "\\t" : *p == '\b' ? "\\b" : *p == '\f' ? "\\f" : NULL;
        if (named) put(b, named, 2);
        else if (*p < 0x20) put(b, esc, (size_t)SDL_snprintf(esc, sizeof esc, "\\u%04x", *p));
        else put(b, (const char *)p, 1);
    }
    put(b, "\"", 1);
}

// The shortest decimal that reads back as the same number, as JavaScript prints it.
static void put_number(buf *b, double n) {
    char text[40];
    if (!isfinite(n)) { put(b, "null", 4); return; }
    if (n == floor(n) && fabs(n) < 1e15) { put(b, text, (size_t)SDL_snprintf(text, sizeof text, "%.0f", n)); return; }
    for (int digits = 1; digits <= 17; digits++) {
        SDL_snprintf(text, sizeof text, "%.*g", digits, n);
        if (SDL_strtod(text, NULL) == n) break;
    }
    put(b, text, SDL_strlen(text));
}

static void newline(buf *b, bool pretty, int depth) {
    if (!pretty) return;
    put(b, "\n", 1);
    for (int i = 0; i < depth; i++) put(b, "  ", 2);
}

static void write(buf *b, const eh_jv *v, bool pretty, int depth) {
    switch (v ? v->type : EH_JV_NULL) {
    case EH_JV_NULL: put(b, "null", 4); break;
    case EH_JV_BOOL: put(b, v->boolean ? "true" : "false", v->boolean ? 4 : 5); break;
    case EH_JV_NUMBER: put_number(b, v->number); break;
    case EH_JV_STRING: put_string(b, v->string); break;
    default: {
        bool object = v->type == EH_JV_OBJECT;
        put(b, object ? "{" : "[", 1);
        for (int i = 0; i < v->count; i++) {
            if (i) put(b, ",", 1);
            newline(b, pretty, depth + 1);
            if (object) {
                put_string(b, v->keys[i]);
                put(b, pretty ? ": " : ":", pretty ? 2 : 1);
            }
            write(b, v->items[i], pretty, depth + 1);
        }
        if (v->count) newline(b, pretty, depth);
        put(b, object ? "}" : "]", 1);
    }
    }
}

char *eh_jv_write(const eh_jv *v, bool pretty) {
    buf b = { 0 };
    put(&b, "", 0);
    write(&b, v, pretty, 0);
    return b.s;
}
