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
