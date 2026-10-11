// The app's parts put together: settings, history, models, engines, pipeline and recorder, and
// what the overlay shows of them. Whoever draws the overlay (the window, or a test) reads eh_view
// with the loop's lock held and is told through the observer when it changed.
#pragma once
#include <stdbool.h>
#include "jv.h"
#include "pipeline.h"

typedef struct {
    bool visible;
    char status[16];   // "", starting, recording, paused, transcribing, cleaning, delivering, done, empty, error
    char notice[96];   // a remark about the microphone while recording
    eh_jv *detail;     // the last status's detail, or NULL
    char phase[16];    // the phase the progress belongs to
    float fraction;
    char *raw, *cleaned;  // the live transcript's two layers
    unsigned sid;
} eh_view;

// Told of everything the pipeline tells the overlay, after eh_view was updated. `kind` is
// "status", "progress", "partial", "overlay", "notify", "history" or "state"; `a` and `b` carry
// what it has to say (see app.c). Called with the lock held. May be NULL.
typedef void eh_observer(const char *kind, const char *a, const char *b, double number, const eh_jv *detail);

// The deliver function stands in for clipboard and paste until they exist, and in tests.
typedef bool eh_deliver_fn(const char *text, const eh_jv *output, eh_signal *signal, eh_delivery *result, char *error, size_t error_size);

// Call once, before anything else, without the lock. `data_dir` holds this build's files.
void eh_app_init(const char *data_dir, eh_observer *observer, eh_deliver_fn *deliver);
const eh_view *eh_app_view(void);
