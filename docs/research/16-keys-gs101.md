# 16. Hardware buttons on bluejay (gs101): xnu patch 0087

Goal: power, volume up and volume down reach SpringBoard through the existing `sweet_hid_key(key, down)` hook
(`iokit/Kernel/arm/hid/SweetHID.cpp`, patch 0058; 0 power, 1 vol+, 2 vol-, 3 home). The Redmi's SPMI/PMIC polling
driver (archived `xnu/patches-archive/0060-sweet-keys-touch-drivers.patch`) is Qualcomm-only and not reused.
Tags: **[obs]** observed on hardware, **[src]** read from source (file:line), **[guess]**.

## Design

- `bsd/dev/sweet_keys_gs101.c` (new, patch 0087). One kernel thread. After `sweetkeysdelay` seconds (default 30) it maps the
  GPIO pages named by the ADT, logs the raw DAT word of each pin once, then samples every tick (10 ms at hz=100) and
  debounces (3 consecutive samples). Each accepted edge is logged and passed to `sweet_hid_key()`.
- Read-only. The driver never writes a pin register: no CON, PUD or DRV change.
- The thread sleeps with `tsleep()`, the form `sweet_log_thread` uses and which wakes on bluejay [obs: the persistent log
  works]. The comment in `bsd/dev/sweet_restart.c` (patch 0083/0086) records that a dedicated `assert_wait_deadline` thread did not fire in
  3 of 4 runs (cause unknown), so the old driver's wait primitive was not copied [obs, cause guess].
- Pure logic in `bsd/dev/sweet_keys_gs101_debounce.h` (`keys_raw_down`, `keys_deb_step`). The first sample only sets the
  reported level, so a key held at start, or a wrong polarity guess, emits no press. `shim/keys_gs101_host_test.c` compiles
  the header extracted from the patch itself (`shim/build.sh`, run by the gate's host-tests step), so the tested text is the
  shipped text.
- All constants live in data, none in the C:
  - `device/google/bluejay/board.json` `key_pins` = `[hid index, pin-controller base, bank offset, pin]` and `key_active_low`.
  - `soc/google/gs101/soc.json` `gpio_alive_dat_off` = `0x4`.
  - `tools/mkadt.py` writes `/sweet-keys` (`compatible google,gs101-gpio-keys`, `keys` = u32 triples hid/DAT-PA/pin,
    `active-low`) only for boards defining `key_pins`. The sweet ADT has no such node (checked: 0 occurrences) and
    `sweet_keys_gs101_start()` returns silently without it.
- Hook: `sweet_ufs_init()` calls `sweet_keys_gs101_start()` after `sweet_usb_probe()`; `bsd/conf/files` gets the new file.
- Boot-args: `sweetkeys=0` disables, `sweetkeysdelay=<s>` (default 30).

Resulting ADT (`tools/adt.py dump /tmp/k.adt --hex`):
`keys = <0x0 0x174d00e4 0x1  0x1 0x174e0044 0x1  0x2 0x174e0024 0x3>`, `active-low = <1>`.

## Facts

| Fact | Tag | Source |
|---|---|---|
| vol-down gpa7-3, vol-up gpa8-1, power gpa10-1, all `GPIO_ACTIVE_LOW` | [src] | `gs101-pixel-common.dtsi:40-65` (Linux master); Google DS `gs101-common.dtsi:46-68` (bluejay includes it); docs/research/05 section 2 |
| gpa7, gpa8 in `pinctrl_far_alive` at `0x174e0000`; gpa10 in `pinctrl_gpio_alive` at `0x174d0000` | [src] | `gs101.dtsi` (`pinctrl@174d0000`, `pinctrl@174e0000`, v6.12 lines 1414, 1427); docs/research/05 section 2 |
| gpa10 pctl_offset 0xE0 (2 pins) in the ALIVE table | [src] | `drivers/pinctrl/samsung/pinctrl-exynos-arm64.c:941-950` (v6.12) |
| gpa7 pctl_offset 0x20 (4 pins), gpa8 0x40 (8 pins) in the FAR_ALIVE table | [src] | same file `:953-958` |
| Bank type for both: `exynos850_bank_type_alive`, CON +0x00 (4 bit/pin), DAT +0x04 (1 bit/pin), PUD +0x08, DRV +0x0c | [src] | `pinctrl-exynos-arm64.c:56-59`; macro `EXYNOS850_PIN_BANK_EINTW` `pinctrl-exynos.h:134-142` |
| A pin level is `(readl(bank + pctl_offset + DAT) >> pin) & 1` | [src] | `pinctrl-samsung.c:592-611` (`samsung_gpio_get`) |
| So DAT words: power `0x174D00E4`, vol+ `0x174E0044`, vol- `0x174E0024` | [src] | derived from the rows above |
| Linux muxes all three as EINT (`GS101_PIN_FUNC_EINT`), `GS101_PIN_PULL_NONE`, 2.5 mA, and gpio-keys reads the level with the pin in that function | [src] | `gs101-pixel-common.dtsi:798-853` (`key_voldown`, `key_volup`, `key_power`) |
| Read-only DAT is therefore enough: Linux does not configure a pull either, so the board has external pull-ups [guess: the dtsi sets PULL_NONE and Linux uses the same DAT read; no schematic seen] | [guess] | as above |
| The far-alive bank `0x174E0000` is readable from the shim/EL1 without a fault | [guess]: no shim code reads either bank (shim/wlanprobe.c:16-17; the `--wlan-probe` run is not done), so the first read is the first-ever one | docs/research/12-wifi-bluejay.md:89,316 |
| The gpa10/`0x174D0000` bank is readable the same way | [guess] | both are APM-clocked blocks (`gs101.dtsi` `CLK_GOUT_APM_APBIF_GPIO_*_PCLK`) |
| Pin numbers, banks and polarity are those of the Oriole/Raven dtsi and of the bluejay downstream DS | [src] | docs/research/05 section 2. No bluejay schematic or live read has confirmed them |

I fetched the Linux files from raw.githubusercontent.com (tags v6.12 and master); the line numbers above are from those
copies.

## Unknowns only a hardware run answers

1. Does the power pin really live in `gpio_alive` (`0x174D00E4` bit 1), or does the 6a differ from the Oriole/Raven dtsi
   (the 05 fact check already found one wrong bank assumption)?
2. Polarity: expect idle DAT bit = 1, pressed = 0. The `start` log prints the idle bit; each press prints the DAT word.
3. Does ABL leave the three pins as inputs/EINT with the pad enabled? If DAT never changes on a press, a pin is probably not
   in an input function and a CON read (bank + 0x0) is the next step. A CON write is not to be added without that read [guess].
4. Does reading `0x174D0000` hang if the APM GPIO pclk is gated? Linux enables `pclk` around every access
   (`pinctrl-samsung.c:603`); the shim has not been seen to read this bank [unverified].
5. Whether the debounce window (30 ms) and the hz=100 tick suit the real contact bounce.

## Run plan (needs the numbered patch built by CI, then the usual image)

1. CI builds the kernel with patch 0087; `tools/mkadt.py --board bluejay` for the ADT (node present).
2. `tools/build-xnu-image.sh` currently passes `sweetkeys=0` in its default boot-args, which now disables this driver too.
   Remove it from the default list (it only ever guarded the Qualcomm driver) or pass a modified list; `--args
   "sweetkeys=1"` appended after it probably does not win, because the first match is the one parsed [guess].
3. Boot with `sweetkeysdelay=30` (default). Read with `tools/qr-log.py` or the persistent UFS log:
   `SWEET KEYS: 3 keys, active-low=1, first read in 30 s`, then per key
   `SWEET KEYS: start key=<n> pa=0x... pin=<p> dat=0x... down=<0|1>`. Idle keys must print `down=0`; a `down=1` at
   rest means wrong polarity or pin.
4. Press and release each key once, with the screen on SpringBoard: expect `SWEET KEYS: key=<n> down=1/0 dat=0x...`
   per edge. Power should lock/wake, vol keys change volume (HID side is patch 0058's job and has its own result).
5. If a line is missing or unchanged, rerun with `sweetkeysdelay=5` and compare the idle and pressed DAT words of the
   whole 32-bit register to see which bit moved.
6. Record date and build id in `docs/HANDOFF.md`. Until then every statement above is [src]/[guess], not "works".
