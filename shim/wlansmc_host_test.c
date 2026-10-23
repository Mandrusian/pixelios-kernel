/* Host test for wlansmc.c (--wlan-smc-read, docs/research/29 W2a) against a FAKE EL3: records every SMC (x0..x3) and answers per scenario. Checks that exactly the two SMC_CMD_PRIV_REG
 * READs are issued (x0 = 0x82000504, x1 = 0x17463EC4 then 0x17463EC0, x2 = 0 = READ, x3 = 0), the printed lines, the same= verdicts (value / ambiguous zero / differs / EL3 refusal
 * -EINVAL / SMCCC unknown function -1 / MMIO fault), that a READ changing the register is reported, and that the helper refuses (zero SMC) any other address, including a WRITE/RMW-style
 * second call. Uses the REAL generated board macros. Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -DWLAN_SMC_READ -I. -Iout -include out/board_config.h -o out/wlansmc_host_test wlansmc_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wlansmc.c"

#if !(SOC_GS101 && defined(WLAN_PMU_PCIE1_ISO))
#error "needs the bluejay board_config.h"
#endif

static struct { uint64_t x0, x1, x2, x3; } calls[16];
static unsigned ncalls;
static uint64_t ret_a0[2], ret_a1, ret_a2, ret_a3;	/* per call index: a0 */
static uint32_t mm[2];					/* PMU_PCIE1_ISO, PMU_PCIE0_ISO */
static int mm_fault, change_on_smc;
static char lines[64][RAMLOG_LINE_MAX + 8];
static unsigned nl;

void con_str(const char *l, const char *s)
{
	assert(nl < 64 && strlen(s) < RAMLOG_LINE_MAX);
	snprintf(lines[nl++], sizeof(lines[0]), "%s %s", l, s);
}

int try_read32(uintptr_t pa, uint32_t *v)
{
	if (mm_fault)
		return 0;
	assert(pa == WLAN_PMU_PCIE1_ISO || pa == WLAN_PMU_PCIE0_ISO);	/* nothing else is read */
	*v = mm[pa == WLAN_PMU_PCIE0_ISO];
	return 1;
}

void wlan_host_smc(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, struct smc_res *r)
{
	assert(ncalls < 16);
	calls[ncalls].x0 = x0; calls[ncalls].x1 = x1; calls[ncalls].x2 = x2; calls[ncalls].x3 = x3;
	r->a0 = ret_a0[ncalls < 2 ? ncalls : 1];
	r->a1 = ret_a1; r->a2 = ret_a2; r->a3 = ret_a3;
	if (change_on_smc)
		mm[0] ^= 1;
	ncalls++;
}

static void world(void)
{
	memset(calls, 0, sizeof(calls));
	ncalls = nl = 0;
	ret_a0[0] = ret_a0[1] = 0;
	ret_a1 = ret_a2 = ret_a3 = 0;
	mm[0] = mm[1] = 0;
	mm_fault = change_on_smc = 0;
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
	assert(0);
}
#define EXPECT(c) do { if (!(c)) fail(__LINE__, #c); } while (0)

static void two_reads(void)
{
	EXPECT(ncalls == 2);
	EXPECT(calls[0].x0 == 0x82000504UL && calls[0].x1 == 0x17463EC4UL && calls[0].x2 == 0 && calls[0].x3 == 0);
	EXPECT(calls[1].x0 == 0x82000504UL && calls[1].x1 == 0x17463EC0UL && calls[1].x2 == 0 && calls[1].x3 == 0);
}

int main(void)
{
	struct smc_res r;

	EXPECT(SMC_FID_PRIV_REG == 0x82000504UL && PRIV_REG_OPTION_READ == 0 && WLAN_PMU_PCIE1_ISO == 0x17463EC4UL && WLAN_PMU_PCIE0_ISO == 0x17463EC0UL);

	/* a0 = the value (nonzero word): same=yes, not ambiguous */
	world(); mm[0] = 1; mm[1] = 0x10; ret_a0[0] = 1; ret_a0[1] = 0x10;
	EXPECT(wlan_smc_read() == 0);
	two_reads();
	EXPECT(has("WLAN SMC read pa=17463EC4 ret=00000001 a0=0000000000000001") && has("WLAN SMC read pa=17463EC0 ret=00000010 a0=0000000000000010"));
	EXPECT(has("WLAN SMC cmp pa=17463EC4 mmio=00000001 same=yes") && has("WLAN SMC summary same=yes ambiguous=0"));
	EXPECT(has("WLAN SMC mmio after pa=17463EC4 v=00000001") && !has("!!!"));

	/* observed state (both words 0) and a0 = 0: yes but ambiguous (a0 may be a status) */
	world();
	ret_a1 = 0x1111; ret_a2 = 0x2222; ret_a3 = 0x3333;
	EXPECT(wlan_smc_read() == 0 && has("WLAN SMC summary same=yes ambiguous=1") && has("a1=0000000000001111 a2=0000000000002222 a3=0000000000003333"));
	two_reads();

	/* a0 = 0 (status) while the word is nonzero: no */
	world(); mm[0] = 1;
	EXPECT(wlan_smc_read() == 1 && has("WLAN SMC cmp pa=17463EC4 mmio=00000001 same=no") && has("WLAN SMC summary same=no"));

	/* EL3 refuses (-EINVAL) / unknown function (-1): error */
	world(); ret_a0[0] = (uint64_t)(int64_t)-22; ret_a0[1] = 0;
	EXPECT(wlan_smc_read() == 2 && has("ret=FFFFFFEA") && has("same=error") && has("WLAN SMC summary same=error"));
	world(); ret_a0[0] = ret_a0[1] = (uint32_t)-1;
	EXPECT(wlan_smc_read() == 2 && has("ret=FFFFFFFF") && has("WLAN SMC summary same=error"));
	two_reads();

	/* the MMIO word is unreadable: error, the SMC still ran (read-only) */
	world(); mm_fault = 1;
	EXPECT(wlan_smc_read() == 2 && has("mmio unreadable same=error") && ncalls == 2);

	/* a READ must not change the register: if it does, say so loudly */
	world(); change_on_smc = 1; ret_a0[0] = 0;
	EXPECT(wlan_smc_read() != 0 && has("!!! a READ changed the register"));

	/* the helper's allowlist: any other address (also a nearby PMU word, a high-bit alias) is refused with ZERO smc */
	world();
	memset(&r, 0, sizeof(r));
	EXPECT(smc_priv_read(0x17463EC8UL, &r) == -1 && smc_priv_read(0x17463EC1UL, &r) == -1 && smc_priv_read(0x17460810UL, &r) == -1 && smc_priv_read(0, &r) == -1);
	EXPECT(smc_priv_read(0x17463EC4UL | (1ULL << 32), &r) == -1 && smc_priv_read(0x17463EE4UL, &r) == -1 && smc_priv_read(0x17463ECCUL, &r) == -1 && ncalls == 0);
	EXPECT(smc_priv_read(0x17463EC4UL, &r) == 0 && smc_priv_read(0x17463EC0UL, &r) == 0 && ncalls == 2);
	two_reads();

	puts("PASS wlansmc host test");
	return 0;
}
