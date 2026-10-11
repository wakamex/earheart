// The user's settings: a JSON file laid over the built-in defaults, as in main/settings.js. There
// is one way in which they change: eh_settings_save writes the file whole, and only when that
// worked does the copy in memory change and do the listeners hear of it.
#pragma once
#include <stdbool.h>
#include "cleanup.h"
#include "jv.h"

// Reads the file at `path` (it need not exist). When it does not, and `import_from` names a
// settings file of the Electron build, those settings are taken over and saved here.
void eh_settings_init(const char *path, const char *import_from);
bool eh_settings_first_run(void);  // there was no settings file, and none to import

const eh_jv *eh_settings(void);                  // the current settings; not to be changed
const char *eh_setting_str(const char *path);    // "" when it is not a string
double eh_setting_num(const char *path);         // 0 when it is not a number
bool eh_setting_bool(const char *path);

// Saves `next` (owned by the call), laid over the defaults and with its limits enforced. False
// with `error` set when the file could not be written; the settings in memory are then unchanged.
bool eh_settings_save(eh_jv *next, char *error, size_t error_size);
// One value changed and the whole saved: the usual case.
bool eh_setting_set(const char *path, eh_jv *value, char *error, size_t error_size);

typedef void eh_settings_listener(void *user, const eh_jv *previous, const eh_jv *current);
void eh_settings_on_changed(eh_settings_listener *fn, void *user);

// Settings as stored on disk, brought up to date and laid over the defaults: what load does to a
// file's content. Returns a new value. For tests.
eh_jv *eh_settings_resolve(const eh_jv *stored);

// The prompt and sampling a cleanup section asks for: the base prompt, the preferred terms, and
// either the chosen style's directive and numbers or the custom numbers. SDL_free the prompt.
typedef struct {
    char *system_prompt;
    eh_sampling sampling;
    bool has_temperature, has_top_p;  // for the remote service, which is sent only what was set
} eh_cleanup_plan;
eh_cleanup_plan eh_resolve_cleanup(const eh_jv *cleanup);
