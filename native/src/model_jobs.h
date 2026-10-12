// Model downloads as the windows see them: started and cancelled by a button, running in the
// background, their progress and outcome there to be read on any frame. One download per model.
// Call everything with the loop's lock held. Ported from the model handlers of main/ipc.js.
#pragma once
#include <stdbool.h>
#include "catalog.h"

typedef struct {
    bool active;              // downloading now
    bool done, ok, cancelled; // how the last download ended
    long long received, total;
    char error[200];
} eh_model_job;

// False with a reason when the model is already downloading or being removed.
bool eh_model_job_start(const eh_model *m, char *error, size_t error_size);
void eh_model_job_cancel(const eh_model *m);
eh_model_job eh_model_job_get(const eh_model *m);  // all zero when there was none
void eh_model_job_forget(const eh_model *m);       // drops a finished download's outcome
bool eh_model_jobs_active(void);
// Stops the model's download if one runs, then deletes its files.
bool eh_model_job_remove(const eh_model *m, char *error, size_t error_size);

// The user's own models, from Hugging Face (the custom-model handlers of main/ipc.js). Both wait
// for the network or the disk with the lock let go.
// Looks the address up again, builds the model for the version named (or the recommended one),
// and adds it to the settings. Files downloaded under an earlier definition of the same id are
// deleted. `model_id` gets the new model's id.
bool eh_custom_model_add(eh_model_kind kind, const char *address, const char *variant, char *model_id, size_t id_size, char *error, size_t error_size);
// Deletes its files and takes it out of the settings; a kind that had it chosen goes back to its
// default model.
bool eh_custom_model_remove(const char *id, char *error, size_t error_size);
