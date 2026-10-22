# 05 – Fact check of 01/02/CASE_STUDY claims (gs101 / bluejay)

Desk research only (2026-10-20). No adb/fastboot/device commands were run.
Every verdict cites a primary source (file and line, or commit). Where only
secondary or community evidence exists, the verdict says so.

Sources and abbreviations:

- **ML** = Linux mainline, `torvalds/linux` master as fetched 2026-10-20
  (`arch/arm64/boot/dts/exynos/google/`, `drivers/soc/samsung/`,
  `drivers/ufs/`, `drivers/watchdog/`).
- **DS-gs** = Google downstream kernel `android.googlesource.com/kernel/gs`,
  branch `android-gs-bluejay-5.10-android14-qpr1`
  (sha `ebc441967af0637888ccb51502855ce76671a726`).
  The base DT is under `arch/arm64/boot/dts/google/`.
- **DS-bj** = `kernel/devices/google/bluejay`, same branch name
  (sha `05a27022a31c8a1412346980688ae7f718c16648`).
  This holds the bluejay DT *overlays* (`dts/`) and the panel module (`display/`).
  `build.config.bluejay` builds `google-base/gs101-{a0,b0}.dtb` plus
  `dtbo.img`, so the board DT is base DTB + overlay chosen by `board_id`.
- **DS-disp** = `kernel/google-modules/display`, branch `android-gs-bluejay-5.10-android14-qpr1`
  (sha `f806f9f24145504d065e53ffa696963300016f6e`).
- **DS-fts** = `kernel/google-modules/touch/fts_touch`, same branch
  (sha `287ec4db6c97597b11ff55f85d771ea521899e8b`).

## Summary

| # | Claim | Verdict |
|---|---|---|
| 1 | Touch = STMicro FTM5 over SPI | **CONFIRMED** (chip, bus). **WRONG** in 02: rails are `LDO25M`/`LDO26M` of S2MPG10, not "vci/vddi". Details below |
| 2 | `gs101-pixel-common.dtsi` exists in mainline; gpio-keys gpa7-3 / gpa8-1 / gpa10-1 | **CONFIRMED** (mainline and bluejay downstream). **WRONG** in 02: the three pins are not all in the 0x174d0000 bank |
| 3a | Bluejay fb = 0xfac00000, 1080x2400, stride 4320, a8r8g8b8 | Oriole/Raven values **CONFIRMED** in mainline. Bluejay: **UNVERIFIABLE** from primary sources (no bluejay splash node anywhere). Community bluejay DT copies the Oriole values |
| 3b | DECON HW trigger register | **CONFIRMED** (derived from Google source plus postmarketOS): DECON0 `0x1C300000`, `TRIG_CON` +0x30, `HW_TRIG_EN` = bit 0, postmarketOS writes `0x3061`. The Linaro cover letter names **no** register. "Later bootloaders changed it": **UNVERIFIABLE** (no sign of a change) |
| 3c | Bluejay panel S6E3FC3, command or video? | S6E3FC3 **CONFIRMED**. **Command mode** (TE-triggered, DSC) **CONFIRMED** from Google source |
| 4 | CNTFRQ 24.576 MHz, virt timer PPI 11 = INTID 27, GICD `0x10400000` / GICR `0x10440000` | **CONFIRMED** |
| 5 | CPU hotplug pre-steps | Offsets **CONFIRMED**, but 02's framing is **WRONG**: the hang is documented for *hotplug re-online*, not first boot. PMU writes are **SMC** (`0x82000504`), `pmu-intr-gen` is plain MMIO. First `CPU_ON` at boot needs neither (inferred from the code, see below) |
| 6 | UFS | `UFSHCD_QUIRK_PRDT_BYTE_GRAN` and `0x14700000` **CONFIRMED**. Link left up by the bootloader: **UNVERIFIABLE**. No SMC is needed for plain I/O (inferred; only FMP/crypto uses SMCs). Residual risk: 128-byte PRDT entries if firmware set DESCTYPE=3 |
| 7 | WDT | Bases **CONFIRMED**. "Bootloader leaves them running": **UNVERIFIABLE**. 02's disable recipe is **INCOMPLETE** (PMU SMC bits and a clock gate matter) |
| 8 | Debug UART | `fastboot oem uart enable` **CONFIRMED** (AOSP doc, Pixel 6a write-up). Base `0x10a00000` **CONFIRMED**. SBU-to-UART instance mapping: **UNVERIFIABLE** in DT (only "SBU, 1.8 V, 115200 default, `uart config` can change it") |
| 9 | `fastboot boot` with an unsigned custom boot.img on 6a | **UNVERIFIABLE** for bluejay. Oriole: community evidence that `fastboot boot` of a custom image works. Details below |
| 10 | A10 iPadOS 18 DSC is plain arm64 | **UNVERIFIABLE** directly (no IPSW cache listing found). Strong indirect evidence only |

---

## 1. Bluejay touchscreen

**Verdict: CONFIRMED part and bus. 02's regulator claim is WRONG.**

Evidence:

- Overlay `DS-bj dts/gs101-bluejay-touch.dtsi:6-16` (used by `pvt1_0`, `mp1_0`,
  `dvt1_0`, `evt1_*`, `proto1_*`; `gs101-bluejay-dev.dts` uses `-touch-dev.dtsi`).
  It includes `google-base/gs101-fts-touch.dtsi`, sets `compatible = "st,fts"`
  and the following:
  - `spi-max-frequency = <10000000>`
  - `st,dchip_id = <0x36 0x48>`
  - `st,max-coords = <1079 2399>`
  - `st,firmware_names = "ftm5_fw_b3.ftb"`
  - `st,panel_map = <&samsung_s6e3fc3_6a 0>`
- Chip id: `DS-fts ftm5/fts_lib/ftsHardware.h:28-29`:
  `ALIX_DCHIP_ID_0 0x36 /* LSB chip ID for FTM5 */`, `ALIX_DCHIP_ID_1 0x48`.
  So `<0x36 0x48>` is ST "ALIX" = **FTM5**.
- Module: `DS-fts ftm5/Makefile`: `obj-$(CONFIG_TOUCHSCREEN_FTS) += ftm5.o`.
  Built via `DS-bj Makefile.ext_modules.bluejay:26` (`ext_modules += touch/fts/ftm5`).
  Driver binds `compatible = "st,fts"` in `ftm5/fts.c:7185`, as an **`spi_driver`** (`fts.c:7209`).

| Item | Value | Source |
|---|---|---|
| Bus | **SPI**, `spi_11` | `DS-gs gs101-common-touch.dtsi:8,106-139` |
| USI | **USI11** (`USI11_USI_PERIC1`) | `DS-gs gs101-usi.dtsi:603-621` |
| SPI engine base | `0x10D40000`, size 0x1000. The same window is `hsi2c_11` and `serial_11`; USI mode is selected in sysreg | `gs101-usi.dtsi:317,603,878` |
| USI mode register | `sysreg_peric1` (`0x10C21000`) + `samsung,usi-offset 0x0c` = `0x10C2100C` | `gs101-usi.dtsi:66,618` |
| SPI pins | CLK/MOSI/MISO = `gpp23-0..2` (func 2) | `gs101-common-touch.dtsi:65-70` |
| SPI CS | **GPIO CS** `gpp23-3` (`cs-gpios`, `cs-gpio`) | `gs101-common-touch.dtsi:73-77,113`; `gs101-fts-touch.dtsi:11-14,34` |
| IRQ GPIO | `st,irq-gpio = <&gpa9 0 0>` (XEINT_20, GPIO_ALIVE bank) | `gs101-fts-touch.dtsi:23`; header `gs101-common-touch.dtsi:11` |
| Reset GPIO | `st,reset-gpio = <&gpp26 0 0>` (peric1 bank) | `gs101-fts-touch.dtsi:24` |
| Bus negotiator | Touch SPI is shared with AOC: `ap2aoc = gpp26-1` ("TS_SPI_SEL", low = AP, high = AOC), `aoc2ap = gpa8-6` | `gs101-common-touch.dtsi:12-20,40-62` |
| Power | `vdd-supply = <&m_ldo25_reg>` = **S2MPG10 LDO25M**, 1.6-1.95 V, "L25M_TSP_DVDD". `avdd-supply = <&m_ldo26_reg>` = **LDO26M**, 2.5-3.3 V, "L26M_TS_AVDD" | `gs101-common-touch.dtsi:126-127`; `gs101-pmic.dtsi:535-553` |

Corrections to 02:

- 02 §7 says the rails are "`vci` (AVDD) and `vddi`". That is wrong. `vci`/`vddi` are
  the *panel* rails (`DS-bj gs101-bluejay-display.dtsi:22-24`: panel `vci-supply =
  <&m_ldo22_reg>`, fixed 3.025 V at `:68-72`, `vddi` deleted). The touch rails are LDO25M/LDO26M.
- Both are on the master PMIC S2MPG10, reachable only via ACPM. 02's conclusion
  (ACPM needed if ABL powers them down) stands.
- Mainline has **no** `spi_11`/`usi11` node (mainline `gs101.dtsi` only goes to
  the `peric0` USIs and a few `peric1` ones). Mainline `gs101-pixel-common.dtsi:403,410`
  only has "touch" comments. All touch facts come from Google's downstream tree.
- The IRQ trigger type is not given in the downstream DT (the driver only reads `st,irq-gpio`); I did not check the driver's `request_irq` flags.
- A TBN/AOC mux (`gpp26-1`) means the AP may not own the SPI bus at handoff. Not verifiable without a device.

## 2. `gs101-pixel-common.dtsi` and gpio-keys

**Verdict: CONFIRMED. 02's bank address is partly WRONG.**

- Exists in mainline: `arch/arm64/boot/dts/exynos/google/gs101-pixel-common.dtsi`.
  Introduced by "arm64: dts: exynos: gs101-oriole: move common Pixel6 & 6Pro parts
  into a .dtsi" (krzk tree commit `58dbafb73173a944657c4a0d480d2cce50bff2bd`,
  part of Draszik's v4 series). The Makefile builds only `gs101-oriole.dtb` and
  `gs101-raven.dtb`. **There is no `gs101-bluejay` in mainline.**
- gpio-keys, `gs101-pixel-common.dtsi:40-63`:
  - `button-vol-down` `<&gpa7 3 GPIO_ACTIVE_LOW>`
  - `button-vol-up` `<&gpa8 1 GPIO_ACTIVE_LOW>`
  - `button-power` `<&gpa10 1 GPIO_ACTIVE_LOW>`
  - pinctrl at `:799-853`: `key_voldown`/`key_volup` in `&pinctrl_far_alive`,
    `key_power` in `&pinctrl_gpio_alive`.
- Bluejay downstream: `DS-gs gs101-common.dtsi:46-68` (bluejay includes
  `google-base/gs101-common.dtsi`, `DS-bj gs101-bluejay.dtsi:7`) has the same three:
  `button@1 gpios <&gpa7 3 0xf>` (code 114), `button@2 <&gpa8 1 0xf>` (115),
  `button@3 <&gpa10 1 0xf>` (116). Bluejay overlays do not override them.
  (The `0xf` flag word includes the active-low bit; mainline normalises it to `GPIO_ACTIVE_LOW`.)

Correction to 02 §8: it says the keys are all in the Alive bank at `0x174d0000`.
Mainline `gs101.dtsi`: `pinctrl_gpio_alive` = `0x174d0000` (`gpa10` and
`gpa9`/`gpa0` live here) but `pinctrl_far_alive` = **`0x174e0000`** (`gpa7`, `gpa8`
live here: `gs101-pinctrl.dtsi:151-206`). So vol-down/vol-up are read from
`0x174e0000`, the power key from `0x174d0000`.

## 3. Framebuffer, DECON trigger, panel

### 3a. Framebuffer address, size, stride, format

**Verdict: Oriole/Raven CONFIRMED. Bluejay UNVERIFIABLE from primary sources.**

- Mainline `gs101-pixel-common.dtsi:97-103`: `cont_splash_mem: splash@fac00000`
  `reg = <0x0 0xfac00000 0x0>; no-map` ("size to be updated by actual board").
  The simplefb node `framebuffer0` (`:28-36`) says "format properties to be
  added by actual board". The 0xfac00000 base is therefore the *common gs101
  Pixel* base in mainline.
- `gs101-oriole.dts:36-48`: size `1080 * 2400 * 4` = 0x9E3400 (10,368,000 B),
  `width 1080 height 2400 stride (1080*4)=4320 format "a8r8g8b8"`.
  `gs101-raven.dts`: `1440 * 3120 * 4`, stride 5760.
- Bluejay: the downstream bluejay/gs DT has **no** splash `reserved-memory`
  node (grepped `gs101*.dtsi` and `DS-bj dts/*` for `splash|fac00000`: no hits).
  The ABL places the framebuffer itself, so no Google source gives bluejay's address.
  The community bluejay DT `gs101-mainline/linux@d28cfa4` `gs101-bluejay.dts`
  just reuses the Oriole values (`0xfac00000`, `1080*2400*4`, `a8r8g8b8`). A
  postmarketOS-for-6a project (`friedrice-sourcecodes/pixel6a_fix_pmosmainline`,
  AI-assisted, secondary) claims display works with `simplefb` on bluejay.
  That is circumstantial support only.
- The Linaro series never covered bluejay: v2 mentioned "Pixel 6 / 6 Pro / 6a" in
  its changelog, v3+ split into oriole and raven only (lkml
  `20250117-gs101-simplefb-v4-0-a5b90ca2f917@linaro.org`).

Conclusion: keep `0xfac00000`, 1080x2400, stride 4320, BGRA/a8r8g8b8 as the
working hypothesis. Verify on the device (read the DECON window/channel address
registers) before trusting it. It is not established for bluejay.

### 3b. The DECON hardware-trigger issue

**Verdict: Register facts CONFIRMED. The Linaro series itself names no register.**

- What Linaro says (cover letter of `[PATCH v4 0/4] Google Pixel 6 (simple)
  framebuffer support`, Andre Draszik, 2025-01-17; applied by krzk: `befbb62c61a5`
  simplefb node, `58dbafb73173` common dtsi, `825c4bfd5c52` raven): *"The stock
  bootloader disables the decon hardware trigger before jumping to Linux,
  preventing framebuffer updates from reaching the display. We have added a
  work-around in our Yocto BSP layer ..."* No register, offset or value is given.
  Peter Griffin's Tested-by on the first posting (Dec 2024): "Tested on Oriole device, with latest initramfs
  and framebuffer was functional".
- The actual register write (secondary but real code): postmarketOS
  `pmaports@v26.06` `device/testing/soc-google-gs101/00-enable-fb.sh`
  (added in commit `d5765b86d915`, 2025-09-27, "soc-google-gs101: new aport"):
  ```
  # Write to decon register to enable framebuffer refreshing
  /usr/bin/devmem2 0x1c300030 w 0x3061
  ```
  `device-google-bluejay` depends on `soc-google-gs101`
  (`device/testing/device-google-bluejay/APKBUILD`), and so do oriole and raven. So the same hook is
  used on all three; for bluejay this is packaging evidence, not a measured
  register read on a 6a.
- Decoding with Google's header. DECON0 main region is `0x1C300000`
  (`DS-gs gs101-drm-dpu.dtsi:235-242`, `reg-names "main"`). `TRIG_CON = 0x0030`
  (`DS-disp samsung/cal_9845/regs-decon.h:97`). Bits (`regs-decon.h:98-116`):

  | Bit | Name | In 0x3061 |
  |---|---|---|
  | 0 | `HW_TRIG_EN` (**the hardware trigger enable**) | 1 |
  | 4 | `HW_TRIG_MASK_DECON` | 0 (trigger unmasked) |
  | 5 | `HW_TRIG_MASK_SLAVE0` | 1 |
  | 6 | `HW_TRIG_MASK_SLAVE1` | 1 |
  | 12 | `HW_TRIG_EDGE_POLARITY` | 1 |
  | 13 | `HW_TRIG_ACTIVE_VALUE` | 1 |
  | 25:24 | `HW_TRIG_SEL` | 0 = TE from DDI0 |

  Google's driver clears `HW_TRIG_MASK_DECON` and sets `HW_TRIG_EN` to unmask
  (`DS-disp cal_9845/decon_reg.c:2063-2081`) and selects DDI0 via `te_from = <0>`
  (`DS-gs gs101-drm-dpu.dtsi:192`, `decon_reg.c:595-615`). 0x3061 matches
  exactly; the write is a whole-word write, not read-modify-write.
- Minimal ask for the shim: set bit 0 (`HW_TRIG_EN`) of `0x1C300030`, clear bit 4.
  Writing the full pmOS value `0x3061` is the known-working form. Whether
  bits 5/6/12/13 are already set by the ABL is unknown. Prefer a read first
  (read-only, still a device command, so not done here).
- Later bootloaders: **UNVERIFIABLE**. No source describes a change. The
  postmarketOS hook was added Sept 2025 and still ships in v26.06 (2026), so
  recent Pixel 6 bootloaders still need it. For a 6a on an old or newer ABL,
  check whether `TRIG_CON.HW_TRIG_EN` reads 0 at handoff.
- The omarchy-mobile project uses `0x1c240030 0x3061` for **cheetah (gs201)**
  and says to read gs101's register on a booted phone before ever writing it.
  The gs101 address is 0x1c300030, not 0x1c240030.

### 3c. Bluejay panel

**Verdict: S6E3FC3, COMMAND mode CONFIRMED.**

- `DS-bj dts/gs101-bluejay-display.dtsi:12-25`: `compatible =
  "samsung,s6e3fc3_6a"`, `reset-gpios = <&gpp24 1 ...>`, `vci-supply = <&m_ldo22_reg>`.
  Also `panel@2` `samsung,sofef01` (alternate DDI) and `samsung,emul`; the touch
  dev overlay (`-touch-dev.dtsi`) lists both panel maps. `vendor_boot_modules.bluejay`
  lists `panel-samsung-s6e3fc3_6a.ko` and others.
- Panel driver `DS-bj display/panel-samsung-s6e3fc3_6a.c:484-536`: 1080x2400,
  `.clock = 168498` (60 Hz), 4 lanes (`:603`), `exynos_mode.mode_flags =
  MIPI_DSI_CLOCK_NON_CONTINUOUS` (**no `MIPI_DSI_MODE_VIDEO`**), **DSC enabled**
  (2 slices, `slice_height 48`), `vblank_usec 120`, TE2 timing.
- Command vs video: `DS-disp samsung/exynos_drm_dsim.c:1080`: `config->mode =
  (mode_flags & MIPI_DSI_MODE_VIDEO) ? DSIM_VIDEO_MODE : DSIM_COMMAND_MODE`;
  `exynos_drm_decon.c:320-330`: not video and no sw_trigger => `DECON_COMMAND_MODE`
  + `DECON_HW_TRIG` with `te_from`. TE pin = `gpp0-3` (`gs101-drm-dpu.dtsi:193`).
  So the 6a panel is **command mode, TE-driven, DSC-compressed output**.
  This is Google's *driver* model; what the ABL does for the splash is not
  documented, but the pmOS `simplefb` path needs the trigger re-armed, consistent with command mode.
- This contradicts the `ios-sweet` assumption in 03 (H11): "video-mode
  continuous scanout" does **not** hold. Writes to the splash buffer only show
  once the HW trigger is on and the panel gets TE pulses. 03 already flags
  "[verify]" correctly.

## 4. Timer and GIC

**Verdict: CONFIRMED.**

- Mainline `gs101.dtsi:1928-1936`: `arm,armv8-timer` with PPIs 13 (sec phys),
  14 (NS phys), **11 (virtual)**, 10 (hyp phys), 12 (hyp virt). DT order is
  sec-phys, ns-phys, virt, hyp-phys => PPI 11 = virtual, INTID 16+11 = **27**. Downstream
  `DS-gs gs101.dtsi:99-105` lists 13, 14, 11, 10 and adds
  `clock-frequency = <24576000>`.
- 24.576 MHz: `DS-gs gs101.dtsi:105,108-113` (`ext_24_5m`, `<24576000>`);
  mainline `gs101-pixel-common.dtsi:628-629` (`&ext_24_5m { clock-frequency =
  <24576000> }`) and `gs101.dtsi:479-482` (`oscclk`). Note: mainline's timer node
  has **no** `clock-frequency`, so Linux trusts `CNTFRQ_EL0` as programmed by
  firmware. Downstream sets it explicitly, so the shim should set/assume
  24,576,000 itself and not read `CNTFRQ_EL0` blindly.
- GIC: mainline `gs101.dtsi:658-665` `gic: interrupt-controller@10400000`
  `reg = <0x10400000 0x10000>, <0x10440000 0x100000> /* GICR * 8 */`, maintenance
  `GIC_PPI 9`. Downstream `DS-gs gs101.dtsi:90-97` is the same. 8 PEs x 128 KiB = 1 MiB.
- Not checked: 02's claim that `GICD_CTLR.DS == 0`.

## 5. CPU hotplug pre-steps

**Verdict: offsets CONFIRMED; the "needed for CPU_ON" framing is WRONG.**

Registers (mainline):

- PMU base `0x17460000` (`gs101.dtsi:1804-1807`), `pmu_intr_gen` `0x17470000`
  (`gs101.dtsi:1835-1838`, `google,gs101-pmu-intr-gen`).
- `GS101_CPU_INFORM(cpu) = 0x0860 + (cpu & 7) * 4` (`exynos-regs-pmu.h:707`).
- `GS101_GRP1_INTR_BID_UPEND 0x0108`, `GRP1_..._CLEAR 0x010c`,
  `GS101_GRP2_INTR_BID_ENABLE 0x0200`, `GRP2_..._UPEND 0x0208`, `GRP2_..._CLEAR 0x020c`
  (`exynos-regs-pmu.h:1012-1016`).
- Online sequence (`exynos-pmu.c:235-257`, `__gs101_cpu_pmu_online`):
  1. `regmap_write(pmureg, GS101_CPU_INFORM(cpuhint), CPU_INFORM_CLEAR /*0*/)`
  2. `update_bits(pmuintrgen, GRP2_INTR_BID_ENABLE, BIT(cpu), 0)`
  3. `read(GRP2_INTR_BID_UPEND)`
  4. `write(GRP2_INTR_BID_CLEAR, reg & BIT(cpu))`
- Offline (`:296-320`): `CPU_INFORM = CPU_INFORM_C2 (1)`, `ENABLE |= BIT(cpu)`,
  then clears `GRP1` pending bits `BIT(cpu)` and `BIT(cpu+8)`.
- **Detail 02 got wrong**: `cpuhint = smp_processor_id()` (`exynos-pmu.c:237`),
  i.e. the CPU *running* the notifier, not the target `cpu`. The `BIT(cpu)`
  masks use the target. For the CPUHP_BP_PREPARE callback that runs on the
  control CPU, the CPU_INFORM slot written is the control CPU's. Don't blindly
  reuse "CPU_INFORM(target)".

Direct MMIO or SMC:

- PMU writes are **SMC**, not MMIO. `gs101_pmu_data.pmu_secure = true`
  (`gs101-pmu.c:325-327`) so `exynos_pmu_probe` builds a custom regmap
  (`exynos-pmu.c:497-515`) using `tensor_sec_reg_write`. That is
  `arm_smccc_smc(0x82000504, pmu_base + reg, TENSOR_PMUREG_WRITE (1), val, ...)`
  (`gs101-pmu.c:19-22,343-356`; function id = fast, SMC32, SIP owner =
  `0x82000504`). RMW uses subcommand 2. Only registers in `gs101_pmu_wr_table`
  are accepted, and `CPU_INFORM(0..7)` (`0x860-0x87c`) lies in the
  `GS101_INFORM4..GS101_IROM_INFORM` range at `:33`.
  Source comment (`gs101-pmu.c:332-339`): *"Tensor SoCs are configured so that PMU_ALIVE
  registers can only be written from EL3, but are still read accessible."*
  Commit `0b7c6075022c` message: "protect the PMU registers ... only write
  accessible in el3 via an SMC call". Reads work as plain MMIO.
- `pmu-intr-gen` is plain MMIO (`devm_regmap_init_mmio`, `exynos-pmu.c:432-440`,
  `regmap_pmu_intr` at `:111`). Only the AP-side shim needs the physical
  address `0x17470000`.

First `CPU_ON` at boot vs hotplug:

- Commit `598995027b91` ("soc: samsung: exynos-pmu: enable CPU hotplug support
  for gs101", 2025-05-06): *"Some additional register writes are required when
  **hotplugging** CPUs on gs101, without these the system hangs when hotplugging."*
  The test is `echo 0 > cpu6/online; echo 1 > cpu6/online`. 02 states "invoking
  standard PSCI_CPU_ON on secondary cores causes the system to hang unless ...",
  which overstates the commit.
- In-code evidence that cold boot works without them: the probe loop
  `for_each_online_cpu(cpu) gs101_cpuhp_pmu_online(cpu);  /* set PMU to power on */`
  (`exynos-pmu.c:457-458`) runs *after* the secondaries came up, and mainline
  `gs101.dtsi` has used plain PSCI (`enable-method = "psci"`, `:82` etc.)
  since the initial SoC patch (Dec 2023), while the hotplug code only landed in
  May 2025 (release not checked). The CPU_INFORM / intr-gen writes are needed for
  **offline -> online** and for **cpuidle (C2)** (commit `78b72897a5c8`).
- Conclusion: the FIRST `PSCI_CPU_ON` of a secondary from a cold state needs
  **none** of the PMU writes (INFERRED from the above; no one has published a
  bare-metal run that proves it). If the shim later offlines and re-onlines a
  CPU, or enables WFI-based idle, do the sequence (via SMC).
  If the ABL itself already parked CPUs via PSCI, or left `CPU_INFORM` non-zero,
  the first `CPU_ON` could hang; a one-shot SMC write of `CPU_INFORM(0..7) = 0`
  before the first `CPU_ON` is cheap insurance but unnecessary on current evidence.

## 6. UFS

**Verdict: mostly CONFIRMED. The "link up" and "no SMC" parts are not primary-source facts.**

- Base: mainline `gs101.dtsi:1750-1775`: `ufs_0: ufs@14700000`,
  `reg = <0x14700000 0x200>` (hci), `<0x14701100 0x200>` (vs_hci), `<0x14780000
  0xa000>` (unipro), `<0x14600000 0x100>` (ufsp), `GIC_SPI 532`, `dma-coherent`,
  `samsung,sysreg = <&sysreg_hsi2 0x710>`; PHY `ufs_0_phy: phy@14704000`. No IOMMU phandle.
- Quirk: `ufs-exynos.c:2278-2295`, `gs101_ufs_drvs.quirks = UFSHCD_QUIRK_PRDT_BYTE_GRAN
  | UFSHCI_QUIRK_SKIP_RESET_INTR_AGGR | UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR |
  UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR | UFSHCI_QUIRK_SKIP_MANUAL_WB_FLUSH_CTRL |
  UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING`; `.opts` include
  `EXYNOS_UFS_OPT_UFSPR_SECURE`. Semantics (`include/ufs/ufshcd.h:703-707`:
  "resolution of the values of PRDTO and PRDTL in UTRD as byte"). Concretely
  in `ufshcd.c`:
  - `:2721-2725`: `prd_table_length = sg_entries * ufshcd_sg_entry_size(hba)`
  - `:4189-4196`: `response_upiu_offset`, `prd_table_offset` and
    `response_upiu_length` are written in **bytes** (non-quirk: dwords).
  02 says PRDTO/PRDTL, correct, but the response-UPIU offset/length are also byte units.
  PRDT entry size is 16 B unless FMP's DESCTYPE=3 is set (then 128 B).
- FMP / SMU: mainline only issues SMCs when `CONFIG_SCSI_UFS_CRYPTO` is on and
  `EXYNOS_UFS_OPT_UFSPR_SECURE` is set (`ufs-exynos.c:1362-1420`):
  - `SMC_CMD_FMP_SECURITY` = SIP `0x1810` (`0xC2001810`) args `(0, SMU_EMBEDDED, CFG_DESCTYPE_3)`:
    "needed to make the hardware use the larger PRDT entry size".
  - `SMC_CMD_SMU` = `0x1850` (`0xC2001850`) `SMU_INIT`: "needed to initialize FMP.
    Without it, errors occur when inline encryption is used."
  - `SMC_CMD_FMP_SMU_RESUME` = `0x1860` (`0xC2001860`) on resume (`:1443-1460`).
  These SMCs (and `exynos_ufs_config_smu()`, skipped for UFSPR_SECURE,
  `:844-850`) gate **inline encryption only**. Unencrypted requests have
  `FAS = FMP_BYPASS_MODE (0)` by default (`DS-gs ufs-exynos-fmp.c:138-142`, same SMCs
  at `:75,83`). So plain reads/writes should not need an SMC. This is an inference
  from the code structure (crypto-off kernels exist via the `#else` stub at
  `ufs-exynos.c:1521`), not an explicit statement.
- **Residual risk (UNVERIFIABLE)**: if firmware (the ABL, which uses UFS) already
  issued `SMC_CMD_FMP_SECURITY ... DESCTYPE_3`, the controller would expect 128-byte PRDT
  entries at handoff. Nothing documents the state the ABL leaves.
- Bootloader leaves link up: **UNVERIFIABLE**. No Google or Linaro source states it.
  Linux does a full HCE reset + link startup (`pre_link`/`post_link` hooks), which
  says nothing about the ABL. 02's "VERIFIED-FROM-SOURCE / INFERRED" should read INFERRED.
- 02 also says UFS DMA hazards from S2MPU: mainline has no `s2mpu` node and
  I did not find a public document listing the protected carve-outs.
  Treat 02's `0x90200000` / `0x93000000` / `0x94000000` / `0xf0000000` list as **unverified**.
  Downstream carve-outs are in `DS-gs gs101-rmem.dtsi`.

## 7. Watchdogs

**Verdict: bases CONFIRMED; disable recipe INCOMPLETE; bootloader state UNVERIFIABLE.**

- Bases: mainline `gs101.dtsi:623-647`: `watchdog_cl0: watchdog@10060000`,
  `GIC_SPI 765`, `samsung,cluster-index = <0>`; `watchdog_cl1: watchdog@10070000`,
  `GIC_SPI 766`, `cluster-index = <1>`; both `compatible = "google,gs101-wdt"`,
  clocks `CLK_GOUT_MISC_WDT_CLUSTERn_PCLK` (cmu_misc) + `ext_24_5m`. Downstream
  `DS-gs gs101.dtsi:1458-1479` same bases (`cl1` also has `use_multistage_wdt`,
  an FIQ debug watchdog).
- 30 s: `gs101-pixel-common.dtsi:929-933` `&watchdog_cl0 { timeout-sec = <30>; status = "okay"; }`.
  That is what *Linux* programs. It says nothing about what the ABL arms.
- WTCON (`s3c2410_wdt.c:32-54`): `RSTEN` bit 0, `INTEN` bit 2, `ENABLE` bit 5
  (`S3C2410_WTCON_ENABLE`), `DBGACK_MASK` bit 16, prescaler bits 15:8, divider bits 4:3.
  Registers: `WTCON 0x00`, `WTDAT`, `WTCNT` (and `WTCLRINT`).
  `__s3c2410wdt_stop` clears `ENABLE | RSTEN` (`:508-516`).
- **gs101 needs PMU bits too.** Driver data (`s3c2410_wdt.c:80-85,321-345`):

  | | cl0 | cl1 |
  |---|---|---|
  | `mask_reset_reg` (`QUIRK_HAS_PMU_MASK_RESET`) | `CLUSTER0_NONCPU_INT_EN 0x1244` bit 2, *inverted* | `0x1444` bit 2 |
  | `cnt_en_reg` (`QUIRK_HAS_PMU_CNT_EN`) | `CLUSTER0_NONCPU_OUT 0x1220` bit **8** | `0x1420` bit **7** |
  | `rst_stat_reg` | `0x3B44` bit 0 | `0x3B44` bit 1 |

  `s3c2410wdt_enable(wdt, false)` (`:460-478`) clears the counter-enable bit
  and sets the reset mask. There is **no** `disable_reg`/`AUTO_DISABLE` quirk on gs101.
  Because `pmu_secure` is true, these PMU bits are writable only through the SMC
  (`0x82000504`), not by direct MMIO from EL1.
- Practical consequence: clearing `WTCON.ENABLE` (and `RSTEN`) at `0x10060000` /
  `0x10070000` stops the WDT's own counter, which is probably enough to stop the
  reset path (inferred), but the PMU counter-enable / reset-mask bits stay as ABL
  left them. If resets still occur, mask via the SMC PMU write (INT_EN bit 2
  cleared, NONCPU_OUT enable bit cleared).
- Hazard: both WDT blocks sit behind `cmu_misc` PCLK gates. Touching
  `0x10060000` with the gate closed can hang the bus on Exynos. The omarchy-mobile
  gs201 bring-up notes the same ("needs a cmu_misc gate").
- "Bootloader leaves them running": **UNVERIFIABLE**. No primary source states it.
  The omarchy-mobile notes only list "Watchdog left armed by ABL" as a risk to
  check (gs201), not an observation.

## 8. Debug UART

**Verdict: command CONFIRMED, base CONFIRMED, SBU/instance mapping UNVERIFIABLE.**

- `fastboot oem uart enable` / `fastboot oem uart config <baud>`: AOSP "Build
  Pixel kernels" (source.android.com/docs/setup/build/building-pixel-kernels):
  *"If you have a serial dongle and want to enable serial logs, the command is:
  `fastboot oem uart enable` / `fastboot oem uart config 3000000`"*, then
  `screen -fn /dev/ttyUSB* 3000000`. The page covers Pixel 6/6 Pro/6a under one flow.
  Pixel 6a specific: the "Porting postmarketOS to the Pixel 6A" blog post
  (gabriel.marcanobrady.family, 2023-04-24): `fastboot oem uart enable`; uses the SBU pins ("co-opt the
  Alternate Mode pins"), 1.8 V adapter, **115200 8N1** by default, which pin is
  TX depends on cable orientation; `fastboot oem uart disable`. Note 02 and the
  AOSP page disagree on baud (115200 default vs 3,000,000 after `uart config`).
- Base `0x10a00000`: mainline `gs101.dtsi:1179-1205` `usi_uart: usi@10a000c0`
  containing `serial_0: serial@10a00000` (`google,gs101-uart`, `GIC_SPI 634`,
  `uart0_bus` = pins `gpp1-2`, `gpp1-3`, function 2, fifosize 256).
  `gs101-pixel-common.dtsi:20,26` uses `serial0`/`stdout-path = &serial_0`.
  Downstream cmdline `DS-gs gs101.dtsi:80`:
  `earlycon=exynos4210,mmio32,0x10A00000 console=ttySAC0,115200n8`.
  So the AP console UART (USI_UART, "UART0" at `0x10a00000`) is the SBU console.
  Note `gpp1-2/3` are SoC-side pads; the DT does not describe the USB-C SBU mux.
  The SBU routing (a TCPC / mux gated by the `oem uart` command) is in the ABL, not DT.
- Gotchas from mainline: `gs101-pixel-common.dtsi:700-706`: *"Until bootloader
  is updated to set those two when console is enabled, we disable PD here"*
  (`pd-disable; typec-power-opmode = "default"`). With the UART enabled the
  USB-C PD stack may be off. Plan for that.
- Which USI instance: **USI0 / `usi_uart`** (peric0), base `0x10a00000`. This
  mapping is from DT, not from a board schematic.

## 9. `fastboot boot` on Pixel 6a

**Verdict: UNVERIFIABLE for an unsigned custom image on bluejay. Evidence below.**

- No Google ABL source is public; `libbootloader` is not the Pixel ABL. All
  statements about ABL behaviour are from users.
- The fastboot host tool passes a `ANDROID!` image through unmodified
  (`platform/system/core fastboot/fastboot.cpp:657-673`, `LoadBootableImage`:
  `memcmp(... BOOT_MAGIC ...)` => returns the file as-is; `die("cannot boot a
  boot.img *and* ramdisk")`). Header v4 is therefore sent as-is. Only the
  non-boot.img path (`fastboot boot Image.lz4`) builds a v2 image and rejects `--dtb` for non-v2.
  The protocol text (`fastboot/README.md:153-155`): "boot: The previously
  downloaded data is a boot.img and should be booted according to the normal
  procedure for a boot.img".
- Bluejay, flash path: the same blog post packs a v4 boot.img, finds that *"the Pixel
  6A bootloader, even when unlocked, will refuse to boot a boot.img that does not
  have a valid AVB signature"*, signs it with a throwaway 4096-bit key via
  `avbtool add_hash_footer` and uses **`fastboot flash boot`**, not `fastboot boot`.
  So: an AVB hash footer (any key) was needed on an unlocked 6a, on that
  firmware (build SD2A.220601.004.B2). Not verified for `fastboot boot`.
- AOSP's own flow (`source.android.com/docs/setup/build/building-pixel-kernels`):
  *"If you haven't disabled verification, you need to do it before flashing the
  custom kernel: `fastboot oem disable-verification`"*. All flashing, no
  `fastboot boot`. The page's Pixel 6/6a (bluejay) flow flashes `boot`, `dtbo`
  and `vendor_boot` (or `vendor_boot:dlkm` with `--dtb`), and says the Pixel 6/6
  Pro/6a repos include a prebuilt `vendor_ramdisk-DEVICE.img`.
  It lists 6a for the Google kernel but *only Pixel 6 and 6 Pro* for upstream
  mainline ("Pixel 6 and Pixel 6 Pro are also supported in the upstream Linux kernel").
- Oriole (not 6a), `fastboot boot` of a **non-Google image**: community
  `TheNoswad/pixel6-mainline-nura` `INSTALL.md` has `pmbootstrap flasher boot`
  ("To try a kernel without flashing it") and `fastboot boot u-boot-boot.img`
  for M8's U-Boot image (oriole, after `pmbootstrap flasher flash_vbmeta`, i.e.
  with the vbmeta flashed with verification off). That is a custom, non-Google
  image booted from RAM on a Pixel 6. It does not prove the same for a 6a, and
  does not show what an unsigned image does with *stock* vbmeta.
- `fastboot boot magisk_patched.img` on 6/6a is widely reported (guides on
  droidwin etc., secondary) for Magisk-patched stock images; those keep the
  stock footer layout.
- Does `fastboot boot` use the flashed `vendor_boot`/`dtbo`? **UNVERIFIABLE.**
  A v4 `boot.img` has no DTB or vendor ramdisk (they live in `vendor_boot`;
  AOSP boot-image-header v4), so a working RAM boot of a v4 image on Pixel
  necessarily consumes the flashed `vendor_boot` (DTB + ramdisk fragments) and
  presumably the `dtbo` overlay chosen by `board_id` (`build.config.bluejay`
  builds `dtbo.img`). That is by design, not confirmed from ABL source. For a
  shim that wants *its own* DTB, a v2 image may not be accepted by this ABL
  generation. Neither is documented.
- Hence the open decision: test `fastboot boot` with (a) a v4 `boot.img` plus
  an AVB hash footer made with a throwaway key, then (b) without a footer; if
  rejected, `fastboot oem disable-verification` (AOSP-documented) is the
  least-invasive switch; it touches vbmeta state but not the Android partitions.
  Per AGENTS.md, confirm any state-changing `fastboot oem` command with the user first.

## 10. A10 iPadOS 18 shared cache (iPad7,11)

**Verdict: UNVERIFIABLE directly. Indirect evidence is strong.**

- The cache lives inside the AEA-encrypted SystemOS cryptex DMG of the IPSW. I
  confirmed the IPSW exists (api.ipsw.me: `iPad7,11`, board `J171AP`, `t8010`,
  e.g. 18.7.10 / 22H374, ~7.9 GB, currently signed) but could not list the DMG
  contents without downloading and decrypting multi-GB images, so I did not
  see the cache filename. No public listing of `dyld_shared_cache_arm64` vs
  `_arm64e` for this device/OS turned up.
- Indirect evidence:
  - Apple, "Preparing your app to work with pointer authentication": arm64e
    is for A12 and later (cited in 04). A10 (t8010) has no FEAT_PAuth, so Apple
    cannot ship a signed-pointer (arm64e) cache for it.
  - A device table on GitHub (`DevYeom/apple-wiki`) lists iPad (7th gen) as
    `arm64`, newer iPads as `arm64e` (secondary).
  - Apple lists the 7th-gen iPad as iPadOS 18-compatible (04 cites
    support.apple.com/104986); the 6th gen is not.
- To settle it with a primary source at zero risk: `ipsw extract --dyld
  iPad_10.2_18.7.10_22H374_Restore.ipsw` (blacktop/ipsw) and `file` the result.
  That is host-side only. I did not do it (no `ipsw`, no sudo, multi-GB download).

---

## Corrections to the existing reports

Facts I found wrong or overstated in `02-gs101-peripherals.md`:

1. §7 touch rails "vci/vddi": actual `LDO25M` (1.8 V TSP_DVDD) and `LDO26M` (3.3 V TS_AVDD).
2. §8 vol-key bank: vol keys are in `pinctrl_far_alive` (`0x174e0000`), not `0x174d0000`.
3. §4 "PSCI CPU_ON hangs without CPU_INFORM / intr-gen": the commit documents *hotplug*
   (offline->online); the register is `CPU_INFORM(smp_processor_id())`; PMU writes go
   via SMC `0x82000504`; `pmu-intr-gen` is MMIO.
4. §11 watchdog: "clear `WTCON_ENABLE` bit in `0x10060000`" is incomplete (PMU bits, clock gate).
   The "30 s" is Linux's own timeout.
5. §5 UFS: "FMP requires SMC 0x1810" applies to inline crypto only; response-UPIU
   offset/length are also byte units; "bootloader leaves link up" is INFERRED, not verified.
6. §6 "bluejay fb at 0xfac00000" is the generic gs101 Pixel value from mainline
   (Oriole/Raven). No bluejay source.
7. §3 timer: the `ext_24_5m` line cited is the MCT/oscclk override; the arch timer's
   explicit frequency is only in the *downstream* DT.
8. `CASE_STUDY.md:36` says the 6a panel is the equivalent of sweet's "video mode,
   live". Google's driver says **command mode**.

Unverified items in 02 I did not check (S2MPU carve-out addresses, GICD
`DS == 0`, USB wrapper addresses, ACPM mailbox addresses, thermal limits).
Treat them as unverified.

## Open questions that only the device can answer (not run here)

- Raw `TRIG_CON` at `0x1C300030` as the ABL hands off (is `HW_TRIG_EN` 0?).
- Where the bluejay ABL puts the framebuffer (read DECON window/channel regs).
- Whether the ABL left the WDTs armed; whether `cmu_misc` WDT PCLK is open.
- Whether UFS is up and which PRDT descriptor size the ABL last set.
- Whether `fastboot boot` takes an unsigned v4 image and which vbmeta state it needs.
