# 19 - gs101 USI11 (SPI11) clock chain in CMU_PERIC1

Status: research only, nothing run on hardware. Tags: **[src]** read from source (file:line), **[guess]** not in any
source read.

Sources (fetched 2026-10-22, all read in full for the quoted parts):

- `LX` = mainline Linux master via git.kernel.org (`.../torvalds/linux.git/plain/<path>`; log head seen 7be4bc52fd9e):
  `drivers/clk/samsung/clk-gs101.c`, `drivers/clk/samsung/clk-exynos-arm64.c`, `drivers/clk/samsung/clk.c`,
  `include/dt-bindings/clock/google,gs101.h`, `arch/arm64/boot/dts/exynos/google/gs101.dtsi`,
  `drivers/soc/samsung/exynos-usi.c`, `drivers/spi/spi-s3c64xx.c`.
- `GK` = Google `kernel/gs`, branch `android-gs-bluejay-5.10-android13-qpr3` (commit 6e771b230c52, 2023-05-30),
  `https://android.googlesource.com/kernel/gs/+/refs/heads/android-gs-bluejay-5.10-android13-qpr3/<path>`:
  `drivers/soc/google/cal-if/gs101/{cmucal-sfr.c,cmucal-node.c,cmucal-qch.c,cmucal-vclk.c,cmucal-vclklut.c}`,
  `drivers/soc/google/cal-if/{ra.c,cal-if.c}`, `drivers/clk/samsung/clk-gs101.c`, `drivers/spi/spi-s3c64xx.c`,
  `arch/arm64/boot/dts/google/{gs101.dtsi,gs101-usi.dtsi}`.

## 1. Block and chain

- CMU_PERIC1 base `0x10c00000`, size 0x10000. [src] LX `gs101.dtsi:1267-1269` (`cmu_peric1: clock-controller@10c00000`,
  `reg = <0x10c00000 0x10000>`); LX `clk-gs101.c:4356` ("Register Offset definitions for CMU_PERIC1 (0x10c00000)");
  GK `cmucal-sfr.c:38` (`SFR_BLOCK(CMU_PERIC1, 0x10c00000, 0x8000)`).
- Mainline mapping of USI11 to the TOP0 ports: pclk = `PERIC1_TOP0_PCLK_4`, ipclk = `PERIC1_TOP0_IPCLK_4`, for the
  usi11 node and its `spi_11` child at `0x10d40000`. [src] LX `gs101.dtsi:1469-1478` (usi11,
  `clocks = PCLK_4, IPCLK_4`, `clock-names = "pclk", "ipclk"`, `samsung,sysreg = <&sysreg_peric1 0x100c>`) and
  `:1512-1521` (spi_11: `clocks = PCLK_4, IPCLK_4`, `clock-names = "spi", "spi_busclk0"`). Same pairing in GK via the
  vclks `GATE_PERIC1_TOP0_USI11_USI` / `VDOUT_CLK_PERIC1_USI11_USI` (GK `gs101-usi.dtsi:612`, `clk-gs101.c:982,1442`).
- Parent chain of the SPI SCK source (ipclk), bottom up: [src] LX `clk-gs101.c:4608` ipclk_4 gate has parent
  `dout_peric1_usi11_usi`; `:4549-4551` that divider has parent `mout_peric1_usi11_usi_user`; `:4524-4525` that mux has
  parents `{ "oscclk", "dout_cmu_peric1_ip" }` (`:4509`); `dout_cmu_peric1_ip` is `CMU_TOP` div `0x18e0` (`:217`, `:1219-1220`,
  4 bits at bit 0) fed from `gout_cmu_peric1_ip` (gate `0x2104` bit 21, `:299`, `:1408-1409`) fed from
  `mout_cmu_peric1_ip` (mux `0x10e8`, bits [1:0], `:152`, `:1089-1090`; parents `dout_cmu_shared0_div4`,
  `dout_cmu_shared2_div2`, `dout_cmu_shared3_div2`, `fout_spare_pll`, `:899-901`). CMU_TOP base `0x1e080000`
  (LX `gs101.dtsi:1910-1912`).
- pclk chain: gate `pclk_4` (LX `clk-gs101.c:4640`) parent `mout_peric1_bus_user` (mux `PLL_CON0_..BUS_USER` `0x600`, bit 4,
  `:4512`, `:4357`).
- Also gated, in the same domain: `gout_peric1_clk_peric1_usi11_usi_clk`, a reset-sync clock on `dout_peric1_usi11_usi`
  (LX `clk-gs101.c:4668`, reg `0x2068` `:4408`). Mainline gives it no consumer (nothing in the `usi11` DT node
  references it), so it is only on when something enables it. [src]

## 2. Register offsets and bits (CMU_PERIC1 = 0x10c00000)

| Item | Offset | Phys addr | Bits | Source |
|---|---|---|---|---|
| `PLL_CON0_MUX_CLKCMU_PERIC1_BUS_USER` (pclk mux) | 0x600 | 0x10C00600 | MUX_SEL bit 4 (0 = oscclk, 1 = `dout_cmu_peric1_bus`), BUSY bit 16 | LX `clk-gs101.c:4357,4512`; GK `cmucal-sfr.c:3463-3465`, `cmucal-node.c:1066-1068` |
| `PLL_CON1_MUX_CLKCMU_PERIC1_BUS_USER` | 0x604 | 0x10C00604 | ENABLE_AUTOMATIC_CLKGATING bit 28 | LX `:4358`; GK `cmucal-sfr.c:3465` |
| `PLL_CON0_MUX_CLKCMU_PERIC1_USI11_USI_USER` | 0x640 | 0x10C00640 | MUX_SEL bit 4 (0 = oscclk, 1 = `dout_cmu_peric1_ip`), BUSY bit 16 | LX `:4365,4524-4525,4509`; GK `cmucal-sfr.c:3466-3467`, `cmucal-node.c:1070-1073` |
| `PLL_CON1_MUX_CLKCMU_PERIC1_USI11_USI_USER` | 0x644 | 0x10C00644 | ENABLE_AUTOMATIC_CLKGATING bit 28 | LX `:4366`; GK `cmucal-sfr.c:3468` |
| `PERIC1_CMU_PERIC1_CONTROLLER_OPTION` | 0x800 | 0x10C00800 | bit 28 ENABLE_AUTOMATIC_CLKGATING, bit 29 ENABLE_POWER_MANAGEMENT (mainline also uses 24, 30, 31) | LX `:4373`; GK `cmucal-sfr.c:12009-12010`; LX `clk-exynos-arm64.c:28-35` |
| `CLK_CON_DIV_DIV_CLK_PERIC1_USI11_USI` | 0x180c | 0x10C0180C | DIVRATIO bits [3:0] (rate = parent / (n+1), standard CMU divider - [guess] for the exact formula, the width is [src]), BUSY bit 16, ENABLE_AUTOMATIC_CLKGATING bit 28 | LX `:4378,4549-4551`; GK `cmucal-sfr.c:449,3876-3878` |
| `..GOUT_BLK_PERIC1_UID_PERIC1_TOP0_IPCLKPORT_IPCLK_4` (SPI SCK source) | 0x202c | 0x10C0202C | CG_VAL bit 21, MANUAL bit 20, ENABLE_AUTOMATIC_CLKGATING bit 28 | LX `:4393,4608-4609`; GK `cmucal-sfr.c:1799,7919-7921` |
| `..GOUT_BLK_PERIC1_UID_PERIC1_TOP0_IPCLKPORT_PCLK_4` | 0x204c | 0x10C0204C | same three bits | LX `:4401,4640`; GK `cmucal-sfr.c:1805,7937-7939` |
| `..GOUT_BLK_PERIC1_UID_RSTNSYNC_CLK_PERIC1_USI11_USI_IPCLKPORT_CLK` | 0x2068 | 0x10C02068 | same three bits | LX `:4408,4668`; GK `cmucal-sfr.c:1787,7883-7885` |
| `..GOUT_BLK_PERIC1_UID_SYSREG_PERIC1_IPCLKPORT_PCLK` (sysreg_peric1 pclk) | 0x2078 | 0x10C02078 | same three bits | LX `:4412,4684`; GK `cmucal-sfr.c:1783,7871-7873` |
| `QCH_CON_PERIC1_TOP0_QCH_USI11_USI` | 0x302c | 0x10C0302C | ENABLE bit 0, CLOCK_REQ bit 1, IGNORE_FORCE_PM_EN bit 2, EXPIRE_VAL bits [25:16] | LX `:4424`; GK `cmucal-sfr.c:2776,11626-11629`, `cmucal-qch.c:848` |
| CMU_TOP `CLK_CON_MUX_MUX_CLKCMU_PERIC1_IP` | 0x10e8 (phys 0x1E0810E8) | | bits [1:0] | LX `:152,1089-1090` |
| CMU_TOP `CLK_CON_DIV_CLKCMU_PERIC1_IP` | 0x18e0 (0x1E0818E0) | | bits [3:0] | LX `:217,1219-1220` |
| CMU_TOP `CLK_CON_GAT_GATE_CLKCMU_PERIC1_IP` | 0x2104 (0x1E082104) | | bit 21 | LX `:299,1408-1409` |

The TOP-level gate/mux/div are shared by every PERIC1 USI/I3C, and the PERIC1 UART/I2C/SPI engines (and the
UART that carries the console, if any) already run from it; so they are very probably already configured by the
bootloader. [guess]

Reset values: **not found.** Neither LX (`clk-gs101.c` carries only offsets, bit index, parents) nor GK (`cmucal-sfr.c`
carries only offset/bit/width) states a reset or power-on value for any of these registers. Do not assume any.

## 3. Which gates are automatic and which are forced

- Gate registers (`0x2000-0x2fff`) have three bits: bit 28 = ENABLE_AUTOMATIC_CLKGATING (HWACG), bit 20 = MANUAL, bit 21 =
  CG_VAL. [src] GK `cmucal-sfr.c:7919-7921` (CG_VAL 21, MANUAL 20, ENABLE_AUTOMATIC_CLKGATING 28); LX
  `clk-exynos-arm64.c:24-25` (`GATE_MANUAL BIT(20)`, `GATE_ENABLE_HWACG BIT(28)`).
- Selection rule, from Google's CAL: MANUAL = 1 -> CG_VAL (bit 21) controls the gate; MANUAL = 0 -> the automatic
  gating (bit 28) decides. [src] GK `ra.c:412-436` (`ra_set_gate`, comment "MANUAL(status) 1 : CG_VALUE(offset) control, 0 :
  ENABLE_AUTOMATIC_CLKGATING(enable) control"; note the else branch in that function writes `clk->offset`, which looks
  like a bug in the vendor source, so I do not rely on the automatic path).
- Mainline sets the global OPTION register and then, if the CMU is "auto capable" (`auto_clock_gate` set and the DT
  `reg` size is exactly 0x10000), leaves the individual gate registers in automatic mode. [src] LX `clk-exynos-arm64.c:98-111`
  (`writel(CMU_OPT_GLOBAL_EN_AUTO_GATING, reg_base + cmu->option_offset)` with value bits 31|30|29|28|24 = 0xF1000000);
  `clk-gs101.c:4702` sets `.auto_clock_gate = true` for PERIC1; `clk.c:246-258` size check. Otherwise (not auto)
  it forces each gate: `val |= GATE_MANUAL; val &= ~GATE_ENABLE_HWACG; writel(val, reg)` (LX `clk-exynos-arm64.c:119-128`;
  the comment says MANUAL "overrides the global CMU automatic mode option"). The driver then toggles bit 21 as a
  normal `clk_gate` set-to-enable (LX `clk.c:356-359`, `GATE(..., 21, flags, 0)`, e.g. `clk-gs101.c:4608-4609`).
- Gates in mainline with `CLK_IS_CRITICAL`: `gout_peric1_peric1_pclk` (0x2004) and `gout_peric1_lhm_axi_p_peric1_i_clk`
  (0x201c) (LX `clk-gs101.c:4566-4593`); `CLK_IGNORE_UNUSED` on the GPIO pclk (0x2018). Everything else
  (including ipclk_4, pclk_4, usi11 rstnsync) has plain flags 0, so Linux gates them when unused.
- QCH registers (`0x3000+`): the USI11 QCH is `QCH_CON_PERIC1_TOP0_QCH_USI11_USI` (0x302c). Mainline does not touch the
  QCH block: `is_gate_reg` is limited to `0x2000-0x2fff` (LX `clk-exynos-arm64.c:42-43,64-67,117-128`). Google CAL does:
  `cal_qch_init(id, use_qch)` calls `ra_set_qch(id, use_qch, 0, 0)`, which clears bits 0 and 1, then writes `en << 0 | req << 1`
  (GK `cal-if.c:175-183`, `ra.c:460-511`); `ra_req_enable_qch` says "QH ENABLE(offset) 1: Skip, 0: REQ(status) control"
  (GK `ra.c:513-530`); `ra_enable_qch` sets/clears bit 0 (GK `ra.c:532-546`). GATE_PERIC1_TOP0_USI11_USI is a
  `HWACG_VCLK` over `PERIC1_TOP0_QCH_USI11_USI` (GK `clk-gs101.c:982-987`). Whether the Android bootloader leaves
  ENABLE set is unknown. [guess]

## 4. Rate after plain bootloader handoff

Not documented in anything read. GK `gs101-usi.dtsi:603-621` (spi_11) and the mainline `spi_11` node have no
`assigned-clock-rates` / `assigned-clocks` (grep over both dtsi found none). The only rate hints are:

- GK `cmucal-vclk.c:3660` registers a single LUT entry for USI11: `{400000, div_clk_peric1_usi11_usi_uud_lut_params}`
  (kHz, i.e. 400 MHz as the nominal maximum in the table), where the params are `{0, 1, 0, 0, 0, 0, 0}`
  (`cmucal-vclklut.c:339-343`); the shared USI table is `cmucal-vclk.c:3642-3650` (400/200/133/100/66/50/40/24.576 MHz).
  This is the LUT of selectable rates, not the post-handoff state. [src]
- Google SPI driver sets the source clock to 4x the SCK (`clk_get_rate != cur_speed * 4` -> `clk_set_rate(src_clk,
  cur_speed*4)`, GK `spi-s3c64xx.c:836-848`) and divides by 2 in its own CLK_CFG (`:854`); mainline `gs101_spi_port_config`
  has `.clk_div = 4`, `.clk_from_cmu = true`. [src] LX `spi-s3c64xx.c` `gs101_spi_port_config` and `:750-754`.
  So at 10 MHz max SCK the target ipclk is 40 MHz (matches note 15 unknown 3).
- Therefore the actual ipclk after handoff (or even whether it is gated or on `oscclk` = 24.576 MHz) must be measured by
  reading `0x10C00640`, `0x10C0180C` and `0x10C0202C`. The USI11 mux's `oscclk` input is the 24.576 MHz oscillator
  (the vendor LUT calls the leaf `24576` kHz, `cmucal-vclk.c:3650`; the DT calls it `ext_24_5m`, LX `gs101.dtsi:479-483`).

## 5. How Linux enables the clocks

- `exynos-usi` driver: `exynos850_usi_clk_names = { "pclk", "ipclk" }`; `exynos_usi_enable()` and `exynos_usi_set_sw_conf()` do
  `clk_bulk_prepare_enable` on both before touching USI_CON/USI_OPTION and `clk_bulk_disable_unprepare` afterwards. [src] LX
  `exynos-usi.c:101,178,196,210,229,244`.
- `spi-s3c64xx` (mainline, `google,gs101-spi`): `devm_clk_get_enabled(.., "spi")` = pclk_4 (gate 0x204c) and
  `devm_clk_get_enabled(.., "spi_busclk0")` = ipclk_4 (gate 0x202c) (LX `spi-s3c64xx.c:1329-1335`, DT
  `gs101.dtsi:1512-1521`); rate set with `clk_set_rate(src_clk, cur_speed * div)` (`:750-754`).
- Google SPI driver: `clk` = `gate_spi_clk` (QCH vclk), `src_clk` = `ipclk_spi` (VDOUT div vclk), both `clk_prepare_enable`d at
  probe and resume (GK `spi-s3c64xx.c:1709-1716,1754-1760,1968-1978,2038-2039`).
- So enabling "the SPI clock" means: the CMU_TOP PERIC1_IP chain, the USI11 mux selected to the TOP source, the USI11
  divider, and the two TOP0 gates (pclk_4, ipclk_4). The rstnsync gate is not requested by any Linux consumer. [src]

## 6. Minimal force-enable sequence (EL1, physical addresses)

All accesses are 32-bit. Every step: read, print, modify only the named bits, write, read back and print; stop (do not
continue) if a read-back mismatches. Base `CMU = 0x10C00000`. Run steps 0-2 as reads only first and log them; the
touch driver already prints in steps. The reads alone answer the "is it gated" question before anything is written.

| # | Action | Address | Basis |
|---|---|---|---|
| 0 | Read only: OPTION, USI11 user-mux, bus user-mux (+0x604/+0x644), div, ipclk_4 gate, pclk_4 gate, rstnsync gate, QCH | 0x10C00800, 0x10C00640, 0x10C00600, 0x10C00604, 0x10C00644, 0x10C0180C, 0x10C0202C, 0x10C0204C, 0x10C02068, 0x10C0302C | offsets [src] |
| 1 | Mux: `v = rd(0x10C00640); v |= 1<<4; wr`; poll bit 16 (BUSY) == 0; read back bit 4 == 1 | 0x10C00640 | offset/bit [src] (LX `clk-gs101.c:4524-4525`, GK `cmucal-sfr.c:3466-3467`). Mux input 1 = `dout_cmu_peric1_ip` [src]; whether the TOP side is on is [guess] (read 0x1E0810E8/0x1E0818E0/0x1E082104 to check: gate bit 21 should be 1) |
| 2 | Divider: leave alone first; if the read shows divratio field such that parent/(n+1) > 40 MHz and you need a lower rate, write bits [3:0] and poll bit 16 (BUSY) == 0 | 0x10C0180C | offset/width [src]; the value to write [guess] (needs the TOP parent frequency, unknown) |
| 3 | Gate ipclk_4: `v = rd; v |= (1<<20); v &= ~(1<<28); v |= (1<<21); wr`; read back, expect bit 20 = 1, 21 = 1, 28 = 0 | 0x10C0202C | bit meaning [src] (GK `cmucal-sfr.c:7919-7921`, LX `clk-exynos-arm64.c:122-128`); the order is the mainline manual-mode behavior [src] |
| 4 | Gate pclk_4: same recipe | 0x10C0204C | [src], same references. Registers already read fine, so pclk is probably on already [guess] |
| 5 | Gate usi11 rstnsync: same recipe | 0x10C02068 | [src] offset/bits; whether the SPI engine needs it in the AP-only path is [guess] (Linux never requests it) |
| 6 | If an SPI transfer still times out: QCH `v = rd(0x10C0302C); v |= 1;` (ENABLE) and also set bit 1 (CLOCK_REQ) to test with a forced request; read back | 0x10C0302C | offset/bits [src] (GK `cmucal-sfr.c:11626-11629`); the value to write is [guess] (Google's `cal_qch_init(id,1)` writes ENABLE=1 / REQ=0, GK `cal-if.c:175-183`, `ra.c:493-497`; REQ=1 is only a test) |
| 7 | If it still fails: set the global OPTION register to automatic mode as Linux does, `wr(0x10C00800, 0xF1000000)` | 0x10C00800 | value [src] (LX `clk-exynos-arm64.c:28-35,110`), but it is only needed if the bootloader left manual mode, and it also changes the other gates, so try it last [guess] |

After steps 1 to 5, re-read the SPI engine registers and run the transfer. Skip step 1 if bit 4 already reads 1 and the
divider field is non-zero (the field value that corresponds to 40 MHz is unknown, see step 2).

## 7. What this does not establish

- No reset value was found for any register above (section 2).
- No source states the handoff rate or on/off state; the first hardware run of step 0 will settle it. Read every address in step 0
  before any write.
- The TOP-level chain (0x1E0810E8 / 0x1E0818E0 / 0x1E082104) should not be written without first reading it.
