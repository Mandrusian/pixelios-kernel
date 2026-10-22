#pragma once
#include <stdint.h>

/*
 * usbprobe.h - stage-1 read-only DWC3 USB probe: what state did ABL's fastboot-boot
 * handoff leave the gs101 USB controller in?
 *
 * gs101 addresses (mainline gs101.dtsi, see docs/research/
 * 02-gs101-peripherals.md section 9): usbdrd31_dwc3 core registers at
 * 0x11110000 with the DWC3 global register block at +0xC100, the
 * usbdrd31_phy "phy" register bank at 0x11100000, and the two PMU PHY
 * control words GS101_PHY_CTRL_USB20/USBDP at 0x17463EB0/0x17463EB4.
 * The offsets below follow the Linux dwc3 driver definitions
 * (drivers/usb/dwc3/core.h, fetched 2026-10-20).
 *
 * The macros are exposed because shim/usbprobe_host_test.c keys its
 * fake register window on the same constants, so a host test can prove
 * the probe touches exactly these addresses and nothing else.
 */

#define USBPROBE_DWC3_BASE		0x11110000UL
#define USBPROBE_GCTL_OFF		0xC110u
#define USBPROBE_GSNPSID_OFF		0xC120u
#define USBPROBE_GUSB2PHYCFG0_OFF	0xC200u
#define USBPROBE_GUSB3PIPECTL0_OFF	0xC2C0u
#define USBPROBE_GEVNTADRLO_OFF		0xC400u
#define USBPROBE_GEVNTADRHI_OFF		0xC404u
#define USBPROBE_GEVNTSIZ_OFF		0xC408u
#define USBPROBE_GEVNTCOUNT_OFF		0xC40Cu
#define USBPROBE_DCFG_OFF		0xC700u
#define USBPROBE_DCTL_OFF		0xC704u
#define USBPROBE_DSTS_OFF		0xC70Cu
#define USBPROBE_DALEPENA_OFF		0xC720u

/* Physical endpoint command block n (ep0out = 0, ep0in = 1):
 * DEPCMDPAR2/PAR1/PAR0 then DEPCMD at +0xC of the block. All four are
 * only ever READ here - issuing a command means WRITING DEPCMD. */
#define USBPROBE_DEP_BASE(n)		(0xC800u + (n) * 0x10u)
#define USBPROBE_DEPCMDPAR2_OFF(n)	(USBPROBE_DEP_BASE(n) + 0x00u)
#define USBPROBE_DEPCMDPAR1_OFF(n)	(USBPROBE_DEP_BASE(n) + 0x04u)
#define USBPROBE_DEPCMDPAR0_OFF(n)	(USBPROBE_DEP_BASE(n) + 0x08u)
#define USBPROBE_DEPCMD_OFF(n)		(USBPROBE_DEP_BASE(n) + 0x0Cu)

#define USBPROBE_PHY_BASE		0x11100000UL
#define USBPROBE_PHY_DUMP_WORDS		16u	/* first 0x40 bytes, 4-byte reads */

#define USBPROBE_PMU_PHY_USB20		0x17463EB0UL
#define USBPROBE_PMU_PHY_USBDP		0x17463EB4UL

/* Fault-guarded 32-bit MMIO read shared with shim.c: returns 1 and
 * stores the value on success, 0 when the access faulted (device-side
 * definition in shim.c rides the probe_guard exception skip; the host
 * tests substitute a fake register window). */
int try_read32(uintptr_t address, uint32_t *value);

/* Print the whole USB section into the console/ramlog report. Every
 * register access goes through try_read32 and prints "unreadable" on a
 * fault instead of crashing; if GSNPSID is unreadable the probe skips
 * all further USB reads. STRICTLY read-only: no writes to any USB,
 * PHY, PMU, GDSC, clock or S2MPU register (enforced against this file
 * by tools/test-bluejay-boot.py, check_usb_safety). */
void usbprobe_report(uintptr_t fdt);
