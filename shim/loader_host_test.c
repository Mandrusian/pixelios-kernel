#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "payload.h"

int main(void)
{
	uint8_t initrd[9000];
	memset(initrd, 0x5a, sizeof(initrd));
	memcpy(initrd + 4096, "SWEETXNU", 8);
	assert(shim_find_magic((uintptr_t)initrd, (uintptr_t)initrd + sizeof(initrd), "SWEETXNU", 8) ==
	       (uintptr_t)initrd + 4096);
	assert(shim_find_magic((uintptr_t)initrd, (uintptr_t)initrd + 4096, "SWEETXNU", 8) == 0);
	return 0;
}
