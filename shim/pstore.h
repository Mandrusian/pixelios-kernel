#pragma once
#include <stdint.h>

/* Build identifier baked into the log marker line. build.sh passes the
 * git short hash (or a date) via -DPSTORE_BUILD_ID. */
#ifndef PSTORE_BUILD_ID
#define PSTORE_BUILD_ID "unknown"
#endif

/* First line written to the ramoops console zone; tools/pstore-pull.py
 * greps for "PIXELIOS PSTORE v1" - keep the two in sync. */
extern const char pstore_marker[];

/* Parse the live FDT ramoops node, mirror the kernel's zone math, and
 * run the safety gates (reserved-range containment, no overlap with the
 * shim image/stacks, FDT blob, initrd, or any other reserved range).
 * Returns 1 when writes are enabled; 0 after printing why not. */
int pstore_init(uintptr_t fdt);

/* Write the whole RAM log into the console zone (no-op when init
 * failed or a save is already in flight elsewhere). Re-entrant from an
 * exception on the same CPU: every store is bounds-checked and the
 * signature is written last, so a torn write reads as empty, never as
 * corrupt. All loops are bounded. */
void pstore_save(void);

/* Panic-path entry for the exception handler: like pstore_save(), but
 * a fault inside the save must not recurse - a nested call on the same
 * CPU prints one line and skips the save, and the panic path (screen,
 * pager, reset) continues. */
void pstore_panic_save(void);

int pstore_ready(void);
uint64_t pstore_zone_addr(void);
/* Size of the validated console zone, or 0 when writes are not armed
 * (read-only accessor for other modules' overlap checks). */
uint64_t pstore_zone_size(void);
int pstore_used_fallback(void);
void pstore_reset(void);	/* host tests only */

/* Kernel-parity console zone math (exposed for host unit tests):
 * returns the offset of the console zone within the ramoops region, or
 * (uint64_t)-1 when no console zone can be written (sizes exceed the
 * region, console size zero, or the kernel's probe would fail); stores
 * the console data capacity (console-size minus the 12-byte
 * persistent_ram_buffer header) in *cap_out when non-NULL. */
uint64_t pstore_console_offset(uint64_t region_size, uint32_t record_size,
			       uint32_t console_size, uint32_t ftrace_size,
			       uint32_t pmsg_size, uint32_t *cap_out);
