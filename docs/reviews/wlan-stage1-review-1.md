# Review 1: shim --wlan-rails (A) and --wlan-en (B), commit ce796d7 (2026-10-23)

Reviewer: Claude Opus 5.5 (adversarial, read-only). Nothing was run on hardware. Host-only: `git archive HEAD` into /tmp, then
`shim/build.sh --board bluejay --avb --pstore-fast --usb-gadget --wlan-probe --wlan-rails --wlan-en` (rc 0). Every host test and audit in that
build passed: wlanrails/wlanen host tests, source self-tests 16/16 and 18/18, object self-tests 3/3 and 3/3, and the shim sequence audit.
I then disassembled `wlanen.o`, `wlanrails.o`, `acpm.o` and `shim.elf`. All cites below were re-read in `~/pixelios-artifacts/refs/wlan-stage1/`.
Process note: author (Sonnet) and reviewer (Opus) are the same model family. AGENTS.md rule 6 asks for a different one; the driver decides.

## Verdict A (--wlan-rails): GO
## Verdict B (--wlan-en): GO-WITH-FIXES (B1 is one attribute; B2 is doc tagging; nothing blocks on hardware facts)

## (1) Exact write set, from the disassembly
- **A**: `wlanrails.o` has no `try_write32` symbol. It has 69 stores, all to the stack. Its undefined symbols are `try_read32`, `acpm_cfg_init/describe/why`, `acpm_s10_read`, `con_str` and `counter_freq`.
  `acpm_s10_read` (acpm.o 0xf80) compiles to a range check `reg - 0x53 <= 3`, then `xfer(c, w0 = reg | pmic_addr<<8 | speedy<<12, w1 = 0 (FUNC_READ), 0, 0)`.
  That is byte for byte the same pattern as `acpm_pmic_read` (both `mov w1,#0` before `bl xfer.constprop.0`). No call site in the linked acpm.o can form FUNC_UPDATE:
  `nm` shows no `acpm_pmic_update`, `acpm_s11_rail_set`, `acpm_dvfs_set` or `acpm_dvfs_reassert`. The only stores the ACPM channel can make are the
  already-audited guarded ones (`w32_allowed`: slot, front, doorbell, rear, INTCR0). The CMU/PMU reads are `try_read32` only. The S2MPG10 registers
  0x53..0x56 = L16M..L19M_CTRL (s2mpg10-register.h:99-102). Mask 7:6, 64 steps, groups 3/4 (regulator.c:257-264, register.h:313-320) match `decode()`.
- **B**: `try_write32` has exactly one `bl`, at w32+0x60, inside `w32`. There are four `bl w32` sites, and their x0 is a movz/movk constant:
  0x6e8 DAT 0x108401C4 (bits 0x4), 0x730 DAT (saved), 0x778 CON 0x108401C0 (saved), 0x9cc CON (0x100). The CON path writes `(old & ~0xF00) | (bits & 0xF00)` and
  refuses when `v & 0xE00` (field > 1). The DAT path writes `(old & ~4) | (bits & 4)`. In this binary the writes are only GPP14.DAT bit 2 and the GPP14.CON
  field [11:8] (value 0 or 1).

**B1 (M) The compiled `w32` has lost its own address check.** `w32` is `noinline` but not `noipa`, so GCC specialised it for the constant callers.
If `pa != EN_CON` it now reads 0x108401C4 and then writes to the caller's `pa` (x2) without checking it. The `wmask(pa)==0 -> -2` refusal is gone from the object.
Today this is safe, because the four call sites are constants and the source audit pins them. The object audit, though, only checks "try_write32 only
inside w32", so the allowlist the docs describe is not present in the shipped code. Fix: add `__attribute__((noipa))` to `w32` (as `put_txt` already has),
or have the object audit check that every `bl w32` loads x0 = 0x108401C0/0x108401C4. The `noipa` fix is the smaller one; the object audit check is stronger. Do one before the run.

## (2) GPIO semantics (read from source)
- Bank: gpp14 = `EXYNOS850_PIN_BANK_EINTG(4, 0x1c0, "gpp14")` in `gs101_pin_peric0` (pinctrl-exynos-arm64.c:988), pinctrl@10840000 (gs101-pinctrl.dtsi:383).
  The bank has 4 pins. `exynos850_bank_type_off` (:47-50) gives CON +0x00 at 4 bits/pin, DAT +0x04 at 1 bit/pin, PUD +0x08, DRV +0x0C, CONPDN +0x10, PUDPDN +0x14.
  `samsung_gpio_set_direction` uses shift = offset * 4, so pin 2 is CON[11:8] (mask 0xF00) and DAT bit 2 (mask 0x4). Correct.
- Observed CON 0x33: pins 0 and 1 are in func 3 = `hsi2c7_bus` (gs101-pinctrl.dtsi:689); pins 2 and 3 are func 0 = input. This also corroborates the bank address on hardware.
  Pin 3 is BT `dev_wake` (common-bluetooth.dtsi:23). DAT 0x3 means the two I2C lines read high and WLAN_EN and BT_DEV_WAKE read low.
  The DAT read-modify-write rewrites bits 0, 1 and 3 with their read levels. That has no effect on function-mode or input pins, and Linux's `samsung_gpio_set_value` (:555-570) does the same read-modify-write.
- Order: DAT then CON, the same as `samsung_gpio_direction_output` (:693-696). While the pin is still an input the DAT write has no electrical effect, so the switch is glitch-free.
  The restore sets DAT low while the pin is an output, then switches it to input. That drives the module off before the pin is released.
- Polarity: DT `<&gpp14 2 0x1>` has flag 0x1 = GPIO_ACTIVE_LOW, but both consumers ignore it. They use the legacy `of_get_named_gpio` and raw values:
  dhd sets it high for on (`gpio_request_one(GPIOF_OUT_INIT_HIGH)` and `gpio_direction_output(.., 1)`, dhd_custom_google.c:542,557,601) and low for off (:616).
  The PCIe RC drives it low at probe (pcie-exynos-rc.c:1989-1994). So physical high = on is read from source. Pinctrl state `cfg_wlanen` (common-wlan.dtsi:14-21) is:
  OUTPUT, PUD none, DRV 2.5 mA, CONPDN PREV, PUDPDN pull-up. There is no regulator-fixed and no gpio-hog for wlan_en.
  The shim does not set PUD or DRV. That is acceptable for a CMOS enable input (*guess*), but see B3.
- Claim "the DAT latch of an input pin is not observable": Linux `samsung_gpio_get` (:592-611) reads DAT whatever the direction, so the source neither proves nor
  refutes the claim. It is Samsung UM behaviour, i.e. a *guess*. It is harmless here: the precondition only needs the *pin* to be low, and the restore writes DAT unconditionally.

## (3) Powering the module with PERST low and the PHY isolated
This is the stock Android boot state (read from source). The RC probe sets WLAN_EN low. dhd init then drives it high and waits 200 ms (dhd_custom_google.c:542-566).
The link is started only later, by carddetect, through `exynos_pcie_pm_resume` (:657; PERST release in rc poweron). No DT clock or regulator is touched before WLAN_EN.
The 32 kHz LPO and the module supplies are not handled by Linux at all (research 29 U3), so stock Android runs this step with whatever the board provides.
The shim skips Linux's "output low" step (S02). Input reading 0 to output high is equivalent. HOSTWAKE, DEVWAKE and CLKREQ stay AP inputs, so nothing can contend.
If PERST (gph2-0, an input) floats up once the module powers, link training against an isolated PHY cannot complete. That is harmless (*guess*), and the PERST level is sampled.

## (4) Restore path and failure branches
Every branch after the first write attempt reaches `restore()`. The CON restore (to input) is attempted even when the DAT restore faulted, which is right:
an input is not driven. A failed restore prints two `!!!` lines. Between enable and restore the only waits are the counter (bounded) and `try_read32` on words that W1 read without a hang (observed).
Left high after a hang, the module is in the same state as Android with Wi-Fi on. The next ABL/Android boot re-drives the pin itself (dhd checks the value, :551-561). Not harmful.
Whether a SoC reset clears a PERIC0 non-alive pin is *unverified*: pad retention is a sleep feature, not a reset one (*guess*).

## (5) Audits
`git diff HEAD~5 HEAD -- tools/test-bluejay-boot.py`: +249/-0, so nothing was loosened. The source audits are strong: exact allowlist bodies, four ordered call sites,
refusals before the first write, a loud unconditional restore, and no loops. The object audit for B is weaker than its message suggests (B1).

## Other findings
- **B2 (L) Untagged claims; tag them *guess*.** `wlanen.c:257` and AUTOMATION.md: "pin state does not survive a reset". `wlanen.c:168`: "the DAT latch ... is not observable".
  AUTOMATION.md: "Expected quiet result: sample `10100`". Also add one line explaining that the DT flag 0x1 is ignored, so nobody "fixes" the polarity later.
- **B3 (L)** Add a read-only "before" line for GPP14 PUD/DRV/CONPDN (+0x08/+0x0C/+0x10). This records what the pin floats to after the restore, and the drive used for the high level.
- **A1 (L)** `acpm_s10_read` was inserted between the `acpm_s11_rail_set` doc comment (acpm.c:549-557) and its code, so that comment now sits above the wrong function.
- **A2 (L, optional)** For U3, `--wlan-rails` could also read S2MPG10 RTCBUF 0x11 (register.h:43), the 32 kHz buffer candidate, as a raw value. Its bit meaning is unknown, so print it without a decode.
