#pragma once
#include <stdio.h>

extern int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void test_audio(const char *vectors);    // tests/audio.c
void test_chunked(const char *vectors);  // tests/chunked.c
void test_text(const char *vectors);     // tests/text.c
void test_settings(const char *vectors); // tests/settings.c
void test_overlay(void);                 // tests/overlay.c
void test_hotkeys(const char *vectors);  // tests/hotkeys.c
void test_vocabulary(const char *vectors);  // tests/commit.c
void test_hf(const char *vectors);          // tests/hf.c
