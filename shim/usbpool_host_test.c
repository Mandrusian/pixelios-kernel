#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "usbpool.h"

static struct fdt_memmap mm;

static void setup(void)
{
	memset(&mm, 0, sizeof mm);
	mm.n_reserved = 2;
	mm.reserved[0].base = 0xF8800000ull;	/* "abl" 32 MiB, as on the phone */
	mm.reserved[0].size = 0x2000000ull;
	mm.reserved[1].base = 0xFD3FF000ull;
	mm.reserved[1].size = 0x400000ull;
}

int main(void)
{
	uint64_t pool = 0;
	const char *why = "";
	struct usbpool_avoid av[2];

	/* ok: Gate 0 values, GEVNTADR page + 32 KiB, nothing to avoid */
	setup();
	assert(usbpool_choose(&mm, 0xF8B92000ull, 0x8000, 0, 0, 0, &pool, &why) == 0);
	assert(pool == 0xF8B92000ull);

	/* GEVNTADR not page aligned: pool is its page */
	assert(usbpool_choose(&mm, 0xF8B92200ull, 0x8000, 0, 0, 0, &pool, &why) == 0);
	assert(pool == 0xF8B92000ull);

	/* GEVNTADR zero */
	assert(usbpool_choose(&mm, 0, 0x8000, 0, 0, 0, &pool, &why) != 0);
	assert(strstr(why, "zero"));

	/* not inside any reserved range */
	assert(usbpool_choose(&mm, 0x80000000ull, 0x8000, 0, 0, 0, &pool, &why) != 0);
	assert(strstr(why, "reserved"));

	/* pool straddles the end of the range */
	assert(usbpool_choose(&mm, 0xFA7FC000ull, 0x8000, 0, 0, 0, &pool, &why) != 0);

	/* wrap / zero length */
	assert(usbpool_choose(&mm, 0xFFFFFFFFFFFFF000ull, 0x8000, 0, 0, 0, &pool, &why) != 0);
	assert(usbpool_choose(&mm, 0xF8B92000ull, 0, 0, 0, 0, &pool, &why) != 0);

	/* range whose end wraps is ignored */
	setup();
	mm.reserved[0].base = 0xFFFFFFFFFFF00000ull;
	mm.reserved[0].size = 0x200000ull;
	assert(usbpool_choose(&mm, 0xFFFFFFFFFFF92000ull, 0x8000, 0, 0, 0, &pool, &why) != 0);

	/* truncated reserved table fails closed */
	setup();
	mm.n_reserved_dropped = 1;
	assert(usbpool_choose(&mm, 0xF8B92000ull, 0x8000, 0, 0, 0, &pool, &why) != 0);
	assert(strstr(why, "truncated"));

	/* avoid ranges: image, dtb, initrd, pstore each overlap -> refuse, with their name */
	setup();
	av[0].lo = 0xF8B9A000ull - 1; av[0].hi = 0xF8B9B000ull; av[0].what = "overlaps image";
	av[1].lo = 0; av[1].hi = 0; av[1].what = "absent";
	assert(usbpool_choose(&mm, 0xF8B92000ull, 0x8000, 0, av, 2, &pool, &why) != 0);
	assert(!strcmp(why, "overlaps image"));
	/* touching but not overlapping is fine; absent (hi<=lo) is ignored */
	av[0].lo = 0xF8B9A000ull; av[0].hi = 0xF8B9B000ull;
	assert(usbpool_choose(&mm, 0xF8B92000ull, 0x8000, 0, av, 2, &pool, &why) == 0);
	/* a range wholly inside the pool also refuses */
	av[0].lo = 0xF8B93000ull; av[0].hi = 0xF8B93020ull; av[0].what = "overlaps dtb";
	assert(usbpool_choose(&mm, 0xF8B92000ull, 0x8000, 0, av, 2, &pool, &why) != 0);

	/* required name: the range must be called abl* (review M4) */
	setup();
	strcpy(mm.reserved[0].name, "abl@f8800000");
	strcpy(mm.reserved[1].name, "ramoops@fd3ff000");
	assert(usbpool_choose(&mm, 0xF8B92000ull, 0x8000, "abl", 0, 0, &pool, &why) == 0);
	assert(pool == 0xF8B92000ull);
	assert(usbpool_choose(&mm, 0xFD400000ull, 0x8000, "abl", 0, 0, &pool, &why) != 0);
	assert(strstr(why, "named"));
	strcpy(mm.reserved[0].name, "other");
	assert(usbpool_choose(&mm, 0xF8B92000ull, 0x8000, "abl", 0, 0, &pool, &why) != 0);
	assert(usbpool_choose(&mm, 0xF8B92000ull, 0x8000, 0, 0, 0, &pool, &why) == 0);

	/* hi pool (USB_DMA_HI_BASE): inside a bank, clear of reserved ranges */
	setup();
	mm.n_banks = 1;
	mm.banks[0].base = 0x880000000ull;
	mm.banks[0].size = 0x100000000ull;
	assert(usbpool_choose_hi(&mm, 0x880200000ull, 0x8000, 0, 0, &pool, &why) == 0);
	assert(pool == 0x880200000ull);
	assert(usbpool_choose_hi(&mm, 0x880200100ull, 0x8000, 0, 0, &pool, &why) != 0);	/* unaligned */
	assert(usbpool_choose_hi(&mm, 0xF8B92000ull, 0x8000, 0, 0, &pool, &why) != 0);	/* outside bank */
	assert(strstr(why, "bank"));
	assert(usbpool_choose_hi(&mm, 0x97FFFC000ull, 0x8000, 0, 0, &pool, &why) != 0);	/* straddles bank end */
	mm.reserved[0].base = 0x880204000ull;	/* overlaps pool tail */
	mm.reserved[0].size = 0x1000;
	assert(usbpool_choose_hi(&mm, 0x880200000ull, 0x8000, 0, 0, &pool, &why) != 0);
	assert(strstr(why, "reserved"));
	setup();
	mm.n_banks = 1; mm.banks[0].base = 0x880000000ull; mm.banks[0].size = 0x100000000ull;
	mm.n_reserved_dropped = 1;
	assert(usbpool_choose_hi(&mm, 0x880200000ull, 0x8000, 0, 0, &pool, &why) != 0);
	setup();
	mm.n_banks = 1; mm.banks[0].base = 0x880000000ull; mm.banks[0].size = 0x100000000ull;
	av[0].lo = 0x880208000ull - 1; av[0].hi = 0x880209000ull; av[0].what = "overlaps image";
	assert(usbpool_choose_hi(&mm, 0x880200000ull, 0x8000, av, 1, &pool, &why) != 0);
	av[0].lo = 0x880208000ull;
	assert(usbpool_choose_hi(&mm, 0x880200000ull, 0x8000, av, 1, &pool, &why) == 0);

	printf("PASS usbpool host test: pool choice and fail-closed cases\n");
	return 0;
}
