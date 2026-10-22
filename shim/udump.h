#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * udump.h - framed read-only userdata dump over the USB gadget
 * (build flag --udump).
 *
 * The dump streams the first N 4 KiB blocks of the GPT 'userdata'
 * partition as binary records between two text markers, so the host
 * receiver can resync inside the CDC-ACM text log:
 *
 *   text:    UDUMP-BEGIN first=<lba> count=<blocks>
 *   record:  "UDMP" | seq u32 | lba u64 (absolute) | nblocks u32
 *            | crc32 u32 (zlib CRC32 of payload) | payload nblocks*4096
 *   end:     same header, nblocks = 0, crc32 = CRC32 of ALL payload
 *   text:    UDUMP-END total=<crc32> blocks=<n>
 *
 * All values little endian. Records are sent as consecutive
 * usbgadget_write pieces of <= 4096 B; one record per 64 KiB chunk.
 *
 * udump.c has NO MMIO of its own: UFS reads go through the caller's
 * read hook (ufs.c's bounds-checked ufs_read_userdata on the device),
 * gadget I/O only through usbgadget_write/usbgadget_ready and the poll
 * hook, console text through con_str/con_hex2. It carries no static
 * state, so the object audit in tools/test-bluejay-boot.py can prove
 * every store lands on the stack or in an argument buffer.
 */

#define UDUMP_BLOCK		4096u
#define UDUMP_CHUNK_BLOCKS	16u		/* 64 KiB per record */
#define UDUMP_PIECE		4096u		/* usbgadget_write piece cap */
#define UDUMP_HDR		24u		/* record header bytes */

/* Compile-time dump cap (dump safety bound): default 16384 blocks = 64 MiB. */
#ifndef UDUMP_MAX_BLOCKS
#define UDUMP_MAX_BLOCKS	16384u
#endif

#ifdef UDUMP
/* shim/build.sh --udump N_MIB: blocks = N * 256; refuse at build time
 * if that exceeds the compile-time cap. */
#ifndef UDUMP_MIB
#define UDUMP_MIB	1
#endif
#define UDUMP_MIB_BLOCKS	((uint32_t)UDUMP_MIB * 256u)
#if UDUMP_MIB * 256 > UDUMP_MAX_BLOCKS
#error "UDUMP_MIB * 256 exceeds UDUMP_MAX_BLOCKS"
#endif
#endif

/* The dump's whole external surface, mirroring usbgadget.h's hooks so no
 * register access can reach this file (also lets the host test fake it):
 *  - read: move nblocks (<= 2) 4 KiB blocks at absolute LBA into buf;
 *    returns 0 on success. Bounded to the userdata GPT range by ufs.c.
 *  - poll: drive the gadget and one text-log pump step (device:
 *    usbgadget_poll + shim.c's usbg_pump; host test: a fake that also
 *    advances fake time).
 *  - log_pending: 1 while text-log bytes still await sending (used to
 *    flush UDUMP-BEGIN before the first record byte).
 *  - ticks/ticks_freq: monotonic time for the bounded waits. */
struct udump_hooks {
	uint32_t (*read)(uint64_t lba, uint32_t nblocks, void *buf);
	void (*poll)(void);
	int (*log_pending)(void);
	uint64_t (*ticks)(void);
	uint64_t ticks_freq;
};

/* Result codes (the reason line is printed before returning). */
enum udump_result {
	UDUMP_OK = 0,		/* end record sent */
	UDUMP_ERR_NOT_READY = 1,/* host never attached (5 s) */
	UDUMP_ERR_REFUSED = 2,	/* bounds/parameter refusal, nothing sent */
	UDUMP_ERR_READ = 3,	/* the UFS reader failed mid-dump */
	UDUMP_ERR_STALLED = 4,	/* flow control: 5 s chunk budget exhausted */
	UDUMP_ERR_WINDOW = 5,	/* the gadget window ended mid-dump */
};

/* Stream `blocks` 4 KiB blocks starting at absolute LBA ud_first, inside
 * [ud_first, ud_last], as framed records. Reads only
 * [ud_first, ud_first+blocks-1]; refuses (printing "UDUMP refused: ...",
 * sending nothing) when ud_first == 0, ud_last < ud_first,
 * blocks > partition size, blocks > UDUMP_MAX_BLOCKS, blocks == 0, the
 * LBA arithmetic would overflow, or any LBA exceeds 32 bits
 * (read_blocks_dma takes a u32 LBA). window_end is an absolute ticks
 * deadline bounding the whole dump. Returns a udump_result code. */
int udump_run(const struct udump_hooks *hooks, void *stage, size_t stage_len,
	      uint64_t ud_first, uint64_t ud_last, uint32_t blocks,
	      uint64_t window_end);
