#pragma once
#include <stdint.h>

/*
 * wlanprobe.h - STRICTLY read-only probe of the BCM4389 Wi-Fi radio path
 * (docs/research/12-wifi-bluejay.md sections 1.2 and 4, step 5): PMU
 * isolation/power-domain words, the WLAN_EN / PERST / CLKREQ / wake pin
 * banks, and the HSI2 PCIe1 wrapper (sysreg, ELBI, PHY, PCS, DBI). Built
 * and linked only with build.sh --wlan-probe on a gs101 board; other
 * boards get an empty stub. Addresses come from soc/google/gs101/soc.json
 * (the "wlan" object) through the generated WLAN_* board macros.
 *
 * Two halves so a bus hang stays attributable: l1 (PMU + pin banks) then
 * l2 (PCIe wrapper, may hang the bus on a gated block). Level 2 is GATED
 * (wlan_gate, same by-construction pattern as gpu_gate in gpuprobe.h): the
 * wrapper is read only if the PMU says the PCIe1 PHY is not isolated AND the
 * CMU_HSI2 PCIe user mux is selected (the state Linux's own power-on leaves);
 * otherwise it prints `WLAN l2 gated: <reason>` and reads nothing. The caller
 * runs wlan_probe_l2_group(0..WLAN_L2_GROUPS-1) with a pstore checkpoint after
 * each so the last line of a hang names the block (PMA, the riskiest, is last).
 * The outbound config window is never touched.
 */

/* Samsung pinctrl bank: CON is 4 bits per pin (1 = output), DAT 1 bit per pin. */
static inline uint32_t wlan_gpio_func(uint32_t con, unsigned pin)
{
	return pin < 8u ? (con >> (4u * pin)) & 0xfu : 0xfu;	/* a bank has 8 pins; out of range reads as "invalid" instead of shifting by >= 32 (review D3) */
}

static inline uint32_t wlan_gpio_level(uint32_t dat, unsigned pin)
{
	return pin < 8u ? (dat >> pin) & 1u : 0u;
}

/*
 * Level-2 gate. Inputs are the raw words and whether each read worked.
 *  - PMU +0x3EC4 (PCIe1) bit 0 = PHY isolation: 1 = isolation bypassed / released (not a power switch: *guess*), 0 = isolated
 *    [src: docs/research/12 section 1.2 table, soc-gs pcie-exynos-rc.h:120 mask, pcie-exynos-common.h:52 PCIE_PHY_BYPASS = 1; the neighbouring word
 *    0x17463EC8 was read on hardware, docs/research/10 [obs]].
 *  - CMU_HSI2 +0x620 PLL_CON0_MUX_CLKCMU_HSI2_PCIE_USER bit 4 = the user-mux select that Linux's own sequences set when a block is on
 *    [src: soc-gs flexpmu_cal_system_gs101.h:355 for the register; the same bit-4 convention as embedded_g3d_on, flexpmu_cal_local_gs101.h:452-454;
 *    that bit 4 is the select is a guess, as in gpu_gate()]. CMU_HSI2 itself is readable by construction: the HSI2 domain is never power-gated
 *    (pd_hsi2 is `status = "disabled"`, gs101-pm-domains.dtsi:90-98, no hsi2 power sequence in flexpmu_cal_local_gs101.h pmucal_pd_list) and the shim
 *    already reads the UFS controller inside HSI2 (0x14700000, hardware/ufs-gpt-read-2026-10-21.txt [obs]).
 * No PMU status word of the HSI2 domain itself is citable (research 20 section 3 D1/D5), so none is used.
 */
enum wlan_gate {
	WLAN_GATE_OPEN = 0,
	WLAN_GATE_ISO_UNREADABLE,
	WLAN_GATE_PHY_ISOLATED,
	WLAN_GATE_MUX_UNREADABLE,
	WLAN_GATE_MUX_OFF,
};

#define WLAN_MUX_SELECT	(1u << 4)

static inline enum wlan_gate wlan_gate(int iso_ok, uint32_t iso, int mux_ok, uint32_t mux)
{
	if (!iso_ok)
		return WLAN_GATE_ISO_UNREADABLE;
	if (!(iso & 1u))
		return WLAN_GATE_PHY_ISOLATED;
	if (!mux_ok)
		return WLAN_GATE_MUX_UNREADABLE;
	if (!(mux & WLAN_MUX_SELECT))
		return WLAN_GATE_MUX_OFF;
	return WLAN_GATE_OPEN;
}

static inline const char *wlan_gate_name(enum wlan_gate g)
{
	/* if-chain on purpose: a switch becomes an absolute-pointer table (ABS64 reloc, build.sh audit). */
	if (g == WLAN_GATE_OPEN) return "open";
	if (g == WLAN_GATE_ISO_UNREADABLE) return "pmu-iso-unreadable";
	if (g == WLAN_GATE_PHY_ISOLATED) return "pcie1-phy-isolated";
	if (g == WLAN_GATE_MUX_UNREADABLE) return "cmu-hsi2-unreadable";
	if (g == WLAN_GATE_MUX_OFF) return "cmu-hsi2-pcie-mux-off";
	return "?";
}

/* Fault-guarded 32-bit MMIO read (shim.c; host tests substitute a fake window). */
int try_read32(uintptr_t address, uint32_t *value);

#define WLAN_L2_GROUPS	5u	/* 0 SYSREG, 1 ELBI, 2 DBI, 3 PMA lock words (last: the riskiest), 4 the one-line WLAN SUMMARY */

void wlan_probe_l1(void);
int wlan_probe_l2_open(void);			/* reads the gate words, prints the gate line; 1 = open. On 0 the caller must not call the groups. */
void wlan_probe_l2_group(unsigned group);	/* re-checks the gate itself and reads nothing if it is closed */
