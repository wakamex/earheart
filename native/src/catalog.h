// The models Earheart can download and run: speech-to-text bundles for sherpa-onnx and GGUF chat
// models for cleanup, each a list of files pinned to an immutable Hugging Face commit with its
// size and SHA-256.
#pragma once
#include <stdbool.h>
#include "stt.h"

typedef enum { EH_MODEL_STT, EH_MODEL_CLEANUP } eh_model_kind;
typedef struct { const char *name; long long bytes; const char *sha256, *url; } eh_model_file;
typedef struct {
    const char *id, *label, *note;
    eh_model_kind kind;
    bool is_default;
    const eh_model_file *files;
    int file_count;
    eh_stt_files sherpa;  // speech models
    const char *gguf;     // cleanup models: the file to load
} eh_model;

extern const eh_model eh_catalog[];
extern const int eh_catalog_count;

const eh_model *eh_catalog_find(eh_model_kind kind, const char *id);  // id NULL: the kind's default
const char *eh_model_kind_dir(eh_model_kind kind);                    // "stt" or "cleanup", the folder under models/
