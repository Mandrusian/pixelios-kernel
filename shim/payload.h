#pragma once
#include <stdint.h>

uintptr_t shim_find_magic(uintptr_t start, uintptr_t end, const char *magic, unsigned len);
