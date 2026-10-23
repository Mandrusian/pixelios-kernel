/*
 * wlanrails.c - read-only rail / CMU / PMU state for the BCM4389 + PCIe1 power-up design, see wlanrails.h. Freestanding, no static state.
 * Every MMIO access is try_read32() in rd() (a fault prints "unreadable"); the rails go through acpm_s10_read() only. No store, no SMC (audited:
 * tools/test-bluejay-boot.py check_wlanrails_source_text + the wlanrails.o object audit).
 *
 * Sources ([src] = Google android-gs-bluejay-6.1-android16, local copies in ~/pixelios-artifacts/refs/wlan-stage1/, docs/research/29 sections 1 and 4):
 *  - LDOxxM_CTRL registers 0x53..0x56: S2MPG10_PM_L16M_CTRL..L19M_CTRL (s2mpg10-register.h:99-103); mode = bits 7:6 (S2MPG10_REG_ENABLE_MASK_7_6, :333; s2m_is_enabled
 *    = (val & mask) != 0, s2mpg10-regulator.c:108-122; mode 1 "ON in Standby Mode", 2 "ON in PWREN_MIF mode", 3 "ON in Normal Mode", :33-44); vsel = bits 5:0
 *    (_N_VOLTAGES(64), vsel_mask = n - 1, regulator.c:206-218); uV = MIN + STEP * vsel with LDO16M/17M = 725000 + 12500 * sel (group 3, register.h:313-317) and
 *    LDO18M/19M = 700000 + 25000 * sel (group 4, :318-320). DT windows: LDO16M/17M 725000..1300000, LDO18M/19M 1600000..1950000 (gs101-pmic.dtsi:439-481);
 *    LDO17M/19M = L17M_PCIE1 / L19M_PCIE1 = the pcie_1 PHY supplies (vreg1/vreg2-supply, gs101-common.dtsi:376-377), LDO16M/18M the PCIe0 (modem) ones.
 *  - PMU +0x3C10 bits 2,3,10 = PMU_CHECK_MASK "TXCO always on" (pcie-exynos-rc.c:80, 2692-2702).
 *  - CMU_HSI2 +0x620 PLL_CON0_MUX_CLKCMU_HSI2_PCIE_USER (flexpmu_cal_system_gs101.h:355); +0x800 bit 28 OSCCLK automatic clock gating (rc.c:2693,2704-2706,3544-3550);
 *    +0x7018/+0x7060/+0x7064/+0x7068/+0x707C = DBG_NFO_QCH_CON_{GPIO,QE_PCIE_GEN4A,QE_PCIE_GEN4B,QE_UFS_EMBD,SYSREG}_HSI2_QCH (flexpmu_cal_system_gs101.h:1313-1340).
 *    The meaning of the bits of these words is UNKNOWN: they are printed raw and compared with the UFS word only (guess: equal = same clock state).
 */
#include <stdint.h>
#include "console.h"
#include "ramlog.h"
#include "acpm.h"
#include "wlanrails.h"

#if SOC_GS101 && defined(UFS_BASE) && defined(WLAN_CMU_HSI2) && defined(WLAN_PMU_TXCO) && defined(WLAN_CMU_OFF_PCIE_MUX) && defined(WLAN_CMU_OFF_OSC) && \
	defined(WLAN_CMU_OFF_QCH_GPIO) && defined(WLAN_CMU_OFF_QCH_PCIE_A) && defined(WLAN_CMU_OFF_QCH_PCIE_B) && defined(WLAN_CMU_OFF_QCH_UFS) && \
	defined(WLAN_CMU_OFF_QCH_SYSREG) && defined(WLAN_S10_L16M_CTRL) && defined(WLAN_S10_L17M_CTRL) && defined(WLAN_S10_L18M_CTRL) && defined(WLAN_S10_L19M_CTRL)

#define UFS_REG_VER	0x08u	/* ufs.c:24 */

__attribute__((noipa)) static unsigned put_txt(char *dst, unsigned cap, unsigned pos, const char *s)
{
	while (*s && pos + 1 < cap)
		dst[pos++] = *s++;
	return pos;
}

__attribute__((noipa)) static unsigned put_dec(char *dst, unsigned cap, unsigned pos, uint32_t v)
{
	char t[10];
	unsigned n = 0;

	t[n++] = (char)('0' + v % 10u);
	v /= 10u;
	for (unsigned i = 1; i < sizeof(t) && v; i++) {
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	}
	while (n && pos + 1 < cap)
		dst[pos++] = t[--n];
	return pos;
}

__attribute__((noipa)) static unsigned put_hex(char *dst, unsigned cap, unsigned pos, uint32_t v, unsigned digits)
{
	for (unsigned i = 0; i < digits && pos + 1 < cap; i++) {
		unsigned d = (v >> (4 * (digits - 1 - i))) & 0xfu;

		dst[pos++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
	}
	return pos;
}

static void emit(char *buf, unsigned pos)
{
	if (pos >= RAMLOG_LINE_MAX)
		pos = RAMLOG_LINE_MAX - 1;
	buf[pos] = 0;
	con_str("WLAN", buf);
}

static void say(const char *a, const char *b)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, a);

	p = put_txt(line, sizeof(line), p, b);
	emit(line, p);
}

/* the only MMIO access of this file: "<grp> <name> pa=XXXXXXXX v=XXXXXXXX" or "... unreadable"; returns 1 and *v when read */
static int rd(const char *grp, const char *name, uintptr_t pa, uint32_t *v)
{
	char line[RAMLOG_LINE_MAX];
	int ok = try_read32(pa, v);
	unsigned p = put_txt(line, sizeof(line), 0, grp);

	p = put_txt(line, sizeof(line), p, " ");
	p = put_txt(line, sizeof(line), p, name);
	p = put_txt(line, sizeof(line), p, " pa=");
	p = put_hex(line, sizeof(line), p, (uint32_t)pa, 8);
	if (ok) {
		p = put_txt(line, sizeof(line), p, " v=");
		p = put_hex(line, sizeof(line), p, *v, 8);
	} else {
		p = put_txt(line, sizeof(line), p, " unreadable");
	}
	emit(line, p);
	return ok;
}

/* "<key>=<0|1>" or "<key>=?" */
static unsigned put_bit(char *dst, unsigned cap, unsigned pos, const char *key, int ok, uint32_t v, unsigned bit)
{
	pos = put_txt(dst, cap, pos, key);
	pos = put_txt(dst, cap, pos, "=");
	return ok ? put_dec(dst, cap, pos, (v >> bit) & 1u) : put_txt(dst, cap, pos, "?");
}

static unsigned put_eq(char *dst, unsigned cap, unsigned pos, const char *key, int ok, uint32_t a, uint32_t b)
{
	pos = put_txt(dst, cap, pos, key);
	pos = put_txt(dst, cap, pos, "=");
	return ok ? put_dec(dst, cap, pos, a == b ? 1u : 0u) : put_txt(dst, cap, pos, "?");
}

/* if-chains, not tables: a pointer table in .rodata would need absolute relocations (ABS64, rejected by build.sh) */
static const char *rname(unsigned k)
{
	if (k == 0)
		return "L16M(PCIe0 PHY)";
	if (k == 1)
		return "L17M(PCIe1 PHY)";
	if (k == 2)
		return "L18M(PCIe0 PHY)";
	return "L19M(PCIe1 PHY)";
}

static const char *mname(uint32_t mode)
{
	if (mode == 0)
		return "off";
	if (mode == 1)
		return "standby";
	if (mode == 2)
		return "PWREN_MIF";
	return "on-normal";
}

/* "RAIL decode <rail> mode=<m>(<name>) enabled=<0|1> sel=<n> uV=<n> dt_window=<0|1>" */
static void decode(const char *rail, uint32_t reg, uint32_t min_uv, uint32_t step_uv, uint32_t dt_min, uint32_t dt_max)
{
	char line[RAMLOG_LINE_MAX];
	uint32_t mode = (reg >> 6) & 3u, sel = reg & 0x3fu, uv = min_uv + step_uv * sel;
	unsigned p = put_txt(line, sizeof(line), 0, "RAIL decode ");

	p = put_txt(line, sizeof(line), p, rail);
	p = put_txt(line, sizeof(line), p, " mode=");
	p = put_dec(line, sizeof(line), p, mode);
	p = put_txt(line, sizeof(line), p, "(");
	p = put_txt(line, sizeof(line), p, mname(mode));
	p = put_txt(line, sizeof(line), p, ") enabled=");
	p = put_dec(line, sizeof(line), p, mode ? 1u : 0u);
	p = put_txt(line, sizeof(line), p, " sel=");
	p = put_dec(line, sizeof(line), p, sel);
	p = put_txt(line, sizeof(line), p, " uV=");
	p = put_dec(line, sizeof(line), p, uv);
	p = put_txt(line, sizeof(line), p, " dt_window=");
	p = put_dec(line, sizeof(line), p, (uv >= dt_min && uv <= dt_max) ? 1u : 0u);
	emit(line, p);
}

int wlan_rails(void)
{
	char line[RAMLOG_LINE_MAX];
	static const uint32_t regs[4] = { WLAN_S10_L16M_CTRL, WLAN_S10_L17M_CTRL, WLAN_S10_L18M_CTRL, WLAN_S10_L19M_CTRL };
	uint32_t ver = 0, mux = 0, osc = 0, qg = 0, qa = 0, qb = 0, qu = 0, qs = 0, txco = 0, v[4] = { 0, 0, 0, 0 };
	int ver_ok, gate, mux_ok = 0, osc_ok = 0, qg_ok = 0, qa_ok = 0, qb_ok = 0, qu_ok = 0, qs_ok = 0, txco_ok;
	struct acpm_cfg c;
	struct acpm_trace t;
	struct acpm_chan h;
	unsigned k, got = 0, p;
	int e = ACPM_OK;

	/* 1. CMU_HSI2, only once the UFS HCI inside HSI2 answers */
	ver_ok = try_read32(UFS_BASE + UFS_REG_VER, &ver);
	gate = ver_ok && ver != 0;
	p = put_txt(line, sizeof(line), 0, "CMU gate ufs_ver=");
	p = ver_ok ? put_hex(line, sizeof(line), p, ver, 8) : put_txt(line, sizeof(line), p, "unreadable");
	p = put_txt(line, sizeof(line), p, gate ? " open=1 (HSI2 proven alive)" : " open=0: CMU_HSI2 words NOT read");
	emit(line, p);
	if (gate) {
		mux_ok = rd("CMU", "PCIE_MUX", WLAN_CMU_HSI2 + WLAN_CMU_OFF_PCIE_MUX, &mux);
		osc_ok = rd("CMU", "OSC", WLAN_CMU_HSI2 + WLAN_CMU_OFF_OSC, &osc);
		qg_ok = rd("CMU", "QCH_GPIO", WLAN_CMU_HSI2 + WLAN_CMU_OFF_QCH_GPIO, &qg);
		qa_ok = rd("CMU", "QCH_PCIE_A", WLAN_CMU_HSI2 + WLAN_CMU_OFF_QCH_PCIE_A, &qa);
		qb_ok = rd("CMU", "QCH_PCIE_B", WLAN_CMU_HSI2 + WLAN_CMU_OFF_QCH_PCIE_B, &qb);
		qu_ok = rd("CMU", "QCH_UFS", WLAN_CMU_HSI2 + WLAN_CMU_OFF_QCH_UFS, &qu);
		qs_ok = rd("CMU", "QCH_SYSREG", WLAN_CMU_HSI2 + WLAN_CMU_OFF_QCH_SYSREG, &qs);
		p = put_bit(line, sizeof(line), put_txt(line, sizeof(line), 0, "CMU decode "), "pcie_mux_bit4", mux_ok, mux, 4);
		p = put_txt(line, sizeof(line), p, " (guess: 1 = user mux selected, as gpu_gate)");
		emit(line, p);
		p = put_bit(line, sizeof(line), put_txt(line, sizeof(line), 0, "CMU decode "), "osc_bit28", osc_ok, osc, 28);
		p = put_txt(line, sizeof(line), p, " (guess: 1 = automatic clock gating on, rc.c:3547-3550)");
		emit(line, p);
		p = put_eq(line, sizeof(line), put_txt(line, sizeof(line), 0, "CMU decode qch same_as_ufs: "), "gpio", qg_ok && qu_ok, qg, qu);
		p = put_eq(line, sizeof(line), put_txt(line, sizeof(line), p, " "), "pcie_a", qa_ok && qu_ok, qa, qu);
		p = put_eq(line, sizeof(line), put_txt(line, sizeof(line), p, " "), "pcie_b", qb_ok && qu_ok, qb, qu);
		p = put_eq(line, sizeof(line), put_txt(line, sizeof(line), p, " "), "sysreg", qs_ok && qu_ok, qs, qu);
		p = put_txt(line, sizeof(line), p, " (guess; bit layout UNKNOWN)");
		emit(line, p);
	}

	/* 2. PMU +0x3C10: bits 2,3,10 must all be set for the PCIe1 reference clock (rc.c:80,2698) */
	txco_ok = rd("PMU", "TXCO", WLAN_PMU_TXCO, &txco);
	if (txco_ok) {
		p = put_txt(line, sizeof(line), 0, "PMU decode txco bits 2,3,10 all set=");
		p = put_dec(line, sizeof(line), p, (txco & 0x40cu) == 0x40cu ? 1u : 0u);
		p = put_txt(line, sizeof(line), p, " (read from source: PMU_CHECK_MASK, rc.c:80,2698)");
		emit(line, p);
	}

	/* 3. the PCIe PHY rails through the ACPM client, same gates as gpu_rails() */
	acpm_cfg_init(&c);
	if (!c.layout_verified) {
		say("RAIL refused: ", acpm_why(ACPM_W_LAYOUT));
	} else if (!c.elem_unit_verified) {
		say("RAIL refused: ", acpm_why(ACPM_W_UNIT));
	} else if (!counter_freq()) {
		say("RAIL refused: ", acpm_why(ACPM_W_TIMER));
	} else if (acpm_describe(&c, &h)) {
		say("RAIL refused: ", acpm_why(h.why));
		say("RAIL describe failed, ", "no request sent");
	} else if (h.rr != h.rf || h.sr != h.sf) {
		say("RAIL refused: ", acpm_why(ACPM_W_BUSY));
		say("RAIL queues not empty, ", "no request sent");
	} else {
		acpm_xfer_zero(&t.a);
		acpm_xfer_zero(&t.b);
		t.seq = 1;
		t.n = 0;
		for (k = 0; k < 4; k++) {
			e = acpm_s10_read(&c, regs[k], &v[k], &t);
			p = put_txt(line, sizeof(line), 0, "RAIL ");
			p = put_txt(line, sizeof(line), p, rname(k));
			p = put_txt(line, sizeof(line), p, " reg=0x");
			p = put_hex(line, sizeof(line), p, regs[k], 2);
			if (e) {
				p = put_txt(line, sizeof(line), p, " FAILED err=");
				p = put_dec(line, sizeof(line), p, (uint32_t)e);
				p = put_txt(line, sizeof(line), p, " ");
				p = put_txt(line, sizeof(line), p, acpm_why(t.a.why));
				emit(line, p);
				if (t.a.published)
					say("RAIL STATE: ", "the request stays queued; channel left as is, rerun only after reading this");
				break;
			}
			p = put_txt(line, sizeof(line), p, " v=0x");
			p = put_hex(line, sizeof(line), p, v[k], 2);
			emit(line, p);
			got++;
		}
		if (got == 4) {
			decode(rname(0), v[0], 725000u, 12500u, 725000u, 1300000u);
			decode(rname(1), v[1], 725000u, 12500u, 725000u, 1300000u);
			decode(rname(2), v[2], 700000u, 25000u, 1600000u, 1950000u);
			decode(rname(3), v[3], 700000u, 25000u, 1600000u, 1950000u);
			say("RAIL decode note: ", "mode 1 is the DT default of every always-on LDO; whether it delivers voltage in active state = guess");
		}
	}

	/* 4. the one summary line */
	p = put_txt(line, sizeof(line), 0, "RAILS l17=");
	if (got == 4) {
		p = put_dec(line, sizeof(line), p, (v[1] >> 6) & 3u);
		p = put_txt(line, sizeof(line), p, "/");
		p = put_dec(line, sizeof(line), p, 725000u + 12500u * (v[1] & 0x3fu));
	} else {
		p = put_txt(line, sizeof(line), p, "?");
	}
	p = put_txt(line, sizeof(line), p, " l19=");
	if (got == 4) {
		p = put_dec(line, sizeof(line), p, (v[3] >> 6) & 3u);
		p = put_txt(line, sizeof(line), p, "/");
		p = put_dec(line, sizeof(line), p, 700000u + 25000u * (v[3] & 0x3fu));
	} else {
		p = put_txt(line, sizeof(line), p, "?");
	}
	p = put_bit(line, sizeof(line), put_txt(line, sizeof(line), p, " "), "cmu_mux", mux_ok, mux, 4);
	p = put_bit(line, sizeof(line), put_txt(line, sizeof(line), p, " "), "txco", txco_ok, (txco & 0x40cu) == 0x40cu ? 1u : 0u, 0);
	emit(line, p);
	return got == 4 ? 0 : 1;
}

#else	/* not gs101 / addresses missing: refuse, no MMIO, no ACPM traffic */

int wlan_rails(void)
{
	con_str("WLAN", "RAIL refused: soc.json wlan/acpm values missing, nothing read");
	return 1;
}

#endif
