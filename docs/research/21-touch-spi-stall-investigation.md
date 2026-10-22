# 21 - Touch SPI11 stall: root-cause investigation (FTM5 on USI11, gs101)

Status: research only, 2026-10-22. No device touched, no kernel built. The `spi-top` run
(`logs/runs/20261022T104928Z-spi-top`, `sweettouchclk=3`) was still in progress when this was written, so its CMU_TOP values
are **not** used here.

Tags: **[hw]** observed on hardware (run dir given), **[src]** read from source (file:line), **[abl]** read from the ABL
disassembly (`~/pixelios-artifacts/abl-code/abl_code.bin`, flat, file offset = address; string base 0xf8803000),
**[guess]** not established by any source.

Sources (fetched 2026-10-22 into `~/pixelios-artifacts/scratch/touch-src/`, not in git):

- `GK` = Google `kernel/gs`, branch `android-gs-bluejay-5.10-android13-qpr3` (head 6e771b230c52): `drivers/spi/spi-s3c64xx.c`
  (`GK-spi`), `arch/arm64/boot/dts/google/{gs101-usi.dtsi,gs101-common-touch.dtsi,gs101-fts-touch.dtsi,gs101-pmic.dtsi}`,
  `drivers/soc/google/cal-if/{ra.c,gs101/cmucal-vclk.c,gs101/cmucal-vclklut.c,gs101/cmucal-node.c}`,
  `drivers/regulator/s2mpg10-regulator.c`, `drivers/mfd/s2mpg10-core.c`, `include/linux/mfd/samsung/s2mpg10-register.h`,
  `include/dt-bindings/pinctrl/samsung.h`.
- `FTM5` = `kernel/google-modules/touch/fts_touch`, same branch, `ftm5/fts.c`, `ftm5/fts_lib/ftsCore.c`.
- `TBN` = `kernel/google-modules/touch/common`, same branch, `touch_bus_negotiator.{c,h}`.
- `AOC` = `kernel/google-modules/aoc`, same branch, `aoc.c`.
- `BJ` = `kernel/devices/google/bluejay`, same branch, `dts/gs101-bluejay-touch.dtsi`.
- `LX` = mainline `drivers/spi/spi-s3c64xx.c` (git.kernel.org master, 2026-10-22).
- ABL disassembly: Sonnet helper (capstone, script `~/pixelios-artifacts/scratch/abl-spi/x.py`); the items marked
  "verified" below were re-disassembled by me.

## 0. What the hardware already told us (all [hw])

| Fact | Run |
|---|---|
| `CS_REG` (0x0c) = **0x1** (NSSOUT inactive, manual mode) after our setup, in every run; the driver never writes it except in variant E | `20261022T102815Z-spi-clk2/merged.log:124` |
| Variants A-D (CS_REG still 0x1): STATUS 0x140 = TX level 5, RX 0, CH_CFG 0x3 | same, `:132-135` |
| Variant E (CS_REG = 0x22, bit0 = **0**, AUTO): STATUS 0x100 = TX level 4, RX 0: **one byte left the FIFO, none came back** | same, `:137`; also `20261022T101540Z-spi-var` |
| USI11 user mux 0x10C00640 = 0x10 (TOP source), divider 0x10C0180C = 0, gates 0x200000, QCH 0x3f0002 | same, `:100-101` |
| USI_CON 0x1 -> 0, USI_OPTION = 0x1, USI mode 0x2 | same, `:119,123` |
| Touch reset gpp26-0: func 0 (input), dat 0, **pud 1 = pull-down** (EXYNOS_PIN_PULL_DOWN = 1, GK `samsung.h:14`) | same, `:108` |
| Touch IRQ gpa9-0: func 0, dat 0, pud 0 | same, `:107` |
| far-alive gpa8 DAT (0x174E0044) = 0x7 -> **bit 6 (aoc2ap, `tbn,aoc2ap_gpio = <&gpa8 6>`) = 0** | keys line `SWEET KEYS: start key=1 pa=0x174e0044 ... dat=0x7`, any 0092 run; pin from GK `gs101-common-touch.dtsi:28` |

## 1. How the two working implementations drive this IP

### 1.1 Google's Linux driver, manual-CS mode (what the touch node selects)

- The touch node sets `samsung,spi-chip-select-mode = <0>` and a `cs-gpio` (GK `gs101-fts-touch.dtsi:33-37`), so the driver
  uses `MANUAL_CS_MODE` (GK-spi `:1224-1231`). [src]
- `s3c64xx_spi_hwinit` writes `CS_REG = SIG_INACT` (bit0 = 1) (GK-spi `:1444`), so 0x1 is the *idle* value. [src]
- Per transfer, manual mode: `enable_datapath()` first (PACKET_CNT, TX FIFO fill, `CH_CFG |= TX|RX`, GK-spi `:539-625`), then
  `enable_cs()` (GK-spi `:1050-1058`), which drives the CS GPIO low and then **writes `CS_REG = 0`** ("Start the signals",
  GK-spi `:627-666`, value at `:661-664`). `disable_cs()` writes `SIG_INACT` back (GK-spi `:745-760`). [src]
- Mainline gs101 uses `QUIRK_CS_AUTO` instead and only ORs `AUTO | NSC_CNT_2` into CS_REG (LX `:355-378`, port config
  `:1597-1608`); without that quirk it also writes `CS_REG = 0` on enable (LX `:364-365`). [src]

### 1.2 The Pixel's own ABL SPI driver (eSIM on USI14), [abl]

`spi_init(bus, rate)` at 0xc43c (verified): sysreg USI mode := 2 (call 0xd32c, table 0x109448, bus 11 -> 0x10C2100C), clock
via 0xd368(bus, rate*4), then `USI_CON |= 1`, `USI_OPTION |= 1`, `FB_CLK = (FB_CLK & ~3) | 1`, `USI_CON &= ~1`.

`spi_xfer` at 0xc4e4 (verified parts marked v):

1. CS GPIO low (0xc548, v), then **`CS_REG &= ~1`** (0xc554-0xc55c, v), optional delay.
2. Per chunk: `PACKET_CNT = n | 0x10000` (0xc5e4, v), TX bytes as 32-bit stores to 0x18 (0xc5fc-0xc608, v), `CH_CFG |= 3`
   (0xc614, v), poll `STATUS[23:15] >= n` (0xc620-0xc62c, v), read RX, poll `STATUS & 0x7fc0 == 0`, `CH_CFG &= ~3`,
   `PACKET_CNT = 0`, `CH_CFG |= SW_RST`, drain, `SW_RST` cleared at the next chunk (helper, not re-verified).
3. CS GPIO high, `CS_REG |= 1` (0xc738-0xc750, v).

Never written by the ABL driver (helper claim, not re-verified): CLK_CFG 0x04, MODE_CFG 0x08, INT_EN, SWAP_CFG, PENDING_CLR.
No ABL code path initialises or transfers on bus 11 (only bus 10 is init-only at 1 MHz; transfers only on the eSIM buses)
(helper). ABL contains no "touch"/"fts"/"ftm" strings (my `strings` over `abl_code.bin`).

**Clock rule** at 0xd368 (verified), for bus != 15, `hz = 4 * SCK`: reject hz > 400 MHz; if hz > 24.576 MHz
(`0x1770000`) select source 400 MHz and write mux bit4 = 1, else source 24.576 MHz (oscclk) and bit4 = 0; poll bit 16; then
`div[3:0] = src / hz - 1` (0 if src < hz), poll bit 16. Bus 11 registers: mux **0x10C00640**, div **0x10C0180C** (tables
0x109548 / 0x1095c8, verified) = the same registers as doc 19.

Google's CAL table agrees: USI 40 MHz = `{div 9, mux 1}`, 24.576 MHz = `{0, 0}`, 400 MHz = `{0, 1}` (GK
`cmucal-vclk.c:3642-3650`, params `cmucal-vclklut.c:302-325`, param order `{DIV_CLK_PERIC1_USI11_USI,
MUX_..._USER}` `cmucal-vclk.c:288-291`). [src]

**Consequence [src + hw]:** the handoff state (mux 1, div 0) means ipclk = 400 MHz (if the TOP source runs), **SCK = 100 MHz**,
ten times the FTM5 limit (`spi-max-frequency = <10000000>`, BJ `gs101-bluejay-touch.dtsi:9`). Even a draining FIFO would
give garbage until the divider is set.

## 2. Ranked hypotheses for the TX-FIFO stall

### H1 (most likely): NSSOUT was never activated in manual mode (CS_REG bit0 = 1)

- For: CS_REG = 0x1 in every A-D run [hw]; both reference implementations clear bit0 before data moves (ABL, before
  CH_CFG; Google, right after) [abl][src]; the only variant with bit0 = 0 (E) is the only one that changed STATUS [hw].
  *Guess:* the shifter does not load while NSSOUT is inactive in manual mode.
- Against: E still stalled after one byte, so H1 alone is probably not the whole story; E used AUTO mode, which mainline
  uses with bit0 left at 1 (LX `:367-371`), so bit0 may be ignored in AUTO and E's change may come from AUTO itself.
- Never tried: the exact manual sequence (GPIO CS low + `CS_REG = 0`, then the burst).

### H2: the SCK source does not run (CMU_TOP PERIC1_IP chain or PLL_SHARED2), or runs far too fast

- For: E's pattern (one byte moved to the shifter, RX level stays 0, so not one bit-time completed) fits a shifter that
  loaded on pclk and waits for ipclk edges [guess]. The USI11 mux picks the TOP source, whose gate/mux/div were never read
  [hw]; the Google boot value for `MUX_CLKCMU_PERIC1_IP` is 1 = `PLL_SHARED2_DIV2` (`cmucal-vclklut.c:344-346`, parents
  `cmucal-node.c:390-395`) [src], but whether ABL/BL2 left it on is unknown. 400 MHz ipclk / 100 MHz SCK is also outside
  anything Linux programs for this device (40 MHz) [src].
- Against: other PERIC1 users likely share the TOP source [guess]; ABL itself only ever runs USI SPIs from oscclk (1 MHz eSIM
  -> 4 MHz <= 24.576 MHz) [abl], so ABL's use proves nothing about the 400 MHz source either way.
- Decisive and cheap: move USI11 to **oscclk**, which removes CMU_TOP from the path entirely (exactly what the ABL does for
  slow buses).

### H3: RX capture / feedback clock path holds the shifter (FB_CLK, pad loopback)

- For: RX level 0 in all runs [hw]; `FB_CLK = 1` selects a feedback delay on the returning SCK (GK `gs101-fts-touch.dtsi:35`,
  written at GK-spi `:976`) [src]; if the engine pops the next TX byte only after the current RX byte completes, a dead
  feedback path would also stop TX [guess].
- Against: ABL uses the same FB_CLK = 1 [abl]; pads read func 2 [hw].
- Discriminator: self-loopback (`MODE_CFG` bit 3, `S3C64XX_SPI_MODE_SELF_LOOPBACK`, GK-spi `:73`; gs101 `has_loopback = true`,
  LX `:1605`) with FB_CLK 0 and 1.

### H4: USI wrapper state (USI_CON / USI_OPTION / sysreg), SW reset

- Against: observed USI_CON 0, USI_OPTION 0x1 (= what ABL sets), mode 2, CH_CFG.SW_RST clear [hw]; variant D toggled the reset
  [hw]. Low. CLK_CFG not latching is expected on a `clk_from_cmu` IP that neither Google (GK-spi `:772-776, :835-860`) nor ABL
  writes [src][abl]: not a cause.

### H5: PACKET_CNT / MODE_CFG / FIFO-write details

- PACKET_CNT as one `n | EN` write and byte-per-32-bit TX stores are exactly what ABL does [abl, verified]: ruled out.
- MODE_CFG: we write 0x1ff80000 (TRAILCNT, as Google hwinit GK-spi `:1464-1468`), ABL leaves the reset value 0 [abl]. Low;
  the ABL-exact variant below keeps MODE_CFG = 0 to remove the difference.

### H6: bus ownership (AOC) or pad conflict

- Against for the stall: a master does not back-pressure on its pads [guess]. Evidence that the AOC is not involved: the AOC
  firmware `aoc.bin` is loaded by the Linux AOC driver (`AOC aoc.c:238,3162`), which never runs under XNU [src -> guess
  that AOC is idle]; aoc2ap gpa8-6 reads 0 = `TBN_BUS_OWNER_AP` (`TBN touch_bus_negotiator.h:20`) [hw], caveat: its CON/PUD
  were not read, so 0 may be a pull. Linux's TBN drives ap2aoc (gpp26-1) low at probe (`TBN :258-261`, `GPIOF_OUT_INIT_LOW` at `:260`), which
  `sweettouchmux=1` already does [hw].

### Not causes of the stall, but certain blockers for the chip-id read afterwards

- **Chip held in reset** [hw + guess]: TS_RESET_L (gpp26-0, active low, GK `gs101-common-touch.dtsi:10`) is an input with
  pull-down at handoff, and phase 1 never drives it. Linux drives it output-low, `mdelay(10)`, high in probe (`FTM5 fts.c:6241-6290`, pulse at `:6280-6282`)
  and again in `fts_system_reset` (`ftsCore.c:89-91`), then polls for `EVT_ID_CONTROLLER_READY` (`ftsCore.c:97-98`).
- **Rails probably off** [guess]: LDO25M (DVDD) / LDO26M (AVDD) have no `regulator-always-on`/`boot-on` (GK
  `gs101-pmic.dtsi:535-553`), ABL has no touch code [abl], and the IRQ line (no pull) reads 0 [hw]. Linux powers them in
  `fts_probe` before GPIOs (`fts.c:6658`, `fts_enable_reg` `:6156-6181`). They sit on S2MPG10 behind ACPM: register
  `L25M_CTRL = 0x5C`, `L26M_CTRL = 0x5D` (`s2mpg10-register.h:108-109`), enable mask bit 7 (`s2mpg10-regulator.c:275-278`,
  `_MASK_7 = 0x80` `s2mpg10-register.h:334`), PMIC slave 0x01 (`s2mpg10-core.c:30`), access only via
  `exynos_acpm_write_reg(..., channel 0, ...)` (`s2mpg10-core.c:94-102`). [src]

## 3. Experiments (cheap, discriminating), as driver changes

All write only SPI11/USI11 registers, the USI11 CMU mux/divider (the two registers the ABL itself writes for this bus), and
touch pads. Each prints before/after like 0089.

| # | Boot-arg (proposed) | Change | Outcome -> meaning |
|---|---|---|---|
| R0 | none (read-only) | print `0x174E0040` CON / `0x174E0048` PUD of gpa8 (aoc2ap); print `SPI+0xdc` (ABL reads FIFO depth from `[base+0xdc] & 0x1ff`, meaning [guess]); print CMU_TOP (0092, already in flight) | gpa8-6 input + 0 with no pull -> AOC really says "AP"; TOP gate bit21/mux sel values settle H2's TOP part |
| X1 | `sweettouchcs=1` | ABL-exact transfer: no MODE_CFG/INT_EN/SWAP writes; GPIO CS low, `CS_REG &= ~1`, `PACKET_CNT = n|0x10000`, fill TX, `CH_CFG |= 3`, poll RX >= n; after: `CH_CFG &= ~3`, `PACKET_CNT = 0`, SW_RST pulse; end `CS_REG |= 1`, GPIO high | completes -> H1 was it (then do X2 anyway: 100 MHz SCK is unusable). Same 0x100-style stall -> H2/H3 |
| X2 | `sweettouchsrc=osc sweettouchdiv=5` | `0x10C00640` bit4 := 0, poll bit16 == 0; `0x10C0180C[3:0]` := 5, poll bit16; = ABL rule for SCK 1 MHz (24.576/6/4 = 1.024 MHz). Run with X1 | completes -> TOP chain or 400 MHz was the blocker (H2); still stalls with X1 -> H3 or IP-level |
| X3 | `sweettouchloop=1` | `MODE_CFG |= 1<<3` with X1+X2; then repeat with `FB_CLK = 0` | RX bytes == TX bytes (`FB 20 00 00 00`) -> engine + ipclk fine, remaining problem is board side; stall in loopback -> internal (clock/reset), not pads/AOC/chip |
| X4 | `sweettouchrst=1` | before identify: reset pin output 0, 10 ms, 1, then wait up to 1.5 s polling the FIFO for event 0x03 (FTM5 `ftsCore.c:89-98`); pads PUD := 0 as Linux (`EXYNOS_PIN_PULL_NONE`, GK `gs101-common-touch.dtsi:64-69`) | `36 48` -> chip alive, go to phase 2 |
| X5 | `sweettouchmisopu=1` | MISO PUD := 3 (pull-up) during identify | all `ff` with pull-up and all `00` with pull-down -> MISO floating -> chip unpowered (rails) or not ours |
| X6 | `sweettouchsrc=top sweettouchdiv=9` | mux bit4 := 1, div := 9 -> 40 MHz ipclk, SCK 10 MHz (ABL rule and Google LUT agree) | production rate, only after X1-X4 pass |

Do not keep `sweettouchclk=2`'s QCH write: QCH ENABLE = 0 with CLOCK_REQ = 1 (the observed 0x3f0002) already means
"REQ controls, clock requested" (GK `ra.c:513-530`); setting ENABLE hands the clock to the Q-channel handshake [src for the
bit rule, guess for the effect].

## 4. Recommended order

1. Read the `spi-top` report (CMU_TOP line) when it lands; it does not change step 2.
2. **One run: X1 + X2 (+ R0 reads)**, variants off: `sweettouch=1 sweettouchmux=1 sweettouchcs=1 sweettouchsrc=osc
   sweettouchdiv=5`. Most likely single fix [guess]; it is exactly the ABL's working sequence and rate rule.
3. If it still stalls: X3 loopback (internal vs external), then the same with the TOP source (div 9) to split oscclk vs TOP.
4. When bytes move: X4 (reset) -> identify. `36 48` -> phase 2 (`sweettouch=2`), then X6 (10 MHz).
5. If identify is all-floating after X4/X5 with loopback passing: rails are off -> ACPM IPC client for one PMIC write each
   (`0x01:0x5C |= 0x80`, `0x01:0x5D |= 0x80`; voltages are set by vsel in the same registers, leave them as found and read them
   first). The protocol is in GK `drivers/soc/google/acpm/` (read since: doc 23, with its 2026-10-22 erratum: `q_elem_size` is in BYTES, no rollback after
   publish; the shim client ships with its gates closed and a human flips them only after reading the `railsr` dump); the same client is on the GPU and Wi-Fi paths
   (docs/research/17 section 4, docs/research/20).

## 5. Alternative routes if SPI is not fixed soon

| Route | Kind | Notes |
|---|---|---|
| Power/Vol keys as navigation (keys driver already reads all three) | **stopgap** | map a press pattern to Home / unlock swipe through `sweet_hid`; no new hardware path |
| Boot-arg scripted gestures (e.g. auto swipe-up after N s) | **stopgap** | trivial, gets past the lock screen for GUI testing |
| Host-injected touch over USB (0091 gadget / `--usbnet` UDP): a kernel UDP listener feeds `sweet_hid_touch()` from a PC tool | **stopgap** | needs the first 0091 USB run; real USB, fake touch; good for driving SpringBoard tests |
| USB OTG host + HID mouse | stopgap, large | DWC3 host + xHCI + HID stack in XNU; far more work than fixing SPI |
| Touch through the AOC (TS_SPI_SEL high) | real, very large | needs Google's `aoc.bin` (cannot go in git) and the AOC IPC protocol; not reasonable |
| Fix SPI (sections 3-4) + ACPM regulator client | **real** | the only AP-side wiring: the touch IC sits on SPI11 only (GK `gs101-common-touch.dtsi:1-14`); FTM5 I2C mode exists in the source but is not wired on this board [src for the wiring, guess that no I2C path exists] |

Linux ordering to copy for the real driver [src]: `fts_probe` -> regulators on (`fts.c:6658`) -> `register_tbn` (assumes
AP owns the bus, `TBN :197-216`, called at `fts.c:6665`) -> IRQ input, reset output low, 10 ms, high (`fts.c:6241-6290`) -> pinctrl active (bus
func 2, CS **output**, PUD none, 2.5 mA = DRV 0) -> `fts_init` -> `fts_system_reset` + wait CONTROLLER_READY
(`fts.c:5338-5365`, `ftsCore.c:66-114`). SPI side before the first transfer: sysreg mode 2 (GK-spi `:1579-1597`), clocks on and
src rate = 4 x SCK (GK-spi `:835-848`), `USI_CON = 0` (GK-spi `:1416-1431`), hwinit (CS_REG = SIG_INACT, GK-spi `:1433-1474`),
then per transfer datapath + `CS_REG = 0`.

## 6. What is still unknown

- CMU_TOP PERIC1_IP state and PLL_SHARED2 state (run in flight).
- Whether NSSOUT gating (H1) is a real property of this IP or E changed something else (X1 answers).
- Rail state of LDO25M/LDO26M (no AP-readable indicator; X5 + loopback is the indirect test).
- Meaning of `SPI+0xdc` (ABL reads it as FIFO depth) and of ABL's 0xc8 bit0 (we already observe USI_OPTION = 0x1).
- Unverified helper claims: ABL's chunk tail (0xc698-0xc6ec), "never writes MODE_CFG/CLK_CFG", the list of buses ABL uses.
