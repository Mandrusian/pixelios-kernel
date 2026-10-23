/* Host test for wlanrails.c (--wlan-rails, docs/research/29 section 4) against the FAKE APM of acpm_host_test.c (#included, its main renamed) plus fake UFS / CMU_HSI2 / PMU
 * words: the four S2MPG10 READ requests (speedy channel 0, type 1, FUNC_READ, registers 0x53..0x56 only), the printed WLAN CMU / PMU / RAIL lines and decodes, the exact set of MMIO
 * addresses read (UFS VER, the seven CMU_HSI2 words, PMU+0x3C10), the CMU gate (UFS unreadable / zero = no CMU read at all), a faulting CMU word, the ACPM refusals and the first
 * failure stopping the rest. wlanrails.c has no write path: the only try_write32 in this binary is the fake APM's own IPC store, and every write is checked to be an IPC channel word.
 * Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -Wno-unused-function -DTOUCH_RAILS_WRITE -I. -Iout -include out/board_config.h -o out/wlanrails_host_test wlanrails_host_test.c */
#define main acpm_host_main
#define try_read32 acpm_fake_read32
#include "acpm_host_test.c"
#undef try_read32
#undef main
int try_read32(uintptr_t address, uint32_t *value);
#include "wlanrails.c"

static char lines[512][RAMLOG_LINE_MAX + 8];
static unsigned nl;
static uintptr_t reads[64];
static unsigned nreads;
static uint32_t ufs_ver, cmu_w[0x8000 / 4], pmu_txco;
static int ufs_fault;
static uintptr_t cmu_fault;

void con_str(const char *label, const char *s)
{
	assert(nl < 512);
	assert(strlen(s) < RAMLOG_LINE_MAX);
	snprintf(lines[nl++], sizeof(lines[0]), "%s %s", label, s);
}

int try_read32(uintptr_t pa, uint32_t *v)
{
	if (pa == UFS_BASE + 0x08) {
		reads[nreads++] = pa;
		if (ufs_fault)
			return 0;
		*v = ufs_ver;
		return 1;
	}
	if (pa >= WLAN_CMU_HSI2 && pa < WLAN_CMU_HSI2 + 0x8000) {
		reads[nreads++] = pa;
		if (pa == cmu_fault)
			return 0;
		*v = cmu_w[(pa - WLAN_CMU_HSI2) / 4];
		return 1;
	}
	if (pa == WLAN_PMU_TXCO) {
		reads[nreads++] = pa;
		*v = pmu_txco;
		return 1;
	}
	return acpm_fake_read32(pa, v);	/* the IPC channel's own SRAM / mailbox words */
}

static int has(const char *s)
{
	for (unsigned i = 0; i < nl; i++)
		if (strstr(lines[i], s))
			return 1;
	return 0;
}

#define EXPECT(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); for (unsigned i_ = 0; i_ < nl; i_++) fprintf(stderr, "  %s\n", lines[i_]); assert(0); } } while (0)

static void world(void)
{
	setup(ESZ);
	nl = nreads = 0;
	ufs_ver = 0x300;	/* observed: hardware/ufs-gpt-read-2026-10-21.txt:273 */
	ufs_fault = 0;
	cmu_fault = 0;
	pmu_txco = 0x40c;
	for (unsigned i = 0; i < sizeof(cmu_w) / 4; i++)
		cmu_w[i] = 0xC0000000u + i;
	cmu_w[WLAN_CMU_OFF_PCIE_MUX / 4] = 0x10;
	cmu_w[WLAN_CMU_OFF_OSC / 4] = 0x10000000u;
	cmu_w[WLAN_CMU_OFF_QCH_UFS / 4] = 0x1111;
	cmu_w[WLAN_CMU_OFF_QCH_PCIE_A / 4] = 0x1111;
	cmu_w[WLAN_CMU_OFF_QCH_PCIE_B / 4] = 0x2222;
	cmu_w[WLAN_CMU_OFF_QCH_GPIO / 4] = 0x1111;
	cmu_w[WLAN_CMU_OFF_QCH_SYSREG / 4] = 0x3333;
	pmic[0x53] = 0x40 | 8;	/* LDO16M mode 1, sel 8 -> 725000 + 100000 */
	pmic[0x54] = 0xC0 | 12;	/* LDO17M mode 3, sel 12 -> 725000 + 150000 = 875000 */
	pmic[0x55] = 0xC0 | 36;	/* LDO18M mode 3, sel 36 -> 700000 + 900000 = 1600000 */
	pmic[0x56] = 0x40 | 36;	/* LDO19M mode 1, sel 36 -> 1600000 */
}

/* every MMIO read of wlanrails.c itself is one of the cited words */
static void reads_ok(unsigned cmu_expected)
{
	unsigned cmu = 0;

	for (unsigned i = 0; i < nreads; i++) {
		uintptr_t pa = reads[i];

		if (pa == UFS_BASE + 0x08 || pa == WLAN_PMU_TXCO)
			continue;
		EXPECT(pa == WLAN_CMU_HSI2 + WLAN_CMU_OFF_PCIE_MUX || pa == WLAN_CMU_HSI2 + WLAN_CMU_OFF_OSC || pa == WLAN_CMU_HSI2 + WLAN_CMU_OFF_QCH_GPIO ||
		       pa == WLAN_CMU_HSI2 + WLAN_CMU_OFF_QCH_PCIE_A || pa == WLAN_CMU_HSI2 + WLAN_CMU_OFF_QCH_PCIE_B || pa == WLAN_CMU_HSI2 + WLAN_CMU_OFF_QCH_UFS ||
		       pa == WLAN_CMU_HSI2 + WLAN_CMU_OFF_QCH_SYSREG);
		cmu++;
	}
	EXPECT(cmu == cmu_expected);
}

static void only_ipc_writes(void)
{
	for (unsigned i = 0; i < nw; i++) {
		uintptr_t pa = wlog[i].pa;

		EXPECT((pa >= SRAM + REQ_BASE_O && pa < SRAM + REQ_BASE_O + QLEN * ESZ) || pa == SRAM + REQ_FRONT_O || pa == SRAM + RSP_REAR_O ||
		       pa == MBOX + ACPM_INTGR1 || pa == MBOX + ACPM_INTCR0);
	}
}

int main(void)
{
	static const uint32_t want[4] = { 0x53, 0x54, 0x55, 0x56 };

	/* happy path */
	world();
	EXPECT(wlan_rails() == 0);
	EXPECT(has("WLAN CMU gate ufs_ver=00000300 open=1"));
	EXPECT(has("WLAN CMU PCIE_MUX pa=14400620 v=00000010") && has("WLAN CMU OSC pa=14400800 v=10000000"));
	EXPECT(has("WLAN CMU QCH_GPIO pa=14407018 v=00001111") && has("WLAN CMU QCH_PCIE_A pa=14407060 v=00001111") && has("WLAN CMU QCH_PCIE_B pa=14407064 v=00002222"));
	EXPECT(has("WLAN CMU QCH_UFS pa=14407068 v=00001111") && has("WLAN CMU QCH_SYSREG pa=1440707C v=00003333"));
	EXPECT(has("WLAN CMU decode pcie_mux_bit4=1 (guess:") && has("WLAN CMU decode osc_bit28=1 (guess:"));
	EXPECT(has("WLAN CMU decode qch same_as_ufs: gpio=1 pcie_a=1 pcie_b=0 sysreg=0 (guess; bit layout UNKNOWN)"));
	EXPECT(has("WLAN PMU TXCO pa=17463C10 v=0000040C") && has("WLAN PMU decode txco bits 2,3,10 all set=1 (read from source:"));
	EXPECT(has("WLAN RAIL L16M(PCIe0 PHY) reg=0x53 v=0x48") && has("WLAN RAIL L17M(PCIe1 PHY) reg=0x54 v=0xCC"));
	EXPECT(has("WLAN RAIL L18M(PCIe0 PHY) reg=0x55 v=0xE4") && has("WLAN RAIL L19M(PCIe1 PHY) reg=0x56 v=0x64"));
	EXPECT(has("WLAN RAIL decode L16M(PCIe0 PHY) mode=1(standby) enabled=1 sel=8 uV=825000 dt_window=1"));
	EXPECT(has("WLAN RAIL decode L17M(PCIe1 PHY) mode=3(on-normal) enabled=1 sel=12 uV=875000 dt_window=1"));
	EXPECT(has("WLAN RAIL decode L18M(PCIe0 PHY) mode=3(on-normal) enabled=1 sel=36 uV=1600000 dt_window=1"));
	EXPECT(has("WLAN RAIL decode L19M(PCIe1 PHY) mode=1(standby) enabled=1 sel=36 uV=1600000 dt_window=1"));
	EXPECT(has("WLAN RAILS l17=3/875000 l19=1/1600000 cmu_mux=1 txco=1"));
	reads_ok(7);
	EXPECT(nbell == 4 && nw == 32);
	{
		uint32_t w0[8], w1[8], n = 0;

		for (unsigned i = 0; i + 1 < nw; i++)
			if (wlog[i].pa >= SRAM + REQ_BASE_O && wlog[i].pa < SRAM + REQ_BASE_O + QLEN * ESZ && (wlog[i].pa - SRAM - REQ_BASE_O) % ESZ == 0) {
				w0[n] = wlog[i].v & 0xffffu;
				w1[n] = wlog[i + 1].v;
				n++;
			}
		EXPECT(n == 4);
		for (unsigned i = 0; i < 4; i++)
			EXPECT(w0[i] == (want[i] | 0x0100u) && w1[i] == 0);	/* channel 0 (S2MPG10), type 1, FUNC_READ = 0 */
	}
	only_ipc_writes();
	EXPECT(pmic[0x54] == (0xC0 | 12) && pmic[0x56] == (0x40 | 36));	/* the PMIC fake was not modified */

	/* a rail outside DT window and an off rail are decoded, not judged */
	world();
	pmic[0x54] = 0x00 | 63;	/* off, sel 63 -> 725000 + 787500 = 1512500 (outside 725000..1300000) */
	EXPECT(wlan_rails() == 0);
	EXPECT(has("L17M(PCIe1 PHY) mode=0(off) enabled=0 sel=63 uV=1512500 dt_window=0") && has("WLAN RAILS l17=0/1512500"));

	/* CMU gate: UFS HCI unreadable or zero -> not one CMU word is read; PMU and rails still are */
	world(); ufs_fault = 1;
	EXPECT(wlan_rails() == 0);
	EXPECT(has("WLAN CMU gate ufs_ver=unreadable open=0: CMU_HSI2 words NOT read") && !has("WLAN CMU PCIE_MUX"));
	reads_ok(0);
	EXPECT(has("WLAN PMU TXCO") && has("WLAN RAIL L17M(PCIe1 PHY) reg=0x54") && has("cmu_mux=? txco=1"));
	world(); ufs_ver = 0;
	EXPECT(wlan_rails() == 0 && has("open=0: CMU_HSI2 words NOT read") && !has("WLAN CMU OSC"));
	reads_ok(0);

	/* one CMU word faults: "unreadable", the rest continue, the decode shows '?' */
	world(); cmu_fault = WLAN_CMU_HSI2 + WLAN_CMU_OFF_PCIE_MUX;
	EXPECT(wlan_rails() == 0);
	EXPECT(has("WLAN CMU PCIE_MUX pa=14400620 unreadable") && has("WLAN CMU decode pcie_mux_bit4=? (guess:") && has("cmu_mux=? txco=1") && has("WLAN CMU OSC pa=14400800 v="));

	/* PMU word 0x40C missing a bit */
	world(); pmu_txco = 0x404;
	EXPECT(wlan_rails() == 0 && has("all set=0") && has("txco=0"));

	/* the first failed rail read stops the rest: the APM returns an error code */
	world(); mode = M_RET;
	EXPECT(wlan_rails() == 1 && nbell == 1 && has("WLAN RAIL L16M(PCIe0 PHY) reg=0x53 FAILED") && !has("reg=0x54") && has("WLAN RAILS l17=? l19=?"));
	world(); mode = M_NEVER;
	EXPECT(wlan_rails() == 1 && nbell == 1 && has("the request stays queued"));

	/* ACPM refusals: zero IPC writes, the MMIO words are still read */
	world(); setS(REQ_FRONT_O, 0);	/* queue not empty */
	EXPECT(wlan_rails() == 1 && nw == 0 && nbell == 0 && has("WLAN RAIL refused: ") && has("WLAN CMU PCIE_MUX"));
	world(); setup(8);		/* wrong element size */
	nl = nreads = 0;
	EXPECT(wlan_rails() == 1 && nw == 0 && nbell == 0 && has("WLAN RAIL refused: "));

	/* acpm_s10_read: only the four PHY LDO registers */
	{
		struct acpm_trace t;
		uint32_t v = 0;

		world();
		t = trace();
		EXPECT(acpm_s10_read(&cfg, 0x5C, &v, &t) == ACPM_E_ARG && nw == 0);	/* a touch rail is not on the list */
		EXPECT(acpm_s10_read(&cfg, 0x57, &v, &t) == ACPM_E_ARG && nw == 0);	/* LDO20M */
		EXPECT(acpm_s10_read(&cfg, 0x52, &v, &t) == ACPM_E_ARG && nw == 0);
		EXPECT(acpm_s10_read(&cfg, 0x54 + 0x100, &v, &t) == ACPM_E_ARG && nw == 0);
		EXPECT(acpm_s10_read(&cfg, 0x54, &v, &t) == ACPM_OK && nbell == 1 && v == (0xC0u | 12u));
		EXPECT(t.a.req[0] == (0x54u | 0x0100u | (1u << 16)) && t.a.req[1] == 0);
	}
	puts("PASS wlanrails host test");
	return 0;
}
