#include "payload.h"

uintptr_t shim_find_magic(uintptr_t start, uintptr_t end, const char *magic, unsigned len)
{
	if (!len || end < start || end - start < len)
		return 0;
	for (uintptr_t p = start; p <= end - len; p++) {
		unsigned i;
		for (i = 0; i < len; i++)
			if (*(volatile const uint8_t *)(p + i) != (uint8_t)magic[i])
				break;
		if (i == len)
			return p;
	}
	return 0;
}
