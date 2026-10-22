/*
 * udump_host_test.c - host tests for shim/udump.c.
 *
 * Everything udump.c touches is faked here: a bounded fake partition
 * reader (records every LBA it serves), a recording usbgadget_write/
 * ready, a text-log pump that models shim.c's usbg_pump (con_str lines
 * queue up; each poll flushes one chunk, so WIRE ORDER is what gets
 * asserted), and fake ticks advanced by the pump (10 ms per poll) so
 * the 5 s flow-control bounds run in microseconds.
 *
 * Record CRCs are checked two independent ways: against constants
 * computed OUTSIDE this tree (python3 zlib over the same fake-partition
 * pattern), and, in the mutation tests, against crc32_zlib() below - an
 * in-test bitwise zlib CRC32 written for this file, never udump.c's own
 * implementation.
 *
 * The fake read hook records every (lba, nblocks) call it receives; the
 * mutation tests replay that log against the exact read sequence a
 * conforming dump must issue (16-block chunks, 2-block reads with a
 * 1-block tail per odd chunk, every read inside [ud_first, ud_last],
 * nothing at all when a refusal is expected). This kills the mutants
 * that survived the first suite (docs/reviews/udump-review-1.md M3).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "udump.h"

#define FAKE_FIRST	0x40000u
#define FAKE_N		512u			/* blocks; 2 MiB fake partition */
#define SINK_CAP	(4u << 20)
#define TESTS		16
/* A far-future but REAL deadline: window_end is an absolute tick value,
 * and the signed past() comparison would read WINDOW_FAR as "already
 * over". The device always passes usbg_end. */
#define WINDOW_FAR	100000000u

/* python3 zlib CRC32 of the pattern byte (lba*131 + i*7 + (lba>>8))&0xff
 * over the named block ranges (absolute LBAs start at FAKE_FIRST). */
#define CRC_C1		0xD3B3C7BCu	/* 1 block  [0,1)   */
#define CRC_C2		0xBBB0442Fu	/* 2 blocks [0,2)   */
#define CRC_C40_0	0xF7FA9252u	/* 16 blocks [0,16) */
#define CRC_C40_1	0x4E6CA513u	/* 16 blocks [16,32)*/
#define CRC_C40_2	0x61DA09A6u	/* 8 blocks [32,40) */
#define CRC_C40T	0xE1B05300u	/* 40 blocks total  */
#define CRC_C20_1	0x0B71C260u	/* 4 blocks [16,20) */
#define CRC_C20T	0x2867D7A0u	/* 20 blocks total  */

static uint8_t fake_part[FAKE_N * 4096];
static uint8_t fake_stage[UDUMP_CHUNK_BLOCKS * UDUMP_BLOCK];
static uint8_t sink[SINK_CAP];
static size_t sink_len;
static uint8_t pending[8192];
static size_t pending_len;
static uint64_t fticks;
static int ready_flag = 1;
static int busy_calls;
static int stall_forever;
static long stall_write_calls;
static int err_piece_len, err_sink_full;
static uint64_t rd_min_lba, rd_max_lba, rd_fail_at = UINT64_MAX;
static uint32_t rd_count, rd_max_blocks;
static struct udump_hooks hooks;

/* Every (lba, nblocks) the fake read hook receives, in call order. */
struct rd_call {
	uint64_t lba;
	uint32_t n;
};
#define RD_LOG_CAP	4096u
static struct rd_call rd_log[RD_LOG_CAP];
static unsigned rd_log_n;
static int rd_log_ovf;

/* Independent bitwise zlib CRC32 (reflected, poly 0xEDB88320, init and
 * final 0xFFFFFFFF), written for this test. Deliberately NOT udump.c's
 * incremental/CRC-instruction code: the oracle must not share an
 * implementation with the code under test. */
static uint32_t crc32_zlib(const uint8_t *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;

	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			if (c & 1u)
				c = 0xEDB88320u ^ (c >> 1);
			else
				c >>= 1;
		}
	}
	return c ^ 0xFFFFFFFFu;
}

#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("FAIL %s:%d: %s\n", __func__, __LINE__, (msg)); return 1; } \
} while (0)

static void fill_part(void)
{
	for (uint32_t lba = 0; lba < FAKE_N; lba++) {
		uint64_t abs_lba = FAKE_FIRST + lba;
		for (uint32_t i = 0; i < 4096; i++)
			fake_part[lba * 4096 + i] =
				(uint8_t)(abs_lba * 131 + i * 7 + (abs_lba >> 8));
	}
}

static uint32_t fake_read(uint64_t lba, uint32_t nblocks, void *buf)
{
	if (rd_log_n < RD_LOG_CAP) {
		rd_log[rd_log_n].lba = lba;
		rd_log[rd_log_n].n = nblocks;
	} else {
		rd_log_ovf = 1;
	}
	rd_log_n++;
	rd_count++;
	if (nblocks > rd_max_blocks)
		rd_max_blocks = nblocks;
	if (nblocks == 0 || nblocks > 2)
		return 1;
	if (lba < FAKE_FIRST || lba + nblocks - 1 >= (uint64_t)FAKE_FIRST + FAKE_N)
		return 1;
	if (lba >= rd_fail_at)
		return 1;
	if (lba < rd_min_lba)
		rd_min_lba = lba;
	if (lba + nblocks - 1 > rd_max_lba)
		rd_max_lba = lba + nblocks - 1;
	for (uint32_t b = 0; b < nblocks; b++)
		memcpy((uint8_t *)buf + b * 4096,
		       &fake_part[(size_t)(lba - FAKE_FIRST + b) * 4096], 4096);
	return 0;
}

size_t usbgadget_write(const char *buf, size_t len)
{
	if (len > 4096) {		/* dump protocol: pieces of <= 4096 B only */
		err_piece_len = 1;
		return 0;
	}
	if (stall_forever) {
		if (++stall_write_calls > 1000000) {
			fprintf(stderr, "FATAL: dump never stalled: no flow-control bound\n");
			exit(1);
		}
		return 0;
	}
	if (busy_calls > 0) {
		busy_calls--;
		return 0;
	}
	if (sink_len + len > SINK_CAP) {
		err_sink_full = 1;
		return 0;
	}
	memcpy(sink + sink_len, buf, len);
	sink_len += len;
	return len;
}

int usbgadget_ready(void)
{
	return ready_flag;
}

static void queue_text(const char *line)
{
	size_t n = strlen(line);
	if (pending_len + n > sizeof pending)
		return;
	memcpy(pending + pending_len, line, n);
	pending_len += n;
}

void con_str(const char *label, const char *s)
{
	char line[128];
	snprintf(line, sizeof line, "%s %s\r\n", label, s);
	queue_text(line);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char line[160];
	if (b && *b)
		snprintf(line, sizeof line, "%s %s%08X %s%08X\r\n", label, a, va, b, vb);
	else
		snprintf(line, sizeof line, "%s %s%08X\r\n", label, a, va);
	queue_text(line);
}

static void hooks_poll(void)
{
	fticks += 10;			/* 10 ms of fake time per pump step */
	if (pending_len) {
		size_t want = pending_len > 4096 ? 4096 : pending_len;
		size_t n = usbgadget_write((const char *)pending, want);
		pending_len -= n;
		memmove(pending, pending + n, pending_len);
	}
}

static int hooks_log_pending(void)
{
	return pending_len != 0;
}

static uint64_t hooks_ticks(void)
{
	return fticks;
}

static void reset_state(void)
{
	sink_len = pending_len = 0;
	fticks = 0;
	ready_flag = 1;
	busy_calls = stall_forever = 0;
	stall_write_calls = 0;
	err_piece_len = err_sink_full = 0;
	rd_count = rd_max_blocks = 0;
	rd_log_n = 0;
	rd_log_ovf = 0;
	rd_min_lba = UINT64_MAX;
	rd_max_lba = 0;
	rd_fail_at = WINDOW_FAR;
	memset(&hooks, 0, sizeof hooks);
	hooks.read = fake_read;
	hooks.poll = hooks_poll;
	hooks.log_pending = hooks_log_pending;
	hooks.ticks = hooks_ticks;
	hooks.ticks_freq = 1000;	/* 5 s budget = 5000 ticks = 500 polls */
}

static int run_dump(uint64_t first, uint64_t last, uint32_t blocks, uint64_t window_end)
{
	return udump_run(&hooks, fake_stage, sizeof fake_stage,
			 first, last, blocks, window_end);
}

static void flush_text(void)
{
	for (unsigned guard = 0; pending_len && guard < 10000; guard++)
		hooks_poll();
}

static long find_bytes(const char *needle, size_t from)
{
	size_t m = strlen(needle);
	for (size_t i = from; i + m <= sink_len; i++)
		if (!memcmp(sink + i, needle, m))
			return (long)i;
	return -1;
}

static uint32_t get_u32le(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get_u64le(const uint8_t *p)
{
	return (uint64_t)get_u32le(p) | (uint64_t)get_u32le(p + 4) << 32;
}

/* Offset of the first byte AFTER the UDUMP-BEGIN line, or -1. */
static long begin_off(void)
{
	long b = find_bytes("UDUMP-BEGIN", 0);
	if (b < 0)
		return -1;
	for (size_t i = (size_t)b; i < sink_len; i++)
		if (sink[i] == '\n')
			return (long)i + 1;
	return -1;
}

struct rec {
	uint32_t seq, nb, crc;
	uint64_t lba;
	size_t payload_off;
};

/* Walk records from off; fills out[] (max 8), sets *end_off past the end
 * record when seen. Returns record count (data records only). */
static int walk_records(size_t off, struct rec out[8], size_t *end_off, int *saw_end)
{
	uint32_t n = 0;
	*saw_end = 0;
	while (off + UDUMP_HDR <= sink_len) {
		const uint8_t *h = sink + off;
		if (memcmp(h, "UDMP", 4))
			break;
		if (n >= 8)
			break;
		out[n].seq = get_u32le(h + 4);
		out[n].lba = get_u64le(h + 8);
		out[n].nb = get_u32le(h + 16);
		out[n].crc = get_u32le(h + 20);
		off += UDUMP_HDR;
		if (out[n].nb == 0) {
			*end_off = off;
			*saw_end = 1;
			return n;
		}
		out[n].payload_off = off;
		off += (size_t)out[n].nb * 4096;
		if (off > sink_len)
			return -1;	/* truncated payload */
		n++;
	}
	return n;
}

static int payload_is_fake(size_t off, uint32_t part_off_blocks, uint32_t nb)
{
	return !memcmp(sink + off, fake_part + (size_t)part_off_blocks * 4096,
		       (size_t)nb * 4096);
}

/* The read sequence a conforming dump of `blocks` blocks at `first`
 * must issue: consecutive 16-block chunks, 2-block reads, the last read
 * of an odd chunk moves 1 block (the reader moves <= 2 blocks per
 * command). Fills out[] up to cap; returns the full expected count. */
static unsigned expected_reads(uint64_t first, uint32_t blocks,
			       struct rd_call out[], unsigned cap)
{
	unsigned n = 0;
	uint32_t done = 0;

	while (done < blocks) {
		uint32_t nb = blocks - done < UDUMP_CHUNK_BLOCKS ?
			      blocks - done : UDUMP_CHUNK_BLOCKS;
		for (uint32_t b = 0; b < nb; b += 2) {
			if (n < cap) {
				out[n].lba = first + done + b;
				out[n].n = nb - b > 1 ? 2u : 1u;
			}
			n++;
		}
		done += nb;
	}
	return n;
}

/* The recorded read log must be exactly the expected sequence, every
 * call 1..2 blocks and inside the dump's own [first, last], and the
 * log must not have been truncated. */
static int check_read_log(uint64_t first, uint64_t last, uint32_t blocks)
{
	struct rd_call want[80];
	unsigned wn = expected_reads(first, blocks, want, 80);
	char msg[160];

	CHECK(!rd_log_ovf, "read log truncated");
	snprintf(msg, sizeof msg, "read call count %u != expected %u",
		 rd_log_n, wn);
	CHECK(rd_log_n == wn, msg);
	for (unsigned i = 0; i < wn; i++) {
		snprintf(msg, sizeof msg,
			 "read #%u: got (lba=%llu n=%u), want (lba=%llu n=%u)",
			 i, (unsigned long long)rd_log[i].lba, rd_log[i].n,
			 (unsigned long long)want[i].lba, want[i].n);
		CHECK(rd_log[i].lba == want[i].lba && rd_log[i].n == want[i].n,
		      msg);
	}
	for (unsigned i = 0; i < rd_log_n; i++) {
		CHECK(rd_log[i].n >= 1 && rd_log[i].n <= 2,
		      "read nblocks outside 1..2");
		snprintf(msg, sizeof msg,
			 "read #%u (lba=%llu n=%u) outside [%llu, %llu]",
			 i, (unsigned long long)rd_log[i].lba, rd_log[i].n,
			 (unsigned long long)first, (unsigned long long)last);
		CHECK(rd_log[i].lba >= first &&
		      rd_log[i].lba + rd_log[i].n - 1 <= last, msg);
	}
	return 0;
}

static int test_framing_small(void)
{
	reset_state();
	int rc = run_dump(FAKE_FIRST, FAKE_FIRST + FAKE_N - 1, 2, WINDOW_FAR);
	CHECK(rc == UDUMP_OK, "rc");
	flush_text();
	CHECK(find_bytes("UDUMP-BEGIN first=262144 count=2", 0) >= 0, "BEGIN line");
	long b = begin_off(), u = find_bytes("UDMP", 0);
	CHECK(b >= 0 && u == b, "records start right after the BEGIN line");
	struct rec r[8];
	size_t end_off;
	int saw_end;
	int n = walk_records((size_t)b, r, &end_off, &saw_end);
	CHECK(n == 1 && saw_end, "one data record + end record");
	CHECK(r[0].seq == 0 && r[0].lba == FAKE_FIRST && r[0].nb == 2, "record 0 fields");
	CHECK(r[0].crc == CRC_C2, "record 0 CRC matches externally computed value");
	CHECK(payload_is_fake(r[0].payload_off, 0, 2), "record 0 payload equals fake partition");
	const uint8_t *e = sink + end_off - UDUMP_HDR;
	CHECK(get_u32le(e + 4) == 1 && get_u64le(e + 8) == FAKE_FIRST + 2 &&
	      get_u32le(e + 16) == 0 && get_u32le(e + 20) == CRC_C2, "end record fields");
	CHECK(find_bytes("UDUMP-END total=BBB0442F blocks=2", end_off) >= 0, "END line after end record");
	CHECK(rd_count == 1 && rd_min_lba == FAKE_FIRST && rd_max_lba == FAKE_FIRST + 1,
	      "reader served exactly [first, first+1]");
	CHECK(rd_max_blocks <= 2, "reader never asked for more than 2 blocks per command");
	CHECK(!err_piece_len && !err_sink_full, "all write pieces <= 4096 B");
	return 0;
}

static int test_framing_multi(void)
{
	reset_state();
	int rc = run_dump(FAKE_FIRST, FAKE_FIRST + FAKE_N - 1, 40, WINDOW_FAR);
	CHECK(rc == UDUMP_OK, "rc");
	flush_text();
	long b = begin_off();
	CHECK(b >= 0, "BEGIN line");
	struct rec r[8];
	size_t end_off;
	int saw_end;
	int n = walk_records((size_t)b, r, &end_off, &saw_end);
	CHECK(n == 3 && saw_end, "three data records + end record");
	CHECK(r[0].seq == 0 && r[0].lba == FAKE_FIRST && r[0].nb == 16 && r[0].crc == CRC_C40_0, "record 0");
	CHECK(r[1].seq == 1 && r[1].lba == FAKE_FIRST + 16 && r[1].nb == 16 && r[1].crc == CRC_C40_1, "record 1");
	CHECK(r[2].seq == 2 && r[2].lba == FAKE_FIRST + 32 && r[2].nb == 8 && r[2].crc == CRC_C40_2, "record 2 (partial chunk)");
	CHECK(payload_is_fake(r[0].payload_off, 0, 16) && payload_is_fake(r[1].payload_off, 16, 16) &&
	      payload_is_fake(r[2].payload_off, 32, 8), "payloads equal fake partition");
	const uint8_t *e = sink + end_off - UDUMP_HDR;
	CHECK(get_u32le(e + 4) == 3 && get_u32le(e + 16) == 0 && get_u32le(e + 20) == CRC_C40T,
	      "end record: seq 3, nblocks 0, CRC of ALL payload");
	CHECK(find_bytes("UDUMP-END total=E1B05300 blocks=40", end_off) >= 0, "END line");
	CHECK(rd_min_lba == FAKE_FIRST && rd_max_lba == FAKE_FIRST + 39, "reader bounds [F, F+39]");
	CHECK(!err_piece_len, "pieces <= 4096");
	return 0;
}

static int test_off_by_one(void)
{
	reset_state();
	int rc = run_dump(FAKE_FIRST, FAKE_FIRST + 19, 20, WINDOW_FAR);	/* partition exactly 20 blocks */
	CHECK(rc == UDUMP_OK, "N == partition size must succeed");
	flush_text();
	long b = begin_off();
	struct rec r[8];
	size_t end_off;
	int saw_end;
	int n = walk_records((size_t)b, r, &end_off, &saw_end);
	CHECK(n == 2 && saw_end, "chunks 16 + 4");
	CHECK(r[1].lba == FAKE_FIRST + 16 && r[1].nb == 4 && r[1].crc == CRC_C20_1, "final partial chunk");
	CHECK(rd_max_lba == FAKE_FIRST + 19, "last block read is exactly ud_last");
	CHECK(get_u32le(sink + end_off - UDUMP_HDR + 20) == CRC_C20T, "end CRC over exactly 20 blocks");

	reset_state();
	rc = run_dump(FAKE_FIRST, FAKE_FIRST + 19, 21, WINDOW_FAR);	/* one block too many */
	CHECK(rc == UDUMP_ERR_REFUSED, "N > partition must be refused");
	flush_text();
	CHECK(find_bytes("UDUMP refused", 0) >= 0, "refusal line");
	CHECK(find_bytes("UDMP", 0) < 0, "no record bytes on refusal");
	CHECK(rd_count == 0, "refusal reads nothing");
	return 0;
}

static int test_refusals(void)
{
	reset_state();
	CHECK(run_dump(FAKE_FIRST, FAKE_FIRST + 200000, UDUMP_MAX_BLOCKS + 1, WINDOW_FAR)
	      == UDUMP_ERR_REFUSED, "N > UDUMP_MAX_BLOCKS refused");
	flush_text();
	CHECK(rd_count == 0 && find_bytes("UDMP", 0) < 0, "nothing read, nothing sent");

	reset_state();
	CHECK(run_dump(0, 1000, 1, WINDOW_FAR) == UDUMP_ERR_REFUSED, "ud_first == 0 refused");
	flush_text();
	CHECK(rd_count == 0 && find_bytes("ud_first=0", 0) >= 0, "reason printed, nothing read");

	reset_state();
	CHECK(run_dump(0x100000000ULL, 0x100000000ULL + 65535, 256, WINDOW_FAR)
	      == UDUMP_ERR_REFUSED, "LBA > 2^32 refused");
	flush_text();
	CHECK(rd_count == 0 && find_bytes("32-bit", 0) >= 0, "reason printed, nothing read");

	reset_state();
	CHECK(run_dump(FAKE_FIRST, FAKE_FIRST + FAKE_N - 1, 0, WINDOW_FAR)
	      == UDUMP_ERR_REFUSED, "blocks == 0 refused");

	reset_state();
	CHECK(run_dump(FAKE_FIRST, FAKE_FIRST - 1, 1, WINDOW_FAR)
	      == UDUMP_ERR_REFUSED, "ud_last < ud_first refused");

	reset_state();
	int rc = udump_run(&hooks, fake_stage, 1024, FAKE_FIRST, FAKE_FIRST + FAKE_N - 1, 1, WINDOW_FAR);
	CHECK(rc == UDUMP_ERR_REFUSED, "stage buffer too small refused");
	return 0;
}

static int test_not_ready(void)
{
	reset_state();
	ready_flag = 0;
	int rc = run_dump(FAKE_FIRST, FAKE_FIRST + FAKE_N - 1, 2, WINDOW_FAR);
	CHECK(rc == UDUMP_ERR_NOT_READY, "not-ready refusal");
	flush_text();
	CHECK(find_bytes("gadget not ready", 0) >= 0, "reason line");
	CHECK(find_bytes("UDMP", 0) < 0 && rd_count == 0, "nothing sent, nothing read");
	CHECK(fticks >= 5000, "waited the full 5 s before giving up");
	return 0;
}

static int test_read_error(void)
{
	reset_state();
	rd_fail_at = FAKE_FIRST + 16;		/* second chunk's first read fails */
	int rc = run_dump(FAKE_FIRST, FAKE_FIRST + FAKE_N - 1, 40, WINDOW_FAR);
	CHECK(rc == UDUMP_ERR_READ, "read error code");
	flush_text();
	CHECK(find_bytes("UDUMP read failed at seq=1", 0) >= 0, "failure names the seq");
	long b = begin_off();
	struct rec r[8];
	size_t end_off;
	int saw_end;
	int n = walk_records((size_t)b, r, &end_off, &saw_end);
	CHECK(n == 1 && !saw_end, "chunk 0 streamed complete, no second record, no end record");
	return 0;
}

static int test_stall(void)
{
	reset_state();
	stall_forever = 1;
	int rc = run_dump(FAKE_FIRST, FAKE_FIRST + FAKE_N - 1, 2, WINDOW_FAR);
	CHECK(rc == UDUMP_ERR_STALLED, "stall code");
	stall_forever = 0;		/* the gadget recovered after the stall */
	flush_text();
	CHECK(find_bytes("UDUMP stalled at seq=0", 0) >= 0, "stall line names the seq");
	CHECK(fticks >= 5000, "the 5 s chunk budget bounded the stall");
	CHECK(find_bytes("UDUMP-END", 0) < 0, "no END after a stall");
	return 0;
}

static int test_busy_recover(void)
{
	reset_state();
	busy_calls = 3;			/* gadget busy for the first 3 writes */
	int rc = run_dump(FAKE_FIRST, FAKE_FIRST + FAKE_N - 1, 1, WINDOW_FAR);
	CHECK(rc == UDUMP_OK, "transient busy must not fail the dump");
	flush_text();
	long b = begin_off();
	struct rec r[8];
	size_t end_off;
	int saw_end;
	int n = walk_records((size_t)b, r, &end_off, &saw_end);
	CHECK(n == 1 && saw_end, "one record + end record");
	CHECK(r[0].nb == 1 && r[0].crc == CRC_C1, "1-block record CRC");
	CHECK(payload_is_fake(r[0].payload_off, 0, 1), "payload equals fake partition");
	CHECK(get_u32le(sink + end_off - UDUMP_HDR + 20) == CRC_C1, "end CRC over 1 block");
	return 0;
}

static int test_window(void)
{
	reset_state();
	int rc = run_dump(FAKE_FIRST, FAKE_FIRST + FAKE_N - 1, 20, 50);	/* window ends at tick 50 */
	CHECK(rc == UDUMP_ERR_WINDOW, "window-over code");
	flush_text();
	CHECK(find_bytes("UDUMP window over at seq=0", 0) >= 0, "window line names the seq");
	long b = begin_off();
	struct rec r[8];
	size_t end_off;
	int saw_end;
	int n = walk_records((size_t)b, r, &end_off, &saw_end);
	CHECK(n <= 1 && !saw_end, "at most one (partial) chunk, no end record");
	CHECK(rd_count == 8, "chunk 0's reads happened (16 blocks, 2 per command)");
	CHECK(find_bytes("UDUMP-END", 0) < 0, "no END past the window");
	return 0;
}

static int test_begin_drain_order(void)
{
	reset_state();
	con_str("PRE1", "seed");	/* pending text from earlier probe lines */
	con_str("PRE2", "seed");
	con_str("PRE3", "seed");
	int rc = run_dump(FAKE_FIRST, FAKE_FIRST + FAKE_N - 1, 1, WINDOW_FAR);
	CHECK(rc == UDUMP_OK, "rc");
	flush_text();
	long u = find_bytes("UDMP", 0);
	long begin = find_bytes("UDUMP-BEGIN", 0);
	long pre3 = find_bytes("PRE3 seed", 0);
	CHECK(u >= 0 && begin >= 0 && pre3 >= 0, "markers present");
	CHECK(pre3 < u, "all pending text flushed before the first record byte");
	CHECK(begin < u, "BEGIN line flushed before the first record byte");
	return 0;
}

/* --- mutants of docs/reviews/udump-review-1.md M3 ---------------------- */

/* Mutant (a): the `ud_last < ud_first` refusal removed. With only
 * ud_last = ud_first - 1 the wrapped part_blocks still refuses (as
 * "N > partition"), so the range reason is asserted too; any smaller
 * ud_last must be refused by THIS check alone. Nothing may be read. */
static int test_mut_refuse_last_below_first(void)
{
	static const struct {
		uint64_t last;
		uint32_t blocks;
	} cases[] = {
		{ FAKE_FIRST - 1, 1 },
		{ FAKE_FIRST - 2, 1 },
		{ FAKE_FIRST - 4096, 4 },
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		reset_state();
		CHECK(run_dump(FAKE_FIRST, cases[i].last, cases[i].blocks,
			       WINDOW_FAR) == UDUMP_ERR_REFUSED,
		      "invalid GPT range must be refused");
		flush_text();
		CHECK(find_bytes("GPT range invalid", 0) >= 0,
		      "range reason line printed");
		CHECK(rd_log_n == 0, "refusal read nothing");
		CHECK(find_bytes("UDMP", 0) < 0, "no record bytes on refusal");
	}
	return 0;
}

/* Mutant (b): the last read of a chunk moves 2 blocks instead of 1
 * (odd block counts). Odd-N single- and multi-chunk dumps must issue
 * exactly the expected sequence, ending in a 1-block read at
 * ud_first + N - 1 == ud_last. */
static int test_mut_tail_single_block(void)
{
	static const uint32_t counts[] = { 1, 5, 17 };

	for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
		reset_state();
		CHECK(run_dump(FAKE_FIRST, FAKE_FIRST + counts[i] - 1,
			       counts[i], WINDOW_FAR) == UDUMP_OK, "rc");
		flush_text();
		CHECK(check_read_log(FAKE_FIRST, FAKE_FIRST + counts[i] - 1,
				     counts[i]) == 0, "read log");
	}
	return 0;
}

/* Mutant (c): reads issued at lba + b + 1 (LBA skew). A >64-block run
 * replays 5 chunks of reads exactly, so a skew that starts after seq 3
 * dies too. */
static int test_mut_lba_skew(void)
{
	reset_state();
	CHECK(run_dump(FAKE_FIRST, FAKE_FIRST + 79, 80, WINDOW_FAR)
	      == UDUMP_OK, "rc");
	flush_text();
	CHECK(check_read_log(FAKE_FIRST, FAKE_FIRST + 79, 80) == 0,
	      "read log");
	return 0;
}

/* Mutant (d): one block past ud_last becomes readable (partition bound
 * off by one). N == partition + 1 must be refused without reading; a
 * full-range dump's reads must still end exactly at ud_last. */
static int test_mut_read_past_last(void)
{
	reset_state();
	CHECK(run_dump(FAKE_FIRST, FAKE_FIRST + 19, 21, WINDOW_FAR)
	      == UDUMP_ERR_REFUSED, "N == partition + 1 refused");
	flush_text();
	CHECK(find_bytes("UDMP", 0) < 0, "no record bytes");
	CHECK(rd_log_n == 0, "refusal read nothing");

	reset_state();
	CHECK(run_dump(FAKE_FIRST, FAKE_FIRST + 39, 40, WINDOW_FAR)
	      == UDUMP_OK, "rc");
	flush_text();
	CHECK(check_read_log(FAKE_FIRST, FAKE_FIRST + 39, 40) == 0,
	      "read log inside ud_last");
	return 0;
}

/* Mutant (e): the 32-bit limit moved to `> 0x100000000`, which lets a
 * dump whose LAST block is LBA 2^32 through. first + N - 1 must be
 * checked against 0xFFFFFFFF: this dump crosses exactly at 2^32. */
static int test_mut_lba_over_32bit(void)
{
	reset_state();
	CHECK(run_dump(0xFFFFFFF9ULL, 0x100001000ULL, 8, WINDOW_FAR)
	      == UDUMP_ERR_REFUSED, "last block at 2^32 must be refused");
	flush_text();
	CHECK(find_bytes("32-bit", 0) >= 0, "32-bit reason line");
	CHECK(rd_log_n == 0, "refusal read nothing");
	CHECK(find_bytes("UDUMP-BEGIN", 0) < 0, "refused before BEGIN");
	CHECK(find_bytes("UDMP", 0) < 0, "no record bytes");
	return 0;
}

/* Mutant (f): the per-record CRC of a partial final chunk computed over
 * 16 blocks instead of nb. Expected CRCs come from crc32_zlib(), this
 * test's own implementation, first pinned to the external python3
 * zlib constants (so the oracle is independent of udump.c). */
static int test_mut_partial_chunk_crc(void)
{
	static const uint32_t counts[] = { 5, 20, 17 };
	char msg[128];

	/* Pin the in-test oracle to the external python3 zlib values. */
	CHECK(crc32_zlib(fake_part, 1 * 4096) == CRC_C1, "oracle pin 1 block");
	CHECK(crc32_zlib(fake_part, 2 * 4096) == CRC_C2, "oracle pin 2 blocks");
	CHECK(crc32_zlib(fake_part, 16 * 4096) == CRC_C40_0,
	      "oracle pin 16 blocks");
	CHECK(crc32_zlib(fake_part + 16 * 4096, 16 * 4096) == CRC_C40_1,
	      "oracle pin 16 blocks at 16");

	for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
		uint32_t blocks = counts[i], done = 0;

		reset_state();
		CHECK(run_dump(FAKE_FIRST, FAKE_FIRST + FAKE_N - 1, blocks,
			       WINDOW_FAR) == UDUMP_OK, "rc");
		flush_text();
		long b = begin_off();
		CHECK(b >= 0, "BEGIN line");
		struct rec r[8];
		size_t end_off;
		int saw_end;
		int n = walk_records((size_t)b, r, &end_off, &saw_end);
		snprintf(msg, sizeof msg, "record count for %u blocks", blocks);
		CHECK(n == (int)((blocks + 15u) / 16u) && saw_end, msg);
		for (int j = 0; j < n; j++) {
			uint32_t nb = blocks - done < 16 ? blocks - done : 16;
			uint32_t want = crc32_zlib(fake_part + (size_t)done * 4096,
						   (size_t)nb * 4096);
			snprintf(msg, sizeof msg,
				 "record %d per-record CRC (N=%u chunk=%u blocks)",
				 j, blocks, nb);
			CHECK(r[j].crc == want, msg);
			done += nb;
		}
		snprintf(msg, sizeof msg, "end-record total CRC (N=%u)", blocks);
		CHECK(get_u32le(sink + end_off - UDUMP_HDR + 20) ==
		      crc32_zlib(fake_part, (size_t)blocks * 4096), msg);
	}
	return 0;
}

int main(void)
{
	fill_part();
	int (*tests[TESTS])(void) = {
		test_framing_small, test_framing_multi, test_off_by_one,
		test_refusals, test_not_ready, test_read_error, test_stall,
		test_busy_recover, test_window, test_begin_drain_order,
		test_mut_refuse_last_below_first, test_mut_tail_single_block,
		test_mut_lba_skew, test_mut_read_past_last,
		test_mut_lba_over_32bit, test_mut_partial_chunk_crc,
	};
	static const char *names[TESTS] = {
		"framing_small", "framing_multi", "off_by_one", "refusals",
		"not_ready", "read_error", "stall", "busy_recover", "window",
		"begin_drain_order",
		"mut_refuse_last_below_first", "mut_tail_single_block",
		"mut_lba_skew", "mut_read_past_last", "mut_lba_over_32bit",
		"mut_partial_chunk_crc",
	};
	int fails = 0;
	for (int i = 0; i < TESTS; i++) {
		if (tests[i]()) {
			printf("FAIL %s\n", names[i]);
			fails++;
		}
	}
	if (fails) {
		printf("udump_host_test: %d/%d tests FAILED\n", fails, TESTS);
		return 1;
	}
	printf("PASS udump_host_test: %d/%d tests\n", TESTS, TESTS);
	return 0;
}
