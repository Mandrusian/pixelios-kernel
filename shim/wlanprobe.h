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
 * l2 (PCIe wrapper, may hang the bus on a gated block; the caller saves
 * pstore between them). The outbound config window is never touched.
 */

/* Samsung pinctrl bank: CON is 4 bits per pin (1 = output), DAT 1 bit per pin. */
static inline uint32_t wlan_gpio_func(uint32_t con, unsigned pin)
{
	return (con >> (4u * pin)) & 0xfu;
}

static inline uint32_t wlan_gpio_level(uint32_t dat, unsigned pin)
{
	return (dat >> pin) & 1u;
}

/* Fault-guarded 32-bit MMIO read (shim.c; host tests substitute a fake window). */
int try_read32(uintptr_t address, uint32_t *value);

void wlan_probe_l1(void);
void wlan_probe_l2(void);	/* also prints the one-line WLAN SUMMARY */
