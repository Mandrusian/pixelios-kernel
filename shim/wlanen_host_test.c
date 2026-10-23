/* Host test for wlanen.c (--wlan-en, docs/research/29 W2b) against a FAKE pinctrl bank model: GPP14 / GPH2 / GPA7 CON + DAT with the Samsung semantics (a pin whose CON field is 1
 * reads its DAT latch, any other pin reads the external level), a fake PMU word, a fake counter, and fault / ignored-write injection. Checks the exact write set and order
 * (DAT then CON to enable; DAT then CON to restore), the printed lines and the summary `WLAN EN enable=.. sample=.. restore=..`, that EVERY refusal writes nothing, that every
 * failure path still restores, that a failed restore is loud (`!!!` lines, restore=FAILED), and the allowlist of the one writer w32 (two registers, masks 0x4 / 0xF00, CON field 0 or 1).
 * Uses the REAL generated board macros. Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -DWLAN_EN_RUN -I. -Iout -include out/board_config.h -o out/wlanen_host_test wlanen_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wlanen.c"

#if !(SOC_GS101 && defined(WLAN_GPP14))
#error "needs the bluejay board_config.h (shim/build.sh --board bluejay writes out/board_config.h)"
#endif

/* ---- fake hardware ---- */
static struct bank { uint32_t con, latch, ext; } b14, bh2, ba7;	/* ext = the level an input pin reads */
static uint32_t pmu_iso;
static uint64_t fake_freq, waited;
static struct { uintptr_t pa; uint32_t v; } wlog[32];
static unsigned nw, nwait_calls;
static uintptr_t rfault_pa;		/* reads of this address fault */
static unsigned wfault_n;		/* 1-based index of the write that faults (not applied); 0 = none */
static unsigned ignore_from;		/* writes from this 1-based index on are accepted but not applied; 0 = none */
static int clkreq_follows_en;		/* the module pulls CLKREQ (gph2-1) low once WLAN_EN is driven high */
static unsigned nreads_outside, npad_reads;
static char lines[256][RAMLOG_LINE_MAX + 8];
static unsigned nl;

uint64_t counter_freq(void) { return fake_freq; }
void wait_ticks(uint64_t t) { waited = t; nwait_calls++; }

void con_str(const char *label, const char *s)
{
	assert(nl < 256);
	assert(strlen(s) < RAMLOG_LINE_MAX);
	snprintf(lines[nl++], sizeof(lines[0]), "%s %s", label, s);
}

static struct bank *which(uintptr_t pa, int *dat)
{
	if (pa == WLAN_GPP14 || pa == WLAN_GPP14 + 4) { *dat = pa != WLAN_GPP14; return &b14; }
	if (pa == WLAN_GPH2 || pa == WLAN_GPH2 + 4) { *dat = pa != WLAN_GPH2; return &bh2; }
	if (pa == WLAN_GPA7 || pa == WLAN_GPA7 + 4) { *dat = pa != WLAN_GPA7; return &ba7; }
	return NULL;
}

static uint32_t dat_read(const struct bank *b)
{
	uint32_t v = 0;

	for (unsigned i = 0; i < 8; i++)
		v |= (((b->con >> (4 * i)) & 0xf) == 1 ? (b->latch >> i) & 1u : (b->ext >> i) & 1u) << i;
	return v;
}

int try_read32(uintptr_t pa, uint32_t *v)
{
	int dat;
	struct bank *b = which(pa, &dat);

	if (pa == rfault_pa)
		return 0;
	if (pa == WLAN_PMU_PCIE1_ISO) { *v = pmu_iso; return 1; }
	if (pa >= WLAN_GPP14 + 8 && pa <= WLAN_GPP14 + 0x14 && !(pa & 3)) { npad_reads++; *v = 0x1000u + (uint32_t)(pa - WLAN_GPP14); return 1; }	/* PUD / DRV / CONPDN / PUDPDN */
	if (!b) { nreads_outside++; return 0; }
	if (clkreq_follows_en && b == &bh2)
		bh2.ext = (bh2.ext & ~2u) | ((((b14.con >> 8) & 0xf) == 1 && (b14.latch & 4)) ? 0u : 2u);
	*v = dat ? dat_read(b) : b->con;
	return 1;
}

int try_write32(uintptr_t pa, uint32_t v)
{
	int dat;
	struct bank *b = which(pa, &dat);

	assert(nw < 32);
	wlog[nw].pa = pa;
	wlog[nw].v = v;
	nw++;
	if (wfault_n && nw == wfault_n)
		return 0;
	if (!b)
		return 0;
	if (ignore_from && nw >= ignore_from)
		return 1;
	if (dat)
		b->latch = v;
	else
		b->con = v;
	return 1;
}

static void world(void)
{
	memset(wlog, 0, sizeof(wlog));
	nw = nl = nwait_calls = nreads_outside = npad_reads = 0;
	waited = 0;
	fake_freq = 24576000ull;
	rfault_pa = 0;
	wfault_n = ignore_from = 0;
	clkreq_follows_en = 0;
	pmu_iso = 0;
	b14.con = 0x33; b14.latch = 0; b14.ext = 0x3;	/* run w1: GPP14.CON 0x33, DAT 0x3 (pins 0,1 high, WLAN_EN low) */
	bh2.con = 0; bh2.latch = 0; bh2.ext = 0x2;	/* PERST input low, CLKREQ high, DEVWAKE low */
	ba7.con = 0; ba7.latch = 0; ba7.ext = 0;	/* HOSTWAKE low */
}

static int has(const char *s)
{
	for (unsigned i = 0; i < nl; i++)
		if (strstr(lines[i], s))
			return 1;
	return 0;
}

static void dump_fail(int line, const char *expr)
{
	fprintf(stderr, "FAIL line %d: %s\n", line, expr);
	for (unsigned i = 0; i < nl; i++)
		fprintf(stderr, "  %s\n", lines[i]);
	for (unsigned j = 0; j < nw; j++)
		fprintf(stderr, "  w %08lx <- %08x\n", (unsigned long)wlog[j].pa, wlog[j].v);
	assert(0);
}

#define EXPECT(c) do { if (!(c)) dump_fail(__LINE__, #c); } while (0)

/* every write of the run is to GPP14.CON / GPP14.DAT and changes only the WLAN_EN field / bit relative to the word read at that moment (checked on the log values) */
static void writes_only_gpp14(void)
{
	for (unsigned i = 0; i < nw; i++)
		EXPECT(wlog[i].pa == WLAN_GPP14 || wlog[i].pa == WLAN_GPP14 + 4);
}

int main(void)
{
	/* the constants against the cited ones */
	EXPECT(EN_CON == 0x108401C0 && EN_DAT == 0x108401C4 && EN_SHIFT == 8 && EN_CON_MASK == 0xF00 && EN_DAT_MASK == 0x4 && PERST_SHIFT == 0 && PERST_DAT_MASK == 0x1);
	EXPECT(PERST_CON == 0x14440000 && PERST_DAT == 0x14440004 && WAIT_MS == 200 && FUNC_INPUT == 0 && FUNC_OUTPUT == 1);

	/* happy path: DAT then CON to enable, 200 ms, sample, DAT then CON to restore */
	world();
	EXPECT(wlan_en_run() == 0);
	EXPECT(nw == 4);
	EXPECT(wlog[0].pa == EN_DAT && wlog[0].v == 0x7);		/* DAT 0x3 | bit 2 */
	EXPECT(wlog[1].pa == EN_CON && wlog[1].v == 0x133);		/* field [11:8] := 1 (OUTPUT), the rest of 0x33 untouched */
	EXPECT(wlog[2].pa == EN_DAT && wlog[2].v == 0x3);		/* back to the saved bit while still an output (read 0x7) */
	EXPECT(wlog[3].pa == EN_CON && wlog[3].v == 0x33);		/* input again */
	writes_only_gpp14();
	EXPECT(nwait_calls == 1 && waited == 24576000ull / 1000u * 200u);
	EXPECT(b14.con == 0x33 && dat_read(&b14) == 0x3 && bh2.con == 0 && ba7.con == 0);
	EXPECT(has("WLAN EN before GPP14 CON=00000033 DAT=00000003") && has("WLAN EN before GPH2 CON=00000000 DAT=00000002"));
	EXPECT(has("WLAN EN after enable CON=00000133 DAT=00000007"));
	EXPECT(has("WLAN EN sample GPP14 CON=00000133 DAT=00000007") && has("WLAN EN sample GPH2 DAT=00000002 GPA7.DAT=00000000"));
	EXPECT(has("WLAN EN restore CON=00000033 DAT=00000003"));
	EXPECT(has("WLAN EN enable=ok sample=10100 restore=ok") && !has("!!!"));
	EXPECT(has("WLAN EN before pads GPP14 PUD=00001008 DRV=0000100C") && has("WLAN EN before pads GPP14 CONPDN=00001010 PUDPDN=00001014"));
	EXPECT(has("WLAN EN after pads GPP14 PUD=00001008 DRV=0000100C") && npad_reads == 8 && nreads_outside == 0);

	/* the module reacts: CLKREQ falls while WLAN_EN is high -> the sample shows it */
	world(); clkreq_follows_en = 1;
	EXPECT(wlan_en_run() == 0 && has("WLAN EN enable=ok sample=10000 restore=ok"));

	/* refusals write NOTHING */
	{
		struct { const char *what; void (*mut)(void); const char *why; } cases[] = {
			{ "WLAN_EN high", NULL, "WLAN_EN gpp14-2 reads high already" },
			{ "WLAN_EN output", NULL, "WLAN_EN gpp14-2 is not a plain input" },
			{ "WLAN_EN pad function", NULL, "WLAN_EN gpp14-2 is not a plain input" },
			{ "PERST high", NULL, "PERST gph2-0 reads high" },
			{ "PERST pad function", NULL, "PERST gph2-0 is in a pad function" },
		};
		for (unsigned i = 0; i < 5; i++) {
			world();
			if (i == 0) b14.ext |= 4;
			if (i == 1) b14.con = 0x133;
			if (i == 2) b14.con = 0x233;
			if (i == 3) bh2.ext |= 1;
			if (i == 4) bh2.con = 0x2;
			EXPECT(wlan_en_run() == 1 && nw == 0 && nwait_calls == 0);
			EXPECT(has(cases[i].why) && has("WLAN EN refused: nothing written") && has("WLAN EN enable=refused sample=n/a restore=n/a"));
		}
		world(); fake_freq = 0;
		EXPECT(wlan_en_run() == 1 && nw == 0 && has("no timer") && has("enable=refused"));
		for (unsigned i = 0; i < 4; i++) {
			uintptr_t pa[4] = { EN_CON, EN_DAT, PERST_CON, PERST_DAT };

			world(); rfault_pa = pa[i];
			EXPECT(wlan_en_run() == 1 && nw == 0 && has("unreadable, nothing written") && has("enable=refused"));
		}
	}

	/* the first write faults (DAT, not applied): enable=fault, no 200 ms wait, the restore still runs (it writes DAT and CON back; the bank ends as it began) */
	world(); wfault_n = 1;
	EXPECT(wlan_en_run() == 1);
	EXPECT(nw == 3 && wlog[0].pa == EN_DAT && wlog[1].pa == EN_DAT && wlog[2].pa == EN_CON && nwait_calls == 0);
	EXPECT(has("WLAN EN enable=fault sample=n/a restore=ok") && !has("!!!"));
	EXPECT(b14.con == 0x33 && dat_read(&b14) == 0x3 && b14.latch == 0x3);

	/* the second write faults (CON): the DAT latch was set while the pin is still an input -> the restore MUST clear the latch (unobservable by reading) */
	world(); wfault_n = 2;
	EXPECT(wlan_en_run() == 1);
	EXPECT(nw == 4 && wlog[0].pa == EN_DAT && wlog[0].v == 0x7 && wlog[1].pa == EN_CON && wlog[2].pa == EN_DAT && wlog[2].v == 0x3 && wlog[3].pa == EN_CON && wlog[3].v == 0x33);
	EXPECT(b14.latch == 0x3 && (b14.latch & 4) == 0 && b14.con == 0x33 && nwait_calls == 0);
	EXPECT(has("WLAN EN enable=fault"));

	/* the CON write is accepted but does not take (read-back mismatch): enable=fault, restore ok, no wait */
	world(); ignore_from = 2;
	EXPECT(wlan_en_run() == 1 && nwait_calls == 0);
	EXPECT(has("WLAN EN enable=fault sample=n/a restore=ok") && b14.con == 0x33);

	/* the restore fails: the last write (CON back to input) is ignored -> output left, LOUD */
	world(); ignore_from = 4;
	EXPECT(wlan_en_run() == 1);
	EXPECT(nw == 4 && b14.con == 0x133 && ((b14.latch >> 2) & 1) == 0);
	EXPECT(has("WLAN EN enable=ok sample=10100 restore=FAILED") && has("!!! RESTORE FAILED: WLAN_EN gpp14-2 may be LEFT as an output / HIGH") && has("WLAN EN restore CON=00000133"));

	/* the DAT restore is ignored too: pin still HIGH */
	world(); ignore_from = 3;
	EXPECT(wlan_en_run() == 1 && has("restore=FAILED") && has("!!! RESTORE FAILED") && (b14.latch & 4) && b14.con == 0x133);

	/* a read-back fault during the restore is a failed restore, not an ok */
	world();
	EXPECT(wlan_en_run() == 0);
	world(); wfault_n = 4;	/* CON restore write faults */
	EXPECT(wlan_en_run() == 1 && has("restore=FAILED"));

	/* the writer's allowlist: any other register or mask is refused with no store */
	world();
	nw = 0;
	EXPECT(w32(WLAN_GPH2, 1) == -2 && w32(WLAN_GPH2 + 4, 1) == -2 && w32(WLAN_GPA7, 1) == -2 && w32(WLAN_PMU_PCIE1_ISO, 1) == -2 && w32(0x17463EC4, 1) == -2 && nw == 0);
	EXPECT(w32(WLAN_GPP14 + 8, 1) == -2 && w32(WLAN_GPP14 - 4, 1) == -2 && nw == 0);
	EXPECT(wmask(EN_CON) == 0xF00 && wmask(EN_DAT) == 0x4 && wmask(EN_CON + 8) == 0 && wmask(PERST_CON) == 0);
	/* bits outside the mask are ignored by w32 (it can only change the masked bits) */
	EXPECT(w32(EN_DAT, 0xffffffffu) == 0 && wlog[0].v == (0x3 | 0x4) && nw == 1);
	world();
	EXPECT(w32(EN_CON, 0xffffffffu) == -2 && nw == 0);		/* field 0xF is a pad function: refused */
	EXPECT(w32(EN_CON, 0x2u << 8) == -2 && w32(EN_CON, 0x3u << 8) == -2 && nw == 0);
	EXPECT(w32(EN_CON, 0x1u << 8) == 0 && wlog[0].v == 0x133 && w32(EN_CON, 0) == 0 && wlog[1].v == 0x33);
	EXPECT(w32_allowed(EN_CON, 0x33, 0x133) && !w32_allowed(EN_CON, 0x33, 0x134) && !w32_allowed(EN_CON, 0x33, 0x333) && !w32_allowed(EN_DAT, 3, 0xb) && w32_allowed(EN_DAT, 3, 7) && !w32_allowed(PERST_DAT, 0, 1));

	puts("PASS wlanen host test");
	return 0;
}
