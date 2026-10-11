#pragma once
#include <stdio.h>

extern int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void test_audio(const char *vectors);    // tests/audio.c
void test_chunked(const char *vectors);  // tests/chunked.c
void test_text(const char *vectors);     // tests/text.c
void test_settings(const char *vectors); // tests/settings.c
