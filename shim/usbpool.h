#pragma once
#include <stddef.h>
#include <stdint.h>

#include "fdt.h"

/*
 * usbpool.h - DMA pool choice for the stage-2 USB gadget. Pure
 * function (no MMIO, no console): host-tested, and audited like
 * usbproto.o (tools/test-bluejay-boot.py).
 *
 * The shim's own .bss is NOT usable for device DMA (UFS DMA from it
 * timed out, probably S2MPU), so the gadget borrows the pool ABL left
 * the DWC3 event buffer in (Gate 0: GEVNTADR 0xF8B92000, inside the
 * reserved-memory node "abl"). Fail closed: any doubt -> no pool.
 */

struct usbpool_avoid {
	uint64_t lo, hi;	/* [lo, hi); hi <= lo means "not present" */
	const char *what;	/* printed when the pool would overlap it */
};

/* Choose the pool: the 4 KiB page of `gevntadr` and pool_len bytes
 * from it. Returns 0 and stores the pool address in *pool_out only if
 * ALL of: gevntadr != 0, the range does not wrap, the reserved table
 * was not truncated, one reserved-memory range fully contains the
 * pool AND its node name starts with required_name (NULL = any; the
 * shim passes "abl" so a stale GEVNTADR pointing into some other
 * firmware carveout is refused), and the pool overlaps none of the
 * avoid[] ranges. Otherwise
 * returns nonzero and *why names the reason (static string). */
int usbpool_choose(const struct fdt_memmap *mm, uint64_t gevntadr,
		   size_t pool_len, const char *required_name,
		   const struct usbpool_avoid *avoid, unsigned n_avoid,
		   uint64_t *pool_out, const char **why);

#ifdef USB_DMA_HI_BASE
/* Dev knob (build.sh USB_DMA_HI_BASE): pool at a fixed PA inside a memory bank. */
int usbpool_choose_hi(const struct fdt_memmap *mm, uint64_t base,
		      size_t pool_len, const struct usbpool_avoid *avoid,
		      unsigned n_avoid, uint64_t *pool_out, const char **why);
#endif
