#pragma once
#include <stddef.h>
#include <stdint.h>
#include "engines.h"
#include "jv.h"

// The transcript of a WAV from the service the "stt" settings name, trimmed. NULL with `error` set.
char *eh_remote_transcribe(const uint8_t *wav, size_t len, const eh_jv *stt, eh_signal *signal, char *error, size_t error_size);
// The transcript cleaned by the service the "cleanup" settings name. NULL with `error` set.
char *eh_remote_clean(const char *transcript, const eh_jv *cleanup, eh_signal *signal, char *error, size_t error_size);
// The service's model ids, each once, in order. NULL with `error` set.
eh_jv *eh_remote_models(const eh_jv *cfg, eh_signal *signal, char *error, size_t error_size);
char *eh_strip_thinking(const char *text);  // a reply without its <think> blocks, trimmed
