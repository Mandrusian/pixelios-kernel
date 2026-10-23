/* Host test for wlanlink.c (--wlan-link, docs/research/29 section 14) against a FAKE SoC: GPIO banks with Samsung semantics (a pin whose CON field is 1 reads its DAT latch, any other pin the external
 * level), plain register files for PMA / PCS / ELBI / I/A / DBI that read back what was written, lock bits that appear after k polls, the PMU isolation word behind a fake of the two EL3 steps
 * (wlan_link_iso_set / _clear), and fault / timeout / ignored-write injection. Built once per N (-DWLAN_LINK_N=7|8|10|19|20|21|22|23|24), see build.sh. Checks: the default run (N = 19) never raises PERST,
 * never enables the LTSSM and ends with the pins, the isolation and the PHY back where they were; N = 20 / 21 add the PERST release and the options; N = 22 adds the documented S22 skip (one 0 us delay row, no write); N = 23 enables the LTSSM and waits for L0 (fake: ELBI 0x2C8 == 0x11
 * after k polls, never, or a read fault), with a fault injected at EVERY forward write row and the teardown checked against the table order; N = 25 adds S25 (read-only iATU layout: DBI words + the ATU block probe + ATU region 0 + ELBI, `LINK R25` lines); N = 24 adds S24, read-only rows whose `log` flag prints `LINK R24 row=<i> rd=0x..` (the link report), no write, teardown unchanged; every failure branch (unreadable probe, lock timeout, write fault, refused / failed isolation, failed restore) stops, tears down in the table order with the isolation clear LAST and is
 * loud; the writer's independent allowlist refuses every other register. Uses the REAL generated board macros and out/wlan_seq.h. Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -DWLAN_LINK_RUN -DWLAN_LINK_N=19 -I. -Iout -include out/board_config.h -o out/wlanlink_host_test19 wlanlink_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FULL_MASK_T 0xffffffffu
#define WLAN_STR2(x) #x
#define WLAN_STR(x) WLAN_STR2(x)
#ifndef WLAN_LINK_N
#define WLAN_LINK_N 19
#endif
/* S24 and S25 each read ELBI 0x2C8 once more, after the L0 wait: one extra poll of the L0 fake each */
#if WLAN_LINK_N >= 25
#define L0X 2u
#elif WLAN_LINK_N >= 24
#define L0X 1u
#else
#define L0X 0u
#endif
#define WLAN_LINK_N_STR WLAN_STR(WLAN_LINK_N)
#include "wlanlink.c"

#if !(SOC_GS101 && defined(WLAN_LINK_RUN) && defined(WLAN_PMA))
#error "needs the bluejay board_config.h and -DWLAN_LINK_RUN"
#endif

/* ---- fake SoC ---- */
struct bank { uint32_t con, latch, ext, w[6]; };
static struct bank gp14, gh2;
static uint32_t pma[0x2000 / 4], pcs[0x1000 / 4], elbi[0x2000 / 4], ia[0x1000 / 4], dbi[0x1000 / 4], atu[0x1000 / 4];
static uint32_t iso1, iso0;
static unsigned pll_after, cdr_after, oc_after, pll_cnt, cdr_cnt, oc_cnt, l0_after, l0_cnt, l0_polls;
static int never_l0, fault_l0_read;
static uint64_t waited_at_en;
static int never_pll, unreadable_blk, iso_set_ret, iso_clear_ret, write_fault_n, ignore_pa_set;
static uintptr_t ignore_pa, fault_read_pa;
static uint64_t freq, tick, waited_us;
static unsigned nsave;
static char lines[320][RAMLOG_LINE_MAX + 8];
static unsigned nl;
enum { EV_W, EV_ISO_SET, EV_ISO_CLR };
static struct { int t; uintptr_t pa; uint32_t v; } ev[600];
static unsigned nev, nwrites;

uint64_t counter_freq(void) { return freq; }
void wait_ticks(uint64_t t) { tick += t; waited_us += freq ? t / (freq / 1000000u) : 0; }
uint64_t touch_ticks(void) { return tick += 3; }
void pstore_save(void) { nsave++; }
void con_str(const char *l, const char *s)
{
	assert(nl < 320 && strlen(s) < RAMLOG_LINE_MAX);
	snprintf(lines[nl++], sizeof(lines[0]), "%s %s", l, s);
}

static uint32_t *cell(uintptr_t pa)
{
	if (pa >= WLAN_PMA && pa < WLAN_PMA + 0x2000) return &pma[(pa - WLAN_PMA) / 4];
	if (pa >= WLAN_PCS && pa < WLAN_PCS + 0x1000) return &pcs[(pa - WLAN_PCS) / 4];
	if (pa >= WLAN_ELBI && pa < WLAN_ELBI + 0x2000) return &elbi[(pa - WLAN_ELBI) / 4];
	if (pa >= WLAN_IA && pa < WLAN_IA + 0x1000) return &ia[(pa - WLAN_IA) / 4];
	if (pa >= WLAN_DBI && pa < WLAN_DBI + 0x1000) return &dbi[(pa - WLAN_DBI) / 4];
	if (pa >= WLAN_ATU && pa < WLAN_ATU + 0x1000) return &atu[(pa - WLAN_ATU) / 4];
	return NULL;
}

static uint32_t dat_read(const struct bank *b)
{
	uint32_t v = 0;

	for (unsigned i = 0; i < 8; i++)
		v |= (((b->con >> (4 * i)) & 0xf) == 1 ? (b->latch >> i) & 1u : (b->ext >> i) & 1u) << i;
	return v;
}

static int blk_unreadable(uintptr_t pa)
{
	return (unreadable_blk == 1 && pa >= WLAN_PMA && pa < WLAN_PMA + 0x2000) || (unreadable_blk == 2 && pa >= WLAN_IA && pa < WLAN_IA + 0x1000) ||
	       (unreadable_blk == 3 && pa >= WLAN_ELBI && pa < WLAN_ELBI + 0x2000) || (unreadable_blk == 4 && pa >= WLAN_DBI && pa < WLAN_DBI + 0x1000) ||
	       (unreadable_blk == 5 && pa >= WLAN_ATU && pa < WLAN_ATU + 0x1000);
}

int try_read32(uintptr_t pa, uint32_t *v)
{
	uint32_t *c;

	if (pa == fault_read_pa || blk_unreadable(pa))
		return 0;
	if (pa == WLAN_PMU_ALIVE + 0x3EC4) { *v = iso1; return 1; }
	if (pa == WLAN_PMU_ALIVE + 0x3EC0) { *v = iso0; return 1; }
	if (pa >= WLAN_GPP14 && pa < WLAN_GPP14 + 0x18) {
		unsigned o = (unsigned)(pa - WLAN_GPP14);
		*v = o == 0 ? gp14.con : o == 4 ? dat_read(&gp14) : gp14.w[o / 4];
		return 1;
	}
	if (pa >= WLAN_GPH2 && pa < WLAN_GPH2 + 0x18) {
		unsigned o = (unsigned)(pa - WLAN_GPH2);
		*v = o == 0 ? gh2.con : o == 4 ? dat_read(&gh2) : gh2.w[o / 4];
		return 1;
	}
	c = cell(pa);
	assert(c && "read outside every modelled block");
	*v = *c;
	if (pa == WLAN_ELBI + 0x2C8 && (elbi[0x54 / 4] & 1u)) {		/* RDLH_LINKUP / LTSSM state: only polled after the LTSSM enable */
		l0_polls++;
		if (fault_l0_read)
			return 0;
		*v = (*v & ~0x3fu) | (!never_l0 && ++l0_cnt >= l0_after ? 0x11u : 0x0u);
	}
	if (pa == WLAN_PMA + 0x3F0) { if (!never_pll && ++pll_cnt >= pll_after) *v |= 8; else *v &= ~8u; }
	if (pa == WLAN_PMA + 0xE0C) { if (++cdr_cnt >= cdr_after) *v |= 4; else *v &= ~4u; }
	if (pa == WLAN_PMA + 0xDE8) { if (++oc_cnt >= oc_after) *v |= 0xf; else *v &= ~0xfu; }
	return 1;
}

int try_write32(uintptr_t pa, uint32_t v)
{
	uint32_t *c;

	assert(nev < 600);
	ev[nev].t = EV_W; ev[nev].pa = pa; ev[nev].v = v; nev++;
	nwrites++;
	if (pa == WLAN_ELBI + 0x54 && (v & 1u))
		waited_at_en = waited_us;
	if (write_fault_n && nwrites == (unsigned)write_fault_n)
		return 0;
	if (ignore_pa_set && pa == ignore_pa)
		return 1;
	if (pa >= WLAN_GPP14 && pa < WLAN_GPP14 + 0x18) {
		unsigned o = (unsigned)(pa - WLAN_GPP14);
		if (o == 0) gp14.con = v; else if (o == 4) gp14.latch = v; else gp14.w[o / 4] = v;
		return 1;
	}
	if (pa >= WLAN_GPH2 && pa < WLAN_GPH2 + 0x18) {
		unsigned o = (unsigned)(pa - WLAN_GPH2);
		if (o == 0) gh2.con = v; else if (o == 4) gh2.latch = v; else gh2.w[o / 4] = v;
		return 1;
	}
	c = cell(pa);
	assert(c && "write outside every modelled block");
	*c = v;
	return 1;
}

int wlan_link_iso_set(uint32_t *rd)
{
	ev[nev].t = EV_ISO_SET; ev[nev].pa = 0; ev[nev].v = 0; nev++;
	if (iso_set_ret == 0)
		iso1 |= 1;
	*rd = iso1;
	return iso_set_ret;
}

int wlan_link_iso_clear(uint32_t *rd)
{
	ev[nev].t = EV_ISO_CLR; ev[nev].pa = 0; ev[nev].v = 0; nev++;
	if (iso_clear_ret == 0)
		iso1 &= ~1u;
	*rd = iso1;
	return iso_clear_ret;
}

static void world(void)
{
	memset(pma, 0, sizeof(pma)); memset(pcs, 0, sizeof(pcs)); memset(elbi, 0, sizeof(elbi)); memset(ia, 0, sizeof(ia)); memset(dbi, 0, sizeof(dbi)); memset(atu, 0, sizeof(atu));
	memset(&gp14, 0, sizeof(gp14)); memset(&gh2, 0, sizeof(gh2));
	gp14.con = 0x33; gp14.ext = 0x3;			/* run w1: CON 0x33, DAT 0x3 */
	gh2.con = 0; gh2.ext = 0x2;				/* PERST input low, CLKREQ high */
	iso1 = 0; iso0 = 0;
	pll_after = 3; cdr_after = 2; oc_after = 4; pll_cnt = cdr_cnt = oc_cnt = l0_cnt = l0_polls = 0;
	l0_after = 3; never_l0 = fault_l0_read = 0; waited_at_en = 0;
	never_pll = unreadable_blk = iso_set_ret = iso_clear_ret = write_fault_n = ignore_pa_set = 0;
	ignore_pa = fault_read_pa = 0;
	freq = 24576000ull; tick = 1000; waited_us = 0; nsave = 0;
	nl = nev = nwrites = 0;
}

static int has(const char *s)
{
	for (unsigned i = 0; i < nl; i++)
		if (strstr(lines[i], s))
			return 1;
	return 0;
}

static int has_last_step_ok(void)	/* 2 = "LINK Snn ok" and the clean summary for this build's N, 1 = only the step line, 0 = neither */
{
	char b[64];

	snprintf(b, sizeof b, "LINK S%02d ok", WLAN_LINK_N);
	if (!has(b))
		return 0;
	snprintf(b, sizeof b, "LINK summary reached=S%02d stopped=none restore=ok", WLAN_LINK_N);
	return has(b) ? 2 : 1;
}

static void fail(int line, const char *e)
{
	fprintf(stderr, "FAIL line %d: %s\n", line, e);
	for (unsigned i = 0; i < nl; i++)
		fprintf(stderr, "  %s\n", lines[i]);
	assert(0);
}
#define EXPECT(c) do { if (!(c)) fail(__LINE__, #c); } while (0)

/* every write of a run is a row of the table (steps <= N, or teardown) with exactly its register; nothing else, ever */
static void writes_are_table_rows(void)
{
	for (unsigned i = 0; i < nev; i++) {
		unsigned k, ok = 0;

		if (ev[i].t != EV_W)
			continue;
		for (k = 0; k < WLAN_SEQ_NROWS + WLAN_SEQ_NTEAR && !ok; k++) {
			struct wlan_row r;

			if (!get_row(k < WLAN_SEQ_NROWS ? k : k - WLAN_SEQ_NROWS, k >= WLAN_SEQ_NROWS, &r))
				continue;
			if ((r.kind == WLK_GPIO_DAT || r.kind == WLK_GPIO_CON || r.kind == WLK_MMIO_RMW || r.kind == WLK_MMIO_WRITE) && pa_of(r.blk, r.off) == ev[i].pa &&
			    (k >= WLAN_SEQ_NROWS || r.step <= WLAN_LINK_N))
				ok = 1;
		}
		EXPECT(ok);
	}
}

static int wrote(uintptr_t pa, uint32_t mask, uint32_t val)
{
	for (unsigned i = 0; i < nev; i++)
		if (ev[i].t == EV_W && ev[i].pa == pa && (ev[i].v & mask) == val)
			return 1;
	return 0;
}

static unsigned last_ev(int t, uintptr_t lo, uintptr_t hi)
{
	unsigned r = 0;

	for (unsigned i = 0; i < nev; i++)
		if (ev[i].t == t && (t != EV_W || (ev[i].pa >= lo && ev[i].pa < hi)))
			r = i + 1;
	return r;
}

static void baseline_restored(void)
{
	EXPECT(gp14.con == 0x33 && (dat_read(&gp14) & 4) == 0 && gh2.con == 0 && (dat_read(&gh2) & 1) == 0 && iso1 == 0);
}

static unsigned reached_step(void)	/* from the summary line: "LINK summary reached=Snn ..." */
{
	for (unsigned i = 0; i < nl; i++) {
		const char *p = strstr(lines[i], "LINK summary reached=S");

		if (p)
			return (unsigned)atoi(p + strlen("LINK summary reached=S"));
	}
	return 99;
}

/* PERST is never high at the end of a run: the last write to the GPH2 DAT clears bit 0 (when one raised it), the pin latch is low and the pin is an input again */
static void perst_low_at_end(void)
{
	unsigned last = 0, raised = 0;

	for (unsigned i = 0; i < nev; i++)
		if (ev[i].t == EV_W && ev[i].pa == WLAN_GPH2 + 4) {
			last = i + 1;
			raised |= ev[i].v & 1u;
		}
	EXPECT(!raised || (ev[last - 1].v & 1u) == 0u);
	EXPECT((gh2.latch & 1u) == 0 && (dat_read(&gh2) & 1u) == 0 && gh2.con == 0);
}

/* the teardown of a run that reached step hi (>= 15: every probed block has teardown rows) is, in order, exactly the write-like teardown rows with step <= hi: the tail of the event log
 * equals the table order (T01 PERST low, T02 LTSSM disable [only if S23 started], T03 WLAN_EN low, soft reset, PHY power-down, isolation clear, then the pins back to inputs) */
static void teardown_tail_is_table_order(unsigned hi)
{
	int et[64];
	uintptr_t epa[64];
	unsigned n = 0, first_perst_low = 0;
	struct wlan_row r;

	for (unsigned i = 0; i < WLAN_SEQ_NTEAR; i++) {
		if (!get_row(i, 1, &r) || r.step > hi)
			continue;
		if (r.kind == WLK_SMC_ISO) {
			EXPECT(n < 64);
			et[n] = EV_ISO_CLR; epa[n++] = 0;
		} else if (r.kind == WLK_GPIO_DAT || r.kind == WLK_GPIO_CON || r.kind == WLK_MMIO_RMW || r.kind == WLK_MMIO_WRITE) {
			EXPECT(n < 64);
			et[n] = EV_W; epa[n++] = pa_of(r.blk, r.off);
		}
	}
	EXPECT(n > 8 && nev >= n);
	for (unsigned j = 0; j < n; j++) {
		EXPECT(ev[nev - n + j].t == et[j]);
		EXPECT(et[j] != EV_W || ev[nev - n + j].pa == epa[j]);
	}
	if (hi >= 20) {		/* T01: PERST low first; T02: LTSSM disable second if the S23 step started; then WLAN_EN low */
		first_perst_low = nev - n;
		EXPECT(ev[first_perst_low].pa == WLAN_GPH2 + 4 && (ev[first_perst_low].v & 1u) == 0);
		if (hi >= 23)
			EXPECT(ev[first_perst_low + 1].pa == WLAN_ELBI + 0x54 && ev[first_perst_low + 1].v == 0 && ev[first_perst_low + 2].pa == WLAN_GPP14 + 4);
		else
			EXPECT(ev[first_perst_low + 1].pa == WLAN_GPP14 + 4);
	}
	EXPECT(last_ev(EV_ISO_CLR, 0, 0) > last_ev(EV_W, WLAN_PMA, WLAN_PMA + 0x2000));	/* the isolation clear after the last PHY write */
	perst_low_at_end();
}

int main(void)
{
	struct wlan_row r;

	EXPECT(WLAN_SEQ_LAST == 25 && WLAN_SEQ_DEFAULT_N == 19 && WLAN_SEQ_NROWS > 200 && WLAN_LINK_N <= WLAN_SEQ_LAST);
	/* the independent allowlist refuses every other register (a corrupted table / index cannot reach them) */
	EXPECT(!w32_allowed(WLAN_GPP14 + 4, 1u << 3, 0, 0) && !w32_allowed(WLAN_GPP14 + 4, 1u << 2, 0, 1u << 3) && w32_allowed(WLAN_GPP14 + 4, 1u << 2, 0, 1u << 2));
	EXPECT(!w32_allowed(WLAN_GPH2 + 4, 1u << 2, 0, 0) && w32_allowed(WLAN_GPH2 + 4, 3u, 0, 1) && !w32_allowed(WLAN_GPH2, 0xf00u, 0, 0) && !w32_allowed(WLAN_GPP14 + 0x18, 1, 0, 0));
	EXPECT(!w32_allowed(WLAN_GPP14 + 0, 0, 0, 0));		/* an empty mask is not a write */
	EXPECT(!w32_allowed(WLAN_PMA + 0x2000, FULL_MASK_T, 0, 0) && !w32_allowed(WLAN_PMA + 0x1ffe, FULL_MASK_T, 0, 0) && w32_allowed(WLAN_PMA + 0x1ffc, FULL_MASK_T, 0, 0));
	EXPECT(!w32_allowed(WLAN_ELBI + 0x500, FULL_MASK_T, 0, 0) && w32_allowed(WLAN_ELBI + 0x54, FULL_MASK_T, 0, 0) && !w32_allowed(WLAN_ELBI + 0x2c8, FULL_MASK_T, 0, 0));
	EXPECT(!w32_allowed(WLAN_DBI + 0x8bc, FULL_MASK_T, 0, 0) && w32_allowed(WLAN_DBI + 0x890, FULL_MASK_T, 0, 0) && !w32_allowed(WLAN_IA + 0x180, FULL_MASK_T, 0, 0));
	EXPECT(!w32_allowed(WLAN_PMU_ALIVE + 0x3EC4, FULL_MASK_T, 0, 0) && !w32_allowed(0x17460810UL, FULL_MASK_T, 0, 0) && !w32_allowed(WLAN_PCS + 0x1000, FULL_MASK_T, 0, 0));
	world();
	EXPECT(w32(WLAN_SEQ_NROWS, 0) == -2 && w32(WLAN_SEQ_NTEAR, 1) == -2 && w32(100000u, 0) == -2 && nwrites == 0);	/* index overflow */
	for (unsigned i = 0; i < WLAN_SEQ_NROWS; i++)		/* a row beyond N is refused by the writer itself */
		if (get_row(i, 0, &r) && r.step > WLAN_LINK_N && (r.kind == WLK_MMIO_WRITE || r.kind == WLK_GPIO_DAT))
			EXPECT(w32(i, 0) == -2);
	EXPECT(nwrites == 0);

	/* a table row of a non-write kind is never written */
	for (unsigned i = 0; i < WLAN_SEQ_NROWS; i++)
		if (get_row(i, 0, &r) && (r.kind == WLK_DELAY || r.kind == WLK_READ_ONLY || r.kind == WLK_WAIT_EQ || r.kind == WLK_SMC_ISO))
			EXPECT(w32(i, 0) == -2);

	/* ---- the happy path ---- */
	world();
	EXPECT(wlan_link_run() == 0);
	writes_are_table_rows();
	EXPECT(has("LINK table sha=") && has("LINK S00 ok") && has_last_step_ok() == 2 && !has("!!!"));
	EXPECT(ev[0].t == EV_W && ev[0].pa == WLAN_GPH2 + 4 && ev[1].pa == WLAN_GPH2 && ev[2].pa == WLAN_GPP14 + 4 && ev[3].pa == WLAN_GPP14);	/* S01 DAT, CON; S02 DAT, CON */
	EXPECT(waited_us >= 200000u + 1000u);
	EXPECT(nsave >= 2u * (WLAN_LINK_N + 1u));		/* a pstore checkpoint before and after every step */
	{
		unsigned iso_set = 0;

		for (unsigned i = 0; i < nev; i++)
			if (ev[i].t == EV_ISO_SET)
				iso_set = i + 1;
		EXPECT(iso_set && last_ev(EV_ISO_CLR, 0, 0) > last_ev(EV_W, WLAN_PMA, WLAN_PMA + 0x2000));		/* the isolation clear comes after the last PHY write */
		EXPECT(last_ev(EV_ISO_CLR, 0, 0) < last_ev(EV_W, WLAN_GPP14, WLAN_GPP14 + 1));				/* ... and before the pins go back to inputs */
	}
#if WLAN_LINK_N >= 8
	EXPECT(wrote(WLAN_PMA + 0xD8, 0xffffffff, 0x28) && wrote(WLAN_PMA + 0x400, 0x80, 0));
#endif
#if WLAN_LINK_N >= 19
	EXPECT(wrote(WLAN_DBI + 0x890, 0xffffffff, 0x12000));
#endif
	baseline_restored();
#if WLAN_LINK_N < 24
	EXPECT(!has("LINK R"));					/* the log flag exists only on the S24 / S25 rows */
#endif
	if (getenv("WLAN_DUMP")) {
		for (unsigned i = 0; i < nl; i++)
			printf("%s\n", lines[i]);
		printf("writes=%u ev=%u waited_us=%llu\n", nwrites, nev, (unsigned long long)waited_us);
	}
#if WLAN_LINK_N < 20
	EXPECT(!wrote(WLAN_GPH2 + 4, 1, 1));			/* the default run NEVER raises PERST */
	EXPECT(!wrote(WLAN_ELBI + 0x54, 0xffffffff, 1) && !wrote(WLAN_ELBI + 0x3bc, 0xffffffff, 0x11));	/* nor enables the LTSSM / the options */
	EXPECT(ev[nev - 1].pa == WLAN_GPH2 && ev[nev - 1].t == EV_W);
#else
	EXPECT(wrote(WLAN_GPH2 + 4, 1, 1));			/* S20: PERST high ... */
	{
		unsigned hi = 0, lo = 0;

		for (unsigned i = 0; i < nev; i++)
			if (ev[i].t == EV_W && ev[i].pa == WLAN_GPH2 + 4) {
				if (ev[i].v & 1u && !hi) hi = i + 1;
				if (hi && !(ev[i].v & 1u) && !lo) lo = i + 1;
			}
		EXPECT(hi && lo > hi);				/* ... and PERST low again, first thing of the teardown (T01) */
		EXPECT(ev[lo].t == EV_W || ev[lo].t == EV_ISO_CLR || 1);
	}
#endif
#if WLAN_LINK_N >= 21
	EXPECT(wrote(WLAN_ELBI + 0x3bc, 0xffffffff, 0x11) && wrote(WLAN_ELBI + 0x3a0, 0xffffffff, 2));
#endif
#if WLAN_LINK_N >= 21 && WLAN_LINK_N < 23
	EXPECT(!wrote(WLAN_ELBI + 0x54, 0xffffffff, 1));		/* the LTSSM stays disabled before S23 */
#endif
#if WLAN_LINK_N >= 22
	{	/* S22 is the documented skip: exactly one row, a 0 us delay, so the step runs ("ok rows=1") and writes nothing */
		unsigned n22 = 0;

		for (unsigned i = 0; i < WLAN_SEQ_NROWS; i++)
			if (get_row(i, 0, &r) && r.step == 22) {
				n22++;
				EXPECT(r.kind == WLK_DELAY && r.a == 0);
			}
		EXPECT(n22 == 1 && has("LINK S22 start") && has("LINK S22 ok rows=1"));
	}
#endif
#if WLAN_LINK_N >= 23
	/* S23: LTSSM enable = ELBI 0x54 := 1, then the L0 wait polls ELBI 0x2C8 until (v & 0x3f) == 0x11 (fake: the 3rd poll), then the teardown disables the LTSSM */
	EXPECT(wrote(WLAN_ELBI + 0x54, 0xffffffff, 1) && l0_polls == 3u + L0X && has("LINK S23 ok rows=2") && wrote(WLAN_ELBI + 0x54, 0xffffffff, 0) && !has("!!!"));
	EXPECT(last_ev(EV_W, WLAN_ELBI + 0x54, WLAN_ELBI + 0x55) > 0 && ev[last_ev(EV_W, WLAN_ELBI + 0x54, WLAN_ELBI + 0x55) - 1].v == 0);	/* the LTSSM ends disabled */
	teardown_tail_is_table_order(23);
#endif
#if WLAN_LINK_N >= 24
	{	/* S24: 13 read-only log rows (DBI/ELBI, a == 2 = log, never a probe), no write; every one prints `LINK R24 row=<i> rd=0x<value>` with the value the fake holds */
		unsigned n24 = 0, nR = 0;
		uint32_t want[32];
		unsigned wid[32];
		char b[64];

		world();
		dbi[0x000 / 4] = 0xA5A5144Du; dbi[0x034 / 4] = 0x40u; dbi[0x070 / 4] = 0x00000010u; dbi[0x07C / 4] = 0x0000D123u; dbi[0x080 / 4] = 0x00220040u; dbi[0x0A0 / 4] = 0x00020002u;
		dbi[0x710 / 4] = 0x00010120u; dbi[0x80C / 4] = 0x00010000u | 0x0Cu; elbi[0x2C8 / 4] = 0x0300AA00u;
		for (unsigned i = 0; i < WLAN_SEQ_NROWS; i++)
			if (get_row(i, 0, &r) && r.step == 24) {
				EXPECT(r.kind == WLK_READ_ONLY && r.a == 2u && (r.blk == WLB_DBI || r.blk == WLB_ELBI) && n24 < 32);
				wid[n24++] = i;
			}
		EXPECT(n24 == 13);
		for (unsigned j = 0; j < n24; j++)
			(void)get_row(wid[j], 0, &r), want[j] = r.blk != WLB_DBI ? 0x0300AA11u : r.off == 0x890u ? 0x12000u : dbi[r.off / 4];	/* ELBI 0x2C8: L0 once the LTSSM is enabled and the fake counted 3 polls */
		EXPECT(wlan_link_run() == 0 && has("LINK S24 ok rows=13") && !has("!!!"));
		for (unsigned j = 0; j < n24; j++) {
			snprintf(b, sizeof b, "LINK R24 row=%u rd=0x%08X", wid[j], (unsigned)want[j]);
			EXPECT(has(b));
		}
		for (unsigned i = 0; i < nl; i++)
			if (strstr(lines[i], "LINK R24 "))
				nR++;
		EXPECT(nR == n24 && !has("LINK R23") && !has("LINK R19"));
		teardown_tail_is_table_order(24);
		baseline_restored();
		world(); fault_read_pa = WLAN_DBI + 0x70;		/* an unreadable capability word stops S24 (fault at that row), teardown unchanged, restore ok */
		EXPECT(wlan_link_run() == 1 && has("LINK S24 fault row=244") && has("stopped=fault at=S24 row=244 restore=ok") && !has("!!!") && has("LINK R24 row=243"));
		teardown_tail_is_table_order(24);
		baseline_restored();
	}
#endif
#if WLAN_LINK_N >= 25
	{	/* S25: READ-ONLY iATU layout: 6 DBI words, the ATU block probe (a == 1, not logged), ATU 0x000..0x018, ELBI 0x2C8; 14 `LINK R25` lines; the ATU window is never written */
		unsigned n25 = 0, nR = 0, probe_row = 0, nprobe = 0, natu = 0;
		uint32_t want;
		unsigned wid[32];
		char b[64];

		world();
		dbi[0x018 / 4] = 0x00010100u; dbi[0x03C / 4] = 0x000001FFu; dbi[0x8BC / 4] = 1u; dbi[0x8F8 / 4] = 0x3532302Au; dbi[0x8FC / 4] = 0x45703032u; dbi[0x900 / 4] = 0xFFFFFFFFu;
		atu[0x00 / 4] = 0x4u; atu[0x04 / 4] = 0x80000000u; atu[0x08 / 4] = 0x60FFE000u; atu[0x10 / 4] = 0x60FFFFFFu; atu[0x14 / 4] = 0x01000000u; elbi[0x2C8 / 4] = 0x0300AA00u;
		for (unsigned i = 0; i < WLAN_SEQ_NROWS; i++)
			if (get_row(i, 0, &r) && r.step == 25) {
				EXPECT(r.kind == WLK_READ_ONLY && (r.blk == WLB_DBI || r.blk == WLB_ATU || r.blk == WLB_ELBI) && n25 < 32);
				EXPECT(r.a == (r.blk == WLB_ATU && r.off == 0u && !nprobe ? 1u : 2u));	/* the first ATU 0x000 row is the probe, every other one logs */
				if (r.a == 1u)
					nprobe++, probe_row = i;
				else
					wid[n25++] = i;
				natu += r.blk == WLB_ATU;
			}
		EXPECT(n25 == 14 && nprobe == 1 && natu == 8);
		EXPECT(wlan_link_run() == 0 && has("LINK S25 ok rows=15") && !has("!!!"));
		for (unsigned j = 0; j < n25; j++) {
			(void)get_row(wid[j], 0, &r);
			want = r.blk == WLB_ELBI ? 0x0300AA11u : r.blk == WLB_DBI ? dbi[r.off / 4] : atu[r.off / 4];
			snprintf(b, sizeof b, "LINK R25 row=%u rd=0x%08X", wid[j], (unsigned)want);
			EXPECT(has(b));
		}
		for (unsigned i = 0; i < nl; i++)
			if (strstr(lines[i], "LINK R25 "))
				nR++;
		EXPECT(nR == n25 && has("LINK R24 row=") && last_ev(EV_W, WLAN_ATU, WLAN_ATU + 0x1000) == 0);	/* S24 still logs its own rows; nothing wrote the ATU window */
		teardown_tail_is_table_order(25);
		baseline_restored();
		world(); unreadable_blk = 5;			/* the ATU probe fails: stop at that row, no ATU row ran, no R25 line from the ATU/ELBI rows, teardown unchanged, restore ok */
		snprintf(b, sizeof b, "LINK S25 fault row=%u", probe_row);
		EXPECT(wlan_link_run() == 1 && has(b) && has("stopped=fault at=S25") && has("restore=ok") && !has("!!!"));
		nR = 0;
		for (unsigned i = 0; i < nl; i++)
			nR += strstr(lines[i], "LINK R25 ") != NULL;
		EXPECT(nR == 6);				/* only the 6 DBI rows before the probe */
		teardown_tail_is_table_order(25);
		baseline_restored();
	}
#endif

	/* ---- failure branches: each stops, tears down, is loud where it must be ---- */
	world(); fault_read_pa = 0;
	gp14.ext |= 4;						/* WLAN_EN reads high: the precondition refuses, nothing written */
	EXPECT(wlan_link_run() == 1 && nwrites == 0 && has("LINK S00 refused row=0") && has("LINK summary reached=S00 stopped=refused at=S00 row=0 restore=ok"));
	world(); gh2.ext |= 1;
	EXPECT(wlan_link_run() == 1 && nwrites == 0 && has("LINK S00 refused"));
	world(); iso1 = 1;
	EXPECT(wlan_link_run() == 1 && nwrites == 0 && nev == 0 && has("LINK S00 refused"));
	world(); gp14.con = 0x133;
	EXPECT(wlan_link_run() == 1 && nwrites == 0 && has("LINK S00 refused"));

#if WLAN_LINK_N >= 8
	world(); unreadable_blk = 1;				/* the PMA probe fails: no PMA write, teardown touches no PMA */
	EXPECT(wlan_link_run() == 1 && has("LINK S08 fault row=23") && has("stopped=fault at=S08 row=23 restore=ok"));
	writes_are_table_rows();
	EXPECT(last_ev(EV_W, WLAN_PMA, WLAN_PMA + 0x2000) == 0 && last_ev(EV_ISO_CLR, 0, 0) && !wrote(WLAN_PMA, 0, 0));
	baseline_restored();

#endif
#if WLAN_LINK_N >= 15
	world(); never_pll = 1;					/* the PLL never locks: S15 timeout at its row, soft reset + PHY power-down + isolation clear LAST */
	EXPECT(wlan_link_run() == 1 && has("LINK S15 timeout row=214") && has("stopped=timeout at=S15 row=214 restore=ok"));
	EXPECT(wrote(WLAN_PMA + 0x400, 0x80, 0) && wrote(WLAN_ELBI + 0x3a4, 2, 0) && last_ev(EV_ISO_CLR, 0, 0) > last_ev(EV_W, WLAN_PMA, WLAN_PMA + 0x2000));
	EXPECT(!wrote(WLAN_ELBI + 0x80, 0xffffffff, 4));	/* S17 never ran */
	baseline_restored();

#endif
#if WLAN_LINK_N >= 14
	world(); unreadable_blk = 2;				/* the I/A probe fails (S14) */
	EXPECT(wlan_link_run() == 1 && has("LINK S14 fault") && wrote(WLAN_PMA + 0x400, 0x80, 0) && last_ev(EV_W, WLAN_IA, WLAN_IA + 0x1000) == 0);
	baseline_restored();
#endif
#if WLAN_LINK_N >= 19
	world(); unreadable_blk = 4;				/* the DBI probe fails (S19): no DBI write */
	EXPECT(wlan_link_run() == 1 && has("LINK S19 fault") && last_ev(EV_W, WLAN_DBI, WLAN_DBI + 0x1000) == 0);
	baseline_restored();

#endif
#if WLAN_LINK_N >= 19
	world(); write_fault_n = 40;				/* a write faults in the middle of the PHY tables */
	EXPECT(wlan_link_run() == 1 && has("fault row=") && has("restore=ok"));
	baseline_restored();

#endif
	world(); iso_set_ret = 2;				/* the isolation release fails (fake EL3 mismatch): stop at S05, the clear still runs */
	EXPECT(wlan_link_run() == 1 && has("LINK S05 fault") && last_ev(EV_ISO_CLR, 0, 0) && last_ev(EV_W, WLAN_PMA, WLAN_PMA + 0x2000) == 0);
	baseline_restored();
	world(); iso_set_ret = 1;				/* refused before any RMW */
	EXPECT(wlan_link_run() == 1 && has("LINK S05 refused"));
	baseline_restored();

	world(); ignore_pa_set = 1; ignore_pa = WLAN_GPP14 + 4;	/* WLAN_EN cannot be driven: S02/S03 read-back mismatch -> fault, teardown loud */
	EXPECT(wlan_link_run() == 1 && has("LINK S03 fault"));

	world(); iso_clear_ret = 2;				/* the isolation clear fails: LOUD */
	EXPECT(wlan_link_run() == 1 && has("restore=FAILED") && has("!!! RESTORE FAILED") && has("LINK T"));
#if WLAN_LINK_N >= 8
	world(); ignore_pa_set = 1; ignore_pa = WLAN_PMA + 0x400;	/* the PHY power-down RMW does not take: writes are not read back, but the run is still ok */
	EXPECT(wlan_link_run() == 0);
#endif
	world(); write_fault_n = 1;				/* the very first write faults */
	EXPECT(wlan_link_run() == 1 && has("LINK S01 fault"));
	world(); freq = 0;
	EXPECT(wlan_link_run() == 2 && nwrites == 0 && has("no timer"));
#if WLAN_LINK_N >= 19
	/* fault injection at EVERY forward write row (S00..N): the k-th try_write32 fails; the run stops there with `fault`, the teardown runs in table order, restore=ok, PERST ends low */
	{
		unsigned fwd = 0, ids[400], steps[400];

		for (unsigned i = 0; i < WLAN_SEQ_NROWS; i++)
			if (get_row(i, 0, &r) && r.step <= WLAN_LINK_N && (r.kind == WLK_GPIO_DAT || r.kind == WLK_GPIO_CON || r.kind == WLK_MMIO_RMW || r.kind == WLK_MMIO_WRITE)) {
				EXPECT(fwd < 400);
				ids[fwd] = i; steps[fwd++] = r.step;
			}
		EXPECT(fwd > 200);
		for (unsigned k = 1; k <= fwd; k++) {
			char b[96];

			world(); write_fault_n = (int)k;
			EXPECT(wlan_link_run() == 1);
			snprintf(b, sizeof b, "stopped=fault at=S%02u row=%u restore=ok", steps[k - 1], ids[k - 1]);
			EXPECT(has(b) && !has("!!!") && reached_step() == steps[k - 1]);
			writes_are_table_rows();
			if (steps[k - 1] >= 15)
				teardown_tail_is_table_order(steps[k - 1]);
			baseline_restored();
			perst_low_at_end();
		}
	}
#endif
#if WLAN_LINK_N >= 20
	world(); ignore_pa_set = 1; ignore_pa = WLAN_GPH2 + 4;		/* PERST cannot be driven high: the S20 read-back mismatches -> fault at S20, teardown in order */
	EXPECT(wlan_link_run() == 1 && has("LINK S20 fault") && reached_step() == 20 && has("restore=ok"));
	teardown_tail_is_table_order(20);
	baseline_restored();
#endif
#if WLAN_LINK_N >= 23
	world(); never_l0 = 1;						/* the link never comes up: bounded wait -> `timeout` at S23, LTSSM disabled, PERST low, restore=ok, no hang */
	EXPECT(wlan_link_run() == 1 && has("LINK S23 timeout row=238") && has("stopped=timeout at=S23 row=238 restore=ok") && !has("!!!"));
	/* MAX_TIMEOUT (common.h:20) = 4800 polls is the cap; poll_row's tick guard (counter_freq / 1e6 truncates 24.576 MHz to 24 ticks/us) ends the wait ~2% earlier, never later */
	EXPECT(l0_polls <= 4800u && l0_polls >= 4700u);
	EXPECT(waited_us - waited_at_en >= 4800u * 10u && waited_us - waited_at_en <= 4800u * 13u);	/* ~MAX_TIMEOUT * usleep_range(10, 12): 48..58 ms, not the old guess of seconds */
	teardown_tail_is_table_order(23);
	baseline_restored();
	world(); fault_l0_read = 1;					/* the ELBI becomes unreadable while polling: fault at S23, same teardown */
	EXPECT(wlan_link_run() == 1 && has("LINK S23 fault row=238") && has("restore=ok") && l0_polls == 1u);
	teardown_tail_is_table_order(23);
	baseline_restored();
	world(); l0_after = 4000;					/* a late L0 (poll 4000, ~48 ms) is still ok; one that never comes is a timeout */
	EXPECT(wlan_link_run() == 0 && l0_polls == 4000u + L0X);
	world(); l0_after = 4801;
	EXPECT(wlan_link_run() == 1 && has("stopped=timeout at=S23"));
	world(); freq = 1000000;					/* a slow counter does not turn the wait into a hang: still <= 4800 polls */
	EXPECT(wlan_link_run() == 0 || l0_polls <= 4800u);
#endif
	puts("PASS wlanlink host test N=" WLAN_LINK_N_STR);
	return 0;
}
