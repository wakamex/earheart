// The models Earheart can download and run: speech-to-text bundles for sherpa-onnx and GGUF chat
// models for cleanup, each a list of files pinned to an immutable Hugging Face commit with its
// size and SHA-256.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "stt.h"

typedef enum { EH_MODEL_STT, EH_MODEL_CLEANUP } eh_model_kind;
// bytes is 0 and sha256 "" where the source did not say: only the user's own models have such files.
typedef struct { const char *name; long long bytes; const char *sha256, *url; } eh_model_file;
typedef struct {
    const char *id, *label, *note;
    eh_model_kind kind;
    bool is_default;
    const eh_model_file *files;
    int file_count;
    eh_stt_files sherpa;  // speech models
    const char *gguf;     // cleanup models: the file to load
    bool custom;          // added by the user from Hugging Face, and kept in the settings (last: the generated catalogue leaves it out)
} eh_model;

extern const eh_model eh_catalog[];
extern const int eh_catalog_count;

// A model by its id, among the catalogue's and the user's own; id NULL: the kind's default.
const eh_model *eh_catalog_find(eh_model_kind kind, const char *id);
// Every model of a kind, the catalogue's first and then the user's. Returns how many.
int eh_models_list(eh_model_kind kind, const eh_model **out, int max);
// The user's own models, from the settings' customModels. A definition whose id or file names
// could reach outside its folder is left out. A model already known under the same definition
// keeps its eh_model, so pointers to it stay good.
struct eh_jv;
void eh_models_set_custom(const struct eh_jv *list);
// False for a name that is not safe as one part of a path on any system.
bool eh_path_segment(const char *value);
const char *eh_model_kind_dir(eh_model_kind kind);                    // "stt" or "cleanup", the folder under models/

// Where models are looked for: this build's own folder first, where downloads go, then any others,
// such as the Electron build's, whose models are used in place.
void eh_models_set_dirs(const char *const *dirs, int count);
const char *eh_models_own_dir(void);
// The folder of an installed copy of the model: one with a completion marker whose recorded sizes
// match the files on disk and whose fingerprint, when it has one, matches this definition. False
// when no folder holds one.
bool eh_model_installed(const eh_model *m, char *dir, size_t dir_size);
// The identity of a definition's files, as main/engines/model-manager.js computes it: the SHA-256
// of the JSON list of each file's name and checksum, in name order. `hex` gets 65 bytes.
void eh_model_fingerprint(const eh_model *m, char hex[65]);
