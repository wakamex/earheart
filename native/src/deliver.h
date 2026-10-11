#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "engines.h"
#include "jv.h"
#include "pipeline.h"

// Puts the text where the output settings say: on the clipboard, and in the paste modes into the
// focused app. False with `error` set only when it reached nowhere; a paste that could not be sent
// is a true result with a note, the text being on the clipboard.
bool eh_deliver(const char *text, const eh_jv *output, eh_signal *signal, eh_delivery *result, char *error, size_t error_size);
