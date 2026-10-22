#pragma once
#include <stddef.h>
int printf(const char *, ...) __attribute__((format(printf, 1, 2)));
int tsleep(void *chan, int pri, const char *wmesg, int timo);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
size_t strlen(const char *);
