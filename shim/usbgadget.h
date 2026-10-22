#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * usbgadget.h - stage-2 polled DWC3 device-mode driver core for the
 * bluejay shim (stage 2). Ported from the
 * working reference xnu/patches/0071-sweet-usb-device-ep0.patch
 * (Qualcomm DWC3 v3.20a that enumerated on real hardware); the
 * protocol decisions come from shim/usbproto.[ch].
 *
 * Safety contract (audited by tools/test-bluejay-boot.py):
 *  - every register WRITE goes through the single static helper w32()
 *    in usbgadget.c, which refuses offsets outside the DWC3 device
 *    EXACT register set of base 0x11110000 (usbprobe.c): GEVNTADR/SIZ/
 *    COUNT, DCFG, DCTL, DEVTEN, DALEPENA and the DEPCMD blocks of
 *    physical eps 0..3 (w32_allowed in usbgadget.c); nothing else;
 *  - register access happens only through the fault-guarded extern
 *    helpers try_read32()/try_write32() (shim.c);
 *  - every wait is a tick-based timeout taken from the caller's
 *    hooks; nothing loops forever;
 *  - a failed access sets a sticky fault flag, prints "USB gadget
 *    fault" once and permanently stops the gadget (fail closed).
 *
 * The caller owns the DMA pool: it validates the pool against the FDT
 * (reserved range containing the ABL GEVNTADR, clear of the shim
 * image / FDT / initrd / ramoops zone); usbgadget_init only re-checks
 * size and alignment. Pool layout mirrors the 0071 patch's UD_* map.
 */

/* Fault-guarded MMIO access, definitions in shim.c (device) and in
 * the host test (fake register file). try_* return 1 on success. */
int try_read32(uintptr_t address, uint32_t *value);
int try_write32(uintptr_t address, uint32_t value);

/* Time source for the bounded waits: shim.c passes the counter idiom
 * it already uses (now() / counter_freq()); the host test fakes both.
 * ticks() must be monotonic; ticks_freq is its frequency in Hz. */
struct usbgadget_hooks {
	uint64_t (*ticks)(void);
	uint64_t ticks_freq;
};

/* Minimum DMA pool (32 KiB, the stage-2 design) and the layout inside it.
 * Offsets marked 0071 are the patch's UD_* values; the bulk ring is
 * this port's addition (the patch has no bulk endpoint). */
#define USBG_POOL_MIN		0x8000u
#define USBG_UD_EVT		0x0000u	/* 0071 patch UD_EVT */
#define USBG_UD_EVT_SIZE	0x1000u	/* 0071 patch UD_EVT_SIZE (4 KiB) */
#define USBG_UD_TRB_OUT		0x1000u	/* 0071 patch UD_TRB_OUT */
#define USBG_UD_TRB_IN		0x1010u	/* 0071 patch UD_TRB_IN */
#define USBG_UD_SETUP		0x1040u	/* 0071 patch UD_SETUP (8 B) */
#define USBG_UD_DATA		0x1100u	/* 0071 patch UD_DATA (512 B) */
#define USBG_UD_BULK_TRB	0x1400u	/* 4-TRB bulk IN ring (this port) */
#define USBG_UD_BULK_DATA	0x1500u	/* 4 KiB bulk IN buffer (this port) */

/* Bring up the gadget with its DMA pool at pool_pa. Returns 0 on
 * success; any failure is fail-closed (prints the reason, leaves the
 * core's registers exactly as aborted, returns nonzero). */
int usbgadget_init(uintptr_t pool_pa, size_t pool_len,
		   const struct usbgadget_hooks *hooks);

/* Consume pending events (max 16 per call), run the ep0 state machine
 * and complete bulk IN transfers. Nonblocking; call it from any wait
 * loop. A register fault inside permanently stops it. */
void usbgadget_poll(void);

/* Queue up to 4096 bytes for bulk IN ep1 (single TRB). Returns the
 * number of bytes accepted: 0 when not configured, DTR not set, a
 * transfer is still in flight, or the gadget is faulted. */
size_t usbgadget_write(const char *buf, size_t len);

/* Streaming is possible: configured AND DTR set AND no fault. */
int usbgadget_ready(void);

/* Halt the core (clear RUN_STOP, wait <= 500 ms) and make the gadget
 * inert. Call when the window ends. Safe if never initialised. */
void usbgadget_stop(void);

/* Sticky fault flag: a register access failed or a command errored. */
int usbgadget_faulted(void);
