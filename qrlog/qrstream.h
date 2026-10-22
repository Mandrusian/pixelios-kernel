#pragma once
#include <stdint.h>

/*
 * QL2: append-only, compressed, loss-tolerant log stream over QR codes.
 *
 *   console bytes --reduce--> reduced stream --unit (qrz)--> records --> transport stream (ring)
 *     --blocks of D bytes--> generations of K blocks --> frames (SRC = a block, REP = XOR of a random subset of a generation)
 *     --base45--> QR (alphanumeric mode, fixed version) --> up to `tiles` QR codes stacked on the screen.
 *
 * Frame text = base45(body); body = hdr + B payload bytes + crc32(le) of everything before:
 *   hdr: u8 0x20|kind<<3|(klog-2)   kind 0 SRC, 1 REP;  u16le sid;  varint hi (complete blocks);  varint lo (first retained block);
 *        SRC: varint block;  REP: varint gen, varint seed
 *   block (B bytes) = u16le (offset of the first record that starts in this block, 0xFFFF none) + D bytes of the transport stream
 *   record = varint rlen<<1|stored, varint clen, varint reduced offset, varint raw log offset of the first line, u32le crc32(reduced bytes), clen bytes
 *   a 0x00 byte where a record should start is padding.
 * The frame schedule needs no feedback: 2 of 3 slots walk the stream once, each generation as its K plain blocks followed by
 * K*red/100 repair frames; the other slots rotate over the repair symbols of all complete generations (any K+~2 of them
 * rebuild a generation; the newest one gets every other slot while the log is still growing) and re-send the blocks of the
 * open generation (a repair frame there covers the blocks it has so far).
 * Host side: tools/qrl2.py. All state is static in a qs_t: no allocation.
 */
#ifndef QS_TX_SIZE
#define QS_TX_SIZE	(2u << 20)	/* transport ring: ~8-12 MB of reduced log */
#endif
#define QS_UNIT_MAX	8192u
#define QS_LINE_MAX	1024u
#define QS_RED_MAX	(QS_UNIT_MAX + QS_LINE_MAX + 96u)
#define QS_HDR_MAX	16u
#define QS_B_MAX	2048u
#define QS_MAXBLK	(QS_TX_SIZE / 64u)
#define QS_TEXT_MAX	4200u
#define QS_TILES_MAX	4u
#define QS_VER_MAX	30

typedef struct qs {
	unsigned ver, ecc, tiles, unit, klog, sid;
	unsigned B, D, cap_chars;
	/* reducer */
	uint64_t raw_next, line_raw0, red0_raw, red_base;
	unsigned red_len, lstart, ndrop, in_line;
	uint32_t hs, hf;
	uint32_t recS[32], recF[32];
	unsigned nrec;
	/* transport stream */
	uint64_t tx_len;
	/* frame scheduler */
	uint64_t next_new, bg, hi_seen;
	uint32_t tick, seed, grow_tick;
	unsigned repair_pct;	/* repair frames sent right behind each generation, percent of K */
	unsigned nofec;		/* 1: no repair frames, plain blocks in rotation (A/B switch, boot-arg sweetqr_fec=0) */
	uint8_t red[QS_RED_MAX];
	uint16_t bfirst[QS_MAXBLK];
	uint8_t tx[QS_TX_SIZE];
} qs_t;

/* Fix the QR geometry; ecc = qrcodegen_Ecc (0 LOW .. 3 HIGH). Returns B (payload bytes per frame, 0 = unusable config). */
unsigned qs_init(qs_t *q, unsigned ver, unsigned ecc, unsigned tiles, unsigned unit, unsigned klog, unsigned sid);
/* Console bytes [raw_off, raw_off+n): reduced, appended, compressed into the stream as units fill. */
void qs_feed(qs_t *q, uint64_t raw_off, const char *p, unsigned n);
/* Compress what is pending and pad the open block, so everything fed so far is deliverable. 1 = something changed. */
int qs_flush(qs_t *q);
int qs_pending(const qs_t *q);
/* Next frame text (NUL-terminated, <= QS_TEXT_MAX). Returns its length, 0 when no block is complete yet. */
unsigned qs_next_frame(qs_t *q, char *text);
uint64_t qs_hi(const qs_t *q);		/* complete blocks */
uint64_t qs_lo(const qs_t *q);		/* first retained block */
uint64_t qs_reduced(const qs_t *q);	/* reduced-stream bytes consumed into units so far */
uint64_t qs_stream_len(const qs_t *q);	/* transport-stream bytes produced so far */
/* Paint tile frames black on white on a 32-bit framebuffer (stride in bytes), stacked and centred. Returns the QR size in modules, 0 on failure. */
unsigned qs_draw(const qs_t *q, volatile uint32_t *fb, unsigned w, unsigned h, unsigned stride, const char *const *texts, unsigned ntiles);
unsigned qs_sizeof(void);
void qs_set_fec(qs_t *q, unsigned on);	/* 0: plain blocks only (no repair frames) */
void qs_set_red(qs_t *q, unsigned percent);	/* repair frames per generation sent inline with the first pass (default 0) */
