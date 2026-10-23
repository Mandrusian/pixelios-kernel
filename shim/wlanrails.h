#pragma once

/*
 * wlanrails.h - READ-ONLY rail / clock / PMU state for the BCM4389 + PCIe1 power-up design (build.sh --wlan-rails, implies --wlan-probe, gs101 only;
 * docs/research/29-wlan-stage1-design.md section 4, step W2-R). Nothing is written: no try_write32, no SMC, no PMIC write.
 *
 * wlan_rails(), in this order (a stop never skips the later independent parts):
 *  1. CMU_HSI2 gate: the UFS HCI VER word (UFS_BASE + 0x08, ufs.c REG_VER; observed ver=0x300, hardware/ufs-gpt-read-2026-10-21.txt:273) must be readable and
 *     nonzero, i.e. the HSI2 block the UFS controller sits in answers. Only then are the CMU_HSI2 words read (PCIE user mux +0x620, OSCCLK gating +0x800,
 *     QCH debug words GPIO +0x7018, PCIE_GEN4A +0x7060, PCIE_GEN4B +0x7064, UFS +0x7068 (the reference: UFS works), SYSREG +0x707C), printed `WLAN CMU <name> pa= v=`
 *     with decoded lines tagged (guess) where the layout is not from source.
 *  2. PMU +0x3C10 (TXCO always-on bits 2,3,10 = rc.c:80,2698; PMU alive reads work from the shim, observed PMU 3ec8) `WLAN PMU TXCO pa= v=` + decode.
 *  3. The S2MPG10 PCIe PHY rails LDO16M..LDO19M (regs 0x53..0x56) through the audited ACPM client (acpm_s10_read; refuses with zero ACPM traffic unless the
 *     ACPM gates, the channel descriptor and empty queues check out, exactly like gpu_rails): `WLAN RAIL <name> reg=0x.. v=0x..` + `WLAN RAIL decode ...`.
 *  4. One summary line `WLAN RAILS l17=<mode>/<uV> l19=<mode>/<uV> cmu_mux=<b> txco=<b>`.
 * Returns 0 if the rail registers were all read.
 */
int wlan_rails(void);
