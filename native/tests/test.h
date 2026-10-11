#pragma once
#include <stdio.h>

extern int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void test_audio(const char *vectors);  // tests/audio.c
