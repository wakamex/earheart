// JSON values that can be built, changed, merged and written: settings, history and the other
// files the app keeps. Objects keep their keys in order. Reading goes through gesso's gs_json;
// this is the writing half, here until gesso has one.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { EH_JV_NULL, EH_JV_BOOL, EH_JV_NUMBER, EH_JV_STRING, EH_JV_ARRAY, EH_JV_OBJECT } eh_jv_type;

typedef struct eh_jv eh_jv;
struct eh_jv {
    eh_jv_type type;
    bool boolean;
    double number;
    char *string;
    char **keys;    // objects: one per item
    eh_jv **items;  // arrays and objects
    int count, cap;
};

eh_jv *eh_jv_null(void);
eh_jv *eh_jv_bool(bool b);
eh_jv *eh_jv_num(double n);
eh_jv *eh_jv_str(const char *s);  // NULL gives null
eh_jv *eh_jv_array(void);
eh_jv *eh_jv_object(void);
void eh_jv_free(eh_jv *v);
eh_jv *eh_jv_clone(const eh_jv *v);

eh_jv *eh_jv_parse(const char *text, size_t len);  // NULL when it is not valid JSON
// As JSON.stringify(value, null, 2) writes it when `pretty`, compact otherwise. SDL_free the result.
char *eh_jv_write(const eh_jv *v, bool pretty);

// Reading. Every accessor takes NULL and the wrong type, and answers with the fallback.
eh_jv *eh_jv_get(const eh_jv *object, const char *key);
eh_jv *eh_jv_path(const eh_jv *v, const char *path);  // keys joined by dots
eh_jv *eh_jv_at(const eh_jv *array, int index);
int eh_jv_count(const eh_jv *v);
const char *eh_jv_string(const eh_jv *v, const char *fallback);
double eh_jv_number(const eh_jv *v, double fallback);
bool eh_jv_boolean(const eh_jv *v, bool fallback);

// Changing. Each takes ownership of `value`.
void eh_jv_set(eh_jv *object, const char *key, eh_jv *value);  // replaces the key's value, or adds the key
void eh_jv_remove(eh_jv *object, const char *key);
void eh_jv_push(eh_jv *array, eh_jv *value);
// Sets a value at a dotted path, making the objects on the way.
void eh_jv_set_path(eh_jv *object, const char *path, eh_jv *value);

// `override` laid over `base`, as the settings are merged over their defaults: a value of the
// wrong type for its place falls back to the default, an array replaces an array whole, and keys
// the defaults do not know are kept. Returns a new value.
eh_jv *eh_jv_merge(const eh_jv *base, const eh_jv *override);
bool eh_jv_equal(const eh_jv *a, const eh_jv *b);
