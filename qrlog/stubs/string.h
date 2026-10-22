#pragma once
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
size_t strlen(const char *s);
char *strchr(const char *s, int c);
