// Transcript cleanup through llama.cpp's C interface, loaded at run time from the engines folder.
// One model and one context at a time; a new prompt reuses the part of the context it shares with
// the last one, which is what makes priming ahead of a clean pay off.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

#define EH_CLEAN_CONTEXT_MIN 4096
#define EH_CLEAN_CONTEXT_MAX 16384
#define EH_CLEAN_SLACK_TOKENS 64

typedef struct {
    float temperature;  // negative: the default, 0.2
    float top_p;        // negative: the default, 0.95
    int top_k;          // 0 or less: the default, 40
    float min_p;        // 0 or less: off
} eh_sampling;

// The turn the model is given: the rules, the transcript labelled as data, and a cue so that it
// continues with the cleaned text. SDL_free the result. eh_cleanup_turn_prefix is the same without
// the cue, a strict prefix of the turn for any longer transcript.
char *eh_cleanup_turn(const char *system_prompt, const char *transcript);
char *eh_cleanup_turn_prefix(const char *system_prompt, const char *transcript);

// Loads `model_path` with a context of `context_size` tokens (0: the minimum), on a GPU when one
// takes it and on the CPU otherwise or when cpu_only. A model already loaded with that context is kept.
bool eh_cleanup_load(const char *engines_dir, const char *model_path, int context_size, bool cpu_only);
const char *eh_cleanup_device(void);  // what the loaded model runs on, such as "Vulkan0" or "CPU"

// The cleaned transcript, trimmed (SDL_free it), or NULL with eh_cleanup_error() set: nothing
// loaded, a turn that does not fit the context, a reply that ran to the token limit, or `cancel`
// set. `progress` is called with the share generated so far, never more than 0.99.
typedef void eh_cleanup_progress(void *user, float fraction);
char *eh_cleanup_clean(const char *system_prompt, const char *transcript, const eh_sampling *sampling,
                       eh_cleanup_progress *progress, void *user, SDL_AtomicInt *cancel);
// Evaluates the start of a turn into the context without generating, so the clean that follows
// starts at once. `text` is an eh_cleanup_turn_prefix.
bool eh_cleanup_prime(const char *text, SDL_AtomicInt *cancel);

// The tokens of the prompt for `turn` as the loaded model would be given them (SDL_free), for tests.
int *eh_cleanup_prompt_tokens(const char *turn, int *count);
int eh_cleanup_count_tokens(const char *text);  // -1 when nothing is loaded

void eh_cleanup_unload(void);
const char *eh_cleanup_error(void);
