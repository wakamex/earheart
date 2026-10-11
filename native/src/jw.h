// Building one flat JSON object as text: the messages between the app and its engine workers.
#pragma once
#include <SDL3/SDL.h>

typedef struct { char *s; size_t n, cap; bool any; } eh_jw;

void eh_jw_begin(eh_jw *w);
void eh_jw_str(eh_jw *w, const char *key, const char *value);  // a NULL value is left out
void eh_jw_num(eh_jw *w, const char *key, double value);
void eh_jw_bool(eh_jw *w, const char *key, bool value);
char *eh_jw_end(eh_jw *w);  // the object's text; SDL_free it
