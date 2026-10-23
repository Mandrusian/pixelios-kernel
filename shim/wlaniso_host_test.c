/* Host test for the W2c writer of wlansmc.c (--wlan-iso, docs/research/29) against a FAKE EL3 that implements a selectable RMW semantics on the PMU isolation word:
 * EL_RMW (the guess: new = (old & ~mask) | (val & mask)), EL_OVERWRITE (new = val), EL_IGNORE (accepted, nothing changes), EL_REFUSE (-EINVAL, nothing changes), EL_LIE (returns -EINVAL but applies),
 * EL_LATE (applies only after N reads). Checks the exact SMC sequence (READ, ONE set, at most ONE clear; x0 = 0x82000504, x1 = 0x17463EC4, x2 = 2, x3 = 1, x4 = 1 / 0), the summaries, that every
 * precondition refuses with NO RMW, that the read-back guard catches changed other bits and contradicting returns, that a second set is never sent, and that a failed restore is loud.
 * Uses the REAL generated board macros. Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -DWLAN_SMC_READ -DWLAN_ISO_RUN -I. -Iout -include out/board_config.h -o out/wlaniso_host_test wlaniso_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wlansmc.c"

#if !(SOC_GS101 && defined(WLAN_PMU_PCIE1_ISO) && defined(WLAN_ISO_RUN))
#error "needs the bluejay board_config.h and -DWLAN_ISO_RUN"
#endif

enum { EL_RMW, EL_OVERWRITE, EL_IGNORE, EL_REFUSE, EL_LIE, EL_LATE };
static struct { uint64_t x[5]; } calls[16];
static unsigned ncalls, nrmw, nset, nclr, nsmc_read, sem, late_after, late_reads;
static uint32_t iso1, iso0, gph2_dat, gpp14_dat;
static int iso1_fault, iso1_fault_after_set, set_done, clear_ignored, read_ret_neg, timer_ok, read_status_zero, late_survives_clear;
static uint64_t fake_freq, waits, last_wait;
static char lines[64][RAMLOG_LINE_MAX + 8];
static unsigned nl;
static int pending_set;

uint64_t counter_freq(void) { return timer_ok ? fake_freq : 0; }
void wait_ticks(uint64_t t) { last_wait = t; waits++; }

void con_str(const char *l, const char *s)
{
	assert(nl < 64 && strlen(s) < RAMLOG_LINE_MAX);
	snprintf(lines[nl++], sizeof(lines[0]), "%s %s", l, s);
}

int try_read32(uintptr_t pa, uint32_t *v)
{
	if (pa == WLAN_PMU_PCIE1_ISO) {
		if (iso1_fault || (iso1_fault_after_set && set_done))
			return 0;
		if (pending_set && ++late_reads > late_after) {
			iso1 |= 1;
			pending_set = 0;
		}
		*v = iso1;
		return 1;
	}
	if (pa == WLAN_PMU_PCIE0_ISO) { *v = iso0; return 1; }
	if (pa == WLAN_GPH2 + 4) { *v = gph2_dat; return 1; }
	if (pa == WLAN_GPP14 + 4) { *v = gpp14_dat; return 1; }
	assert(0 && "unexpected MMIO read");
	return 0;
}

void wlan_host_smc(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, struct smc_res *r)
{
	assert(ncalls < 16);
	calls[ncalls].x[0] = x0; calls[ncalls].x[1] = x1; calls[ncalls].x[2] = x2; calls[ncalls].x[3] = x3; calls[ncalls].x[4] = 0;
	ncalls++;
	nsmc_read++;
	assert(x0 == 0x82000504UL && x2 == 0);
	r->a0 = read_ret_neg ? (uint64_t)(int64_t)-22 : read_status_zero ? 0 : iso1;	/* the guess: a READ returns the value in a0 (get_priv_reg, exynos-pd_el3.c:53-63) */
	r->a1 = r->a2 = r->a3 = 0;
}

void wlan_host_smc5(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, struct smc_res *r)
{
	uint32_t old = iso1;

	assert(ncalls < 16);
	calls[ncalls].x[0] = x0; calls[ncalls].x[1] = x1; calls[ncalls].x[2] = x2; calls[ncalls].x[3] = x3; calls[ncalls].x[4] = x4;
	ncalls++;
	nrmw++;
	if (x4 == 1)
		nset++;
	else
		nclr++;
	r->a0 = r->a1 = r->a2 = r->a3 = 0;
	if (x4 == 1)
		set_done = 1;
	if (x4 == 0 && clear_ignored)
		return;
	switch (sem) {
	case EL_RMW:
		iso1 = (old & ~(uint32_t)x3) | ((uint32_t)x4 & (uint32_t)x3);
		break;
	case EL_OVERWRITE:
		iso1 = (uint32_t)x4;
		break;
	case EL_IGNORE:
		break;
	case EL_REFUSE:
		r->a0 = (uint64_t)(int64_t)-22;
		break;
	case EL_LIE:
		iso1 = (old & ~(uint32_t)x3) | ((uint32_t)x4 & (uint32_t)x3);
		r->a0 = (uint64_t)(int64_t)-22;
		break;
	case EL_LATE:
		if (x4 == 1) {
			pending_set = 1;
			late_reads = 0;
		} else {
			iso1 &= ~1u;
			if (!late_survives_clear)
				pending_set = 0;
		}
		break;
	}
}

static void world(void)
{
	memset(calls, 0, sizeof(calls));
	ncalls = nrmw = nset = nclr = nsmc_read = nl = 0;
	sem = EL_RMW;
	late_after = 0;
	late_reads = 0;
	iso1 = 0xA0;		/* bit 0 clear; other bits set so that an overwrite is visible (the observed word is 0) */
	iso0 = 0x10;
	gph2_dat = 0x2;		/* PERST low, CLKREQ high */
	gpp14_dat = 0x3;	/* WLAN_EN low */
	iso1_fault = iso1_fault_after_set = set_done = clear_ignored = read_ret_neg = read_status_zero = late_survives_clear = 0;
	timer_ok = 1;
	fake_freq = 24576000ull;
	waits = last_wait = 0;
	pending_set = 0;
}

static int has(const char *s)
{
	for (unsigned i = 0; i < nl; i++)
		if (strstr(lines[i], s))
			return 1;
	return 0;
}

static void fail(int line, const char *e)
{
	fprintf(stderr, "FAIL line %d: %s\n", line, e);
	for (unsigned i = 0; i < nl; i++)
		fprintf(stderr, "  %s\n", lines[i]);
	for (unsigned i = 0; i < ncalls; i++)
		fprintf(stderr, "  smc %llx %llx %llx %llx %llx\n", (unsigned long long)calls[i].x[0], (unsigned long long)calls[i].x[1], (unsigned long long)calls[i].x[2], (unsigned long long)calls[i].x[3], (unsigned long long)calls[i].x[4]);
	assert(0);
}
#define EXPECT(c) do { if (!(c)) fail(__LINE__, #c); } while (0)

static void call_is(unsigned i, uint64_t x2, uint64_t x3, uint64_t x4)
{
	EXPECT(calls[i].x[0] == 0x82000504UL && calls[i].x[1] == 0x17463EC4UL && calls[i].x[2] == x2 && calls[i].x[3] == x3 && calls[i].x[4] == x4);
}

int main(void)
{
	struct smc_res r;

	EXPECT(PRIV_REG_OPTION_RMW == 2 && ISO_MASK == 1 && ISO_REG == 0x17463EC4UL && ISO_POLLS == 10);

	/* the guess (RMW): READ, one set, one clear, one final look; the register ends as it began */
	world();
	EXPECT(wlan_iso_run() == 0);
	EXPECT(ncalls == 3 && nset == 1 && nclr == 1 && nrmw == 2);
	call_is(0, 0, 0, 0); call_is(1, 2, 1, 1); call_is(2, 2, 1, 0);
	EXPECT(has("WLAN ISO before ISO1=000000A0 ISO0=00000010") && has("WLAN ISO after set ISO1=000000A1 ISO0=00000010") && has("WLAN ISO after clear ISO1=000000A0"));
	EXPECT(has("WLAN ISO final ISO1=000000A0") && last_wait == fake_freq / 100u);
	EXPECT(has("WLAN ISO release=ok readback=0x000000A1 restore=ok") && !has("!!!") && iso1 == 0xA0);

	/* F3: the SMC READ must agree with the MMIO word (here: a0 is a status 0 while the word is 0xA0) -> nothing is set */
	world(); read_status_zero = 1;
	EXPECT(wlan_iso_run() == 2 && nrmw == 0 && ncalls == 1 && has("the SMC READ value differs from the MMIO word") && has("release=refused readback=n/a restore=n/a"));
	/* ... at the observed word 0 a status 0 and a value 0 agree (the documented ambiguity) */
	world(); read_status_zero = 1; iso1 = 0;
	EXPECT(wlan_iso_run() == 0 && nset == 1 && nclr == 1);

	/* full overwrite semantics: the other bits of the word change -> mismatch; the one clear follows; never a second set */
	world(); sem = EL_OVERWRITE;
	EXPECT(wlan_iso_run() == 1 && nset == 1 && nclr == 1);
	EXPECT(has("WLAN ISO release=mismatch readback=0x00000001 restore=ok"));

	/* ... which the observed all-zero word cannot show (documented limit): bit 0 behaves, nothing else to see */
	world(); sem = EL_OVERWRITE; iso1 = 0;
	EXPECT(wlan_iso_run() == 0 && has("release=ok readback=0x00000001 restore=ok"));

	/* F1: accepted (ret 0) but nothing changes: mismatch, and the clear is STILL sent (idempotent), never a second set */
	world(); sem = EL_IGNORE;
	EXPECT(wlan_iso_run() == 1 && nset == 1 && nclr == 1 && has("WLAN ISO release=mismatch readback=0x000000A0 restore=ok"));
	EXPECT(waits == ISO_POLLS - 1 + 1);	/* bounded: 10 reads / 9 waits of 100 us for the set, 0 for the clear, 1 final wait */

	/* EL3 refuses (-EINVAL), nothing changes: refused; the clear is sent too (and refused, bit 0 stays 0 -> restore ok) */
	world(); sem = EL_REFUSE;
	EXPECT(wlan_iso_run() == 1 && nset == 1 && nclr == 1 && has("release=refused readback=0x000000A0 restore=ok") && has("ret=FFFFFFEA"));

	/* EL3 says -EINVAL but applied it: contradiction -> mismatch, and the set bit is cleared */
	world(); sem = EL_LIE;
	EXPECT(wlan_iso_run() == 1 && nset == 1 && nclr == 1 && has("release=mismatch readback=0x000000A1 restore=ok") && iso1 == 0xA0);

	/* the bit shows up late but inside the bound -> ok; outside the bound -> mismatch and the clear (sent anyway) cancels the pending apply: the bit ends cleared (F1) */
	world(); sem = EL_LATE; late_after = 3;
	EXPECT(wlan_iso_run() == 0 && has("release=ok readback=0x000000A1 restore=ok"));
	world(); sem = EL_LATE; late_after = 30;
	EXPECT(wlan_iso_run() == 1 && nclr == 1 && has("release=mismatch readback=0x000000A0 restore=ok") && iso1 == 0xA0);
	/* an EL3 whose late apply survives the clear and lands at the final look: the final read shows it -> LOUD */
	world(); sem = EL_LATE; late_after = 11; late_survives_clear = 1;
	EXPECT(wlan_iso_run() == 1 && nclr == 1 && has("WLAN ISO final ISO1=000000A1") && has("restore=FAILED") && has("!!! RESTORE FAILED"));

	/* the clear is accepted but does not take: LOUD */
	world(); clear_ignored = 1;
	EXPECT(wlan_iso_run() == 1 && nset == 1 && nclr == 1 && has("release=ok readback=0x000000A1 restore=FAILED") && has("!!! RESTORE FAILED"));

	/* the register becomes unreadable after the set: fault, the clear is still sent */
	world(); iso1_fault_after_set = 1;
	EXPECT(wlan_iso_run() == 1 && nset == 1 && nclr == 1 && has("release=fault readback=n/a restore=FAILED"));

	/* every precondition refuses with NO RMW (the SMC READ may have been sent, never a set) */
	world(); iso1 |= 1;
	EXPECT(wlan_iso_run() == 2 && nrmw == 0 && has("ISO refused: PHY isolation already released") && has("release=refused readback=n/a restore=n/a"));
	world(); gph2_dat |= 1;
	EXPECT(wlan_iso_run() == 2 && nrmw == 0 && ncalls == 0 && has("PERST gph2-0 reads high"));
	world(); gpp14_dat |= 4;
	EXPECT(wlan_iso_run() == 2 && nrmw == 0 && ncalls == 0 && has("WLAN_EN gpp14-2 reads high"));
	world(); iso1_fault = 1;
	EXPECT(wlan_iso_run() == 2 && nrmw == 0 && ncalls == 0 && has("unreadable, nothing sent"));
	world(); timer_ok = 0;
	EXPECT(wlan_iso_run() == 2 && nrmw == 0 && ncalls == 0 && has("no timer"));
	world(); read_ret_neg = 1;
	EXPECT(wlan_iso_run() == 2 && nrmw == 0 && ncalls == 1 && has("SMC READ of the isolation word was refused"));

	/* the helpers carry constants only: the host fake sees exactly these registers */
	world();
	memset(&r, 0, sizeof(r));
	smc_priv_rmw_set(&r);
	smc_priv_rmw_clear(&r);
	call_is(0, 2, 1, 1); call_is(1, 2, 1, 0);
	EXPECT(ncalls == 2);

	puts("PASS wlaniso host test");
	return 0;
}
