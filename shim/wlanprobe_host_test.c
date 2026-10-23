/* Host test for wlanprobe.c / wlanprobe.h: the pure decoders and gate, AND the real probe file built against a fake register window with a faulting
 * address (review D3): exact output lines, the SUMMARY, that a closed gate reads NOTHING of the PCIe wrapper, the group order (PMA last), and that
 * no line exceeds the RAM-log line size. try_write32 is deliberately NOT defined, so linking fails if the probe ever references a writer.
 * Uses the REAL generated board macros. Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -I. -Iout -include out/board_config.h -o out/wlanprobe_host_test wlanprobe_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wlanprobe.c"

#if !(SOC_GS101 && defined(WLAN_ELBI) && defined(WLAN_CMU_HSI2))
#error "needs the bluejay board_config.h (shim/build.sh --board bluejay writes out/board_config.h)"
#endif

static char lines[512][RAMLOG_LINE_MAX + 8];
static unsigned nl;
static uint32_t iso1, mux, pma_3f0, pma_e0c, pma_de8, elbi_2c8, dbi_id;
static int iso_fault, mux_fault;
static uintptr_t bad_pa;
static uintptr_t reads[1024];
static unsigned nr;

void con_str(const char *label, const char *s)
{
	assert(nl < 512);
	assert(strlen(label) + 1 + strlen(s) < RAMLOG_LINE_MAX + 4);
	assert(strlen(s) < RAMLOG_LINE_MAX);
	snprintf(lines[nl++], sizeof(lines[0]), "%s %s", label, s);
}

int try_read32(uintptr_t pa, uint32_t *v)
{
	assert(nr < 1024);
	reads[nr++] = pa;
	if (pa == bad_pa || (iso_fault && pa == WLAN_PMU_PCIE1_ISO) || (mux_fault && pa == WLAN_CMU_HSI2 + 0x620))
		return 0;
	if (pa == WLAN_PMU_PCIE1_ISO) *v = iso1;
	else if (pa == WLAN_PMU_PCIE0_ISO) *v = 0;
	else if (pa == WLAN_CMU_HSI2 + 0x620) *v = mux;
	else if (pa == WLAN_GPP14 + 4) *v = 0x4;		/* WLAN_EN gpp14-2 high */
	else if (pa == WLAN_GPH2 + 4) *v = 0x0;			/* PERST low */
	else if (pa == WLAN_PMA + 0x3f0) *v = pma_3f0;
	else if (pa == WLAN_PMA + 0xe0c) *v = pma_e0c;
	else if (pa == WLAN_PMA + 0xde8) *v = pma_de8;
	else if (pa == WLAN_ELBI + 0x2c8) *v = elbi_2c8;
	else if (pa == WLAN_DBI) *v = dbi_id;
	else *v = 0xA0000000u | (uint32_t)(pa & 0xfffffu);
	return 1;
}

static int has(const char *s)
{
	for (unsigned i = 0; i < nl; i++)
		if (!strcmp(lines[i], s))
			return 1;
	return 0;
}

static unsigned reads_in(uintptr_t lo, uintptr_t hi)
{
	unsigned n = 0;

	for (unsigned i = 0; i < nr; i++)
		n += reads[i] >= lo && reads[i] < hi;
	return n;
}

static void reset(void)
{
	nl = nr = 0;
	iso1 = 1;
	mux = 0x10;
	pma_3f0 = 0x8;
	pma_e0c = 0x4;
	pma_de8 = 0xf;
	elbi_2c8 = 0x11;
	dbi_id = 0x12345678u;
	iso_fault = mux_fault = 0;
	bad_pa = 0;
}

#define EXPECT(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); for (unsigned i_ = 0; i_ < nl; i_++) fprintf(stderr, "  %s\n", lines[i_]); assert(0); } } while (0)

static void run_groups(void)
{
	for (unsigned g = 0; g < 4; g++)
		wlan_probe_l2_group(g);
	wlan_probe_l2_group(4);
}

int main(void)
{
	/* decoders (a bank has 8 pins: out of range is bounded, review D3) */
	assert(wlan_gpio_func(0x00000100u, 2) == 1 && wlan_gpio_func(0x00000100u, 1) == 0 && wlan_gpio_func(0x00000100u, 3) == 0);
	assert(wlan_gpio_level(0x4u, 2) == 1 && wlan_gpio_level(0x4u, 0) == 0);
	assert(wlan_gpio_func(0x00F00021u, 0) == 1 && wlan_gpio_func(0x00F00021u, 1) == 2 && wlan_gpio_func(0x00F00021u, 5) == 0xF);
	assert(wlan_gpio_func(0xF0000000u, 7) == 0xF && wlan_gpio_func(0xFFFFFFFFu, 8) == 0xF && wlan_gpio_func(0x1u, 32) == 0xF && wlan_gpio_level(0xFFu, 8) == 0 && wlan_gpio_level(0xFFu, 31) == 0);
	/* the gate: PHY not isolated AND the CMU mux selected, every other case closed */
	assert(wlan_gate(1, 1, 1, 0x10) == WLAN_GATE_OPEN);
	assert(wlan_gate(0, 1, 1, 0x10) == WLAN_GATE_ISO_UNREADABLE);
	assert(wlan_gate(1, 0, 1, 0x10) == WLAN_GATE_PHY_ISOLATED);
	assert(wlan_gate(1, 0xfffffffeu, 1, 0x10) == WLAN_GATE_PHY_ISOLATED);	/* only bit 0 counts */
	assert(wlan_gate(1, 1, 0, 0) == WLAN_GATE_MUX_UNREADABLE);
	assert(wlan_gate(1, 1, 1, 0xffffffefu) == WLAN_GATE_MUX_OFF);		/* only bit 4 counts */
	assert(!strcmp(wlan_gate_name(WLAN_GATE_PHY_ISOLATED), "pcie1-phy-isolated") && !strcmp(wlan_gate_name(WLAN_GATE_OPEN), "open"));

	/* level 1: exact lines */
	reset();
	wlan_probe_l1();
	EXPECT(has("WLAN PMU_PCIE1_ISO pa=17463EC4 v=00000001") && has("WLAN PMU_PCIE0_ISO pa=17463EC0 v=00000000"));
	EXPECT(has("WLAN PD_HSI0_CFG pa=17462080 v=A0062080") && has("WLAN PD_HSI0_STAT pa=17462084 v=A0062084") && has("WLAN PD_HSI2_A pa=17462180 v=A0062180"));
	EXPECT(has("WLAN GPP14.CON pa=108401C0 v=A00401C0") && has("WLAN GPP14.DAT pa=108401C4 v=00000004") && has("WLAN WLAN_EN gpp14-2 func=1 lvl=1"));
	EXPECT(has("WLAN GPH2.CON pa=14440000 v=A0040000") && has("WLAN GPH2.DAT pa=14440004 v=00000000") && has("WLAN PERST gph2-0 func=0 lvl=0") && has("WLAN DEVWAKE gph2-5 func=0 lvl=0"));
	EXPECT(has("WLAN GPA7.CON pa=174E0020 v=A00E0020") && has("WLAN HOSTWAKE gpa7-0 func=0 lvl=0"));

	/* gate open: all four groups and the SUMMARY, exact */
	reset();
	EXPECT(wlan_probe_l2_open() == 1 && has("WLAN CMU_HSI2_PCIE_MUX pa=14400620 v=00000010") && has("WLAN l2 gate=open"));
	run_groups();
	EXPECT(has("WLAN SYSREG+000 pa=14420000 v=A0020000") && has("WLAN SYSREG+01C pa=1442001C v=A002001C"));
	EXPECT(has("WLAN ELBI+000 pa=14520000 v=A0020000") && has("WLAN ELBI+040 pa=14520040 v=A0020040") && has("WLAN ELBI+054 pa=14520054 v=A0020054"));
	EXPECT(has("WLAN ELBI+080 pa=14520080 v=A0020080") && has("WLAN ELBI+2C8 pa=145202C8 v=00000011") && has("WLAN ELBI+3A4 pa=145203A4 v=A00203A4"));
	EXPECT(has("WLAN DBI+000 pa=14800000 v=12345678") && has("WLAN DBI+00C pa=1480000C v=A000000C"));
	EXPECT(has("WLAN PMA+3F0 pa=145503F0 v=00000008") && has("WLAN PMA+E0C pa=14550E0C v=00000004") && has("WLAN PMA+DE8 pa=14550DE8 v=0000000F"));
	EXPECT(has("WLAN PMA+5FC pa=145505FC v=A00505FC") && has("WLAN PMA+E18 pa=14550E18 v=A0050E18"));
	EXPECT(has("WLAN PHY pll_lock=1 cdr_lock=1 oc_done=1 oc_after_rst=0"));		/* E18 pattern word bit 7 = 0 */
	EXPECT(has("WLAN SUMMARY gate=open en=1 perst=0 dbi_id=12345678 link=11"));
	EXPECT(!has("WLAN PCS+000 pa=14540000 v=A0040000") && reads_in(WLAN_PCS, WLAN_PCS + 0x1000) == 0);	/* the uncited PCS / PMA 00..1C dumps are gone */
	EXPECT(reads_in(WLAN_PMA, WLAN_PMA + 0x1000) == 5);
	{	/* PMA, the riskiest block, is read after the last SYSREG / ELBI / DBI read of the groups (the SUMMARY's two re-reads come later still) */
		unsigned dbi0c = 0, first_pma = 0;

		for (unsigned i = 0; i < nr; i++) {
			if (reads[i] == WLAN_DBI + 0x0c && !dbi0c)
				dbi0c = i;
			if (reads[i] >= WLAN_PMA && reads[i] < WLAN_PMA + 0x1000 && !first_pma)
				first_pma = i;
		}
		EXPECT(dbi0c && first_pma && dbi0c < first_pma);
	}

	/* a faulting address: printed as unreadable, the dump goes on, the decoded fields say '?' */
	reset(); bad_pa = WLAN_ELBI + 0x2c8;
	EXPECT(wlan_probe_l2_open() == 1);
	run_groups();
	EXPECT(has("WLAN ELBI+2C8 pa=145202C8 unreadable") && has("WLAN ELBI+2E8 pa=145202E8 v=A00202E8") && has("WLAN SUMMARY gate=open en=1 perst=0 dbi_id=12345678 link=?"));
	reset(); bad_pa = WLAN_PMA + 0xe0c;
	wlan_probe_l2_open();
	run_groups();
	EXPECT(has("WLAN PMA+E0C pa=14550E0C unreadable") && has("WLAN PHY pll_lock=1 cdr_lock=? oc_done=1 oc_after_rst=0"));

	/* closed gates read NOTHING of the wrapper or the PHY: isolated PHY, unreadable ISO word, mux off, mux unreadable */
	for (int c = 0; c < 4; c++) {
		reset();
		if (c == 0) iso1 = 0;
		if (c == 1) iso_fault = 1;
		if (c == 2) mux = 0xffffffefu;
		if (c == 3) mux_fault = 1;
		EXPECT(wlan_probe_l2_open() == 0);
		run_groups();
		EXPECT(reads_in(WLAN_SYSREG, WLAN_SYSREG + 0x1000) == 0 && reads_in(WLAN_ELBI, WLAN_ELBI + 0x1000) == 0 && reads_in(WLAN_DBI, WLAN_DBI + 0x1000) == 0 &&
		       reads_in(WLAN_PMA, WLAN_PMA + 0x1000) == 0 && reads_in(WLAN_PCS, WLAN_PCS + 0x1000) == 0);
		EXPECT(has(c == 0 ? "WLAN l2 gated: pcie1-phy-isolated" : c == 1 ? "WLAN l2 gated: pmu-iso-unreadable" : c == 2 ? "WLAN l2 gated: cmu-hsi2-pcie-mux-off" : "WLAN l2 gated: cmu-hsi2-unreadable"));
		EXPECT(has(c == 0 ? "WLAN SUMMARY gate=pcie1-phy-isolated en=1 perst=0 dbi_id=unreadable link=?" :
			   c == 1 ? "WLAN SUMMARY gate=pmu-iso-unreadable en=1 perst=0 dbi_id=unreadable link=?" :
			   c == 2 ? "WLAN SUMMARY gate=cmu-hsi2-pcie-mux-off en=1 perst=0 dbi_id=unreadable link=?" :
				    "WLAN SUMMARY gate=cmu-hsi2-unreadable en=1 perst=0 dbi_id=unreadable link=?"));
		if (c == 0)
			EXPECT(reads_in(WLAN_CMU_HSI2, WLAN_CMU_HSI2 + 0x1000) == 0);	/* the CMU word is read only after the PHY says powered */
	}
	/* no line is longer than the RAM-log line (checked in con_str for every line above) */
	puts("PASS wlanprobe decoders, gate and probe output");
	return 0;
}
