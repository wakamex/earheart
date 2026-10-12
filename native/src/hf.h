// Models from Hugging Face: a pasted address read as a repository, the repository's files sorted
// into the versions that can be downloaded, and the model definition made from the one chosen.
// Ported from main/services/hf-models.js. The caller frees every gs_jval returned.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "catalog.h"
#include "gs_json.h"

typedef struct { char owner[128], repo[160], ref[200]; } eh_hf_repo;  // ref: a branch, tag or commit, or ""

// "owner/model", or an address on huggingface.co or hf.co with an optional tree, blob or resolve
// reference. False with the reason for anything else.
bool eh_hf_parse(const char *input, eh_hf_repo *out, char *error, size_t error_size);
// The page of Hugging Face that lists models of the kind the app can run.
const char *eh_hf_search_url(eh_model_kind kind);

// The versions a repository holds, from what Hugging Face answered about it (`info`, with its HTTP
// status) and about its files (`tree`, likewise): {repo, commit, recommended, variants: [{label,
// totalBytes, files: [{name, bytes?, sha256?, url}], sherpa?}]}, best version first. NULL with the
// reason when the repository cannot be used.
gs_jval *eh_hf_listing(eh_model_kind kind, const eh_hf_repo *r, int info_status, const gs_jval *info, int tree_status, const gs_jval *tree, char *error, size_t error_size);
void eh_hf_commit(const eh_hf_repo *r, const gs_jval *info, char *out, size_t size);  // the commit the files are read at
// The model definition for one of those versions, as settings keep it in customModels.
gs_jval *eh_hf_model(eh_model_kind kind, const char *repo_full, const gs_jval *variant);

// Asks Hugging Face about the address and returns the listing. Waits for the network: call it
// without the loop's lock.
gs_jval *eh_hf_discover(eh_model_kind kind, const char *input, char *error, size_t error_size);
