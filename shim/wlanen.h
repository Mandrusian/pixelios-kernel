#pragma once
#include <stdint.h>

/*
 * wlanen.h - opt-in WRITER, stage W2b of docs/research/29-wlan-stage1-design.md: drive WLAN_EN (gpp14-2, the BCM4389 WL_REG_ON) high for 200 ms, sample the
 * pins, and ALWAYS put the pin back (build.sh --wlan-en, implies --wlan-probe, gs101 only). Nothing else is written: no PERST, no PHY isolation, no SMC, no PMIC.
 *
 * wlan_en_run(): refuses with ZERO writes unless the counter runs, the four words GPP14.CON/DAT and GPH2.CON/DAT are readable and
 *   - GPP14.CON field of pin 2 is exactly FUNC_INPUT (0)   [observed run w1: CON 0x33, pin 2 field 0; pinctrl-samsung.h:61-62 PIN_CON_FUNC_INPUT/OUTPUT = 0/1]
 *   - GPP14.DAT bit 2 is 0 (WLAN_EN low)                   [observed w1: DAT 0x3]
 *   - GPH2 pin 0 (PERST) reads low (DAT bit 0 = 0) and its CON field is input (0) or output (1)   [observed w1: input, low]
 * then, in the order of Linux's gpio_direction_output (pinctrl-samsung.c:693-696, samsung_gpio_set_value THEN samsung_gpio_set_direction):
 *   1. GPP14.DAT  := DAT | bit 2          (mask 0x4)
 *   2. GPP14.CON  := CON with field [11:8] := 1 (OUTPUT)   (mask 0xF00)
 * reads both back, waits 200 ms (WIFI_TURNON_DELAY, dhd_custom_google.c:566, dhd.h:3137-3140) on the counter, samples WLAN_EN / PERST / CLKREQ / DEVWAKE /
 * HOSTWAKE and the PMU isolation word, then restores:
 *   3. GPP14.DAT  := saved bit 2 (drive low while still an output: what dhd_wlan_power(0) does, dhd_custom_google.c:616)
 *   4. GPP14.CON  := saved field (input; samsung_gpio_direction_input is the CON field alone, pinctrl-samsung.c:656-676)
 * and reads both back. A failed restore prints `!!! RESTORE FAILED` lines (the pin state is volatile: guess) and `restore=FAILED`. The summary line is
 *   `WLAN EN enable=<ok|refused|fault> sample=<en perst clkreq devwake hostwake as 0/1/?> restore=<ok|FAILED|n/a>`   (n/a: nothing was written)
 * Reads only, before and after: GPP14 PUD/DRV/CONPDN/PUDPDN (+0x08/0x0C/0x10/0x14). Polarity: raw HIGH = ON, the DT flag 0x1 (active-low) is ignored by the legacy GPIO calls of both drivers (guess), do not "fix" it.
 * Every write is w32(): it reads the register, replaces ONLY the bits of its mask and refuses anything else (audited: the two GPP14 registers, masks 0x4 / 0xF00,
 * CON field value 0 or 1). A write that faults or does not read back is `fault`; the restore still runs.
 */
int try_read32(uintptr_t address, uint32_t *value);
int try_write32(uintptr_t address, uint32_t value);
int wlan_en_run(void);
