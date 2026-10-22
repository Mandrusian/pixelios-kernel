# 15 - Pixel 6a (gs101) touchscreen driver, xnu patch 0088

Status: written, **not compiled in CI, not run on hardware**. Nothing in this file is "works". Evidence tags: **observed on
hardware** (none yet for this driver), **read from source** (with file:line), **guess**.

Sources read for this note (fetched 2026-10-22):

- `FTM5` = Google `kernel/google-modules/touch/fts_touch`, branch `android-gs-bluejay-5.10-android13-qpr3` (commit
  `f87fd67f`), directory `ftm5/`. `fts.c` is 7250 lines in that tree.
- `DS-gs` = Google `kernel/gs`, same branch, `arch/arm64/boot/dts/google/` (`gs101-common-touch.dtsi`,
  `gs101-fts-touch.dtsi`, `gs101-usi.dtsi`, `gs101-pinctrl.dtsi`) and `include/dt-bindings/pinctrl/samsung.h`.
- `DS-bj` = Google `kernel/devices/google/bluejay`, `dts/gs101-bluejay-touch.dtsi`.
- `Linux` = mainline v6.12: `drivers/spi/spi-s3c64xx.c`, `drivers/soc/samsung/exynos-usi.c`,
  `drivers/pinctrl/samsung/pinctrl-exynos-arm64.c`, `arch/arm64/boot/dts/exynos/google/gs101.dtsi`.

The bus, pins and chip id were already established in `05-fact-check.md` section 1; this note adds the register-level
and protocol-level facts the driver needs.

## 1. Design

One new file pair, `bsd/dev/sweet_touch_gs101.c` (kernel side) and `bsd/dev/sweet_ftm5.h` (pure protocol code: command
bytes, event decode, coordinate scaling; `shim/ftm5_host_test.c` tests it, `shim/build.sh` extracts the header from the
patch so the test sees the kernel's bytes).

- **ADT gated.** `sweet_touch_gs101_init()` is called from `bsd/dev/sweet_ufs.c` right after `sweet_usb_probe()` (the
  place the archived Redmi driver `sweet_touch_init()` was called from, 0060). It returns silently unless `/sweet-touch`
  exists with `compatible = google,gs101-ftm5-spi`. `tools/mkadt.py` adds that node only when `board.json` has
  `touch_chip_id` (bluejay); sweet's ADT is unchanged (checked: node list of `--board sweet` has no `sweet-touch`).
- **No constants in C that the ADT can carry.** Engine base, USI mode register and value, USI block offset, pad banks
  and pins, SPI function, max frequency, chip id, max coordinates and screen size all come from `/sweet-touch`, built from
  `soc/google/gs101/soc.json` (engine, USI, pad bank bases) and `device/google/bluejay/board.json` (wiring). What stays in
  C is the Samsung pad-controller register layout, the SPI engine register layout and the FTM5 protocol, i.e. things that
  are properties of the IP, not of the board.
- **Polled, late.** A kernel thread started from the init sleeps `sweettouchdelay` seconds (default 45) before it maps
  or reads anything, so a bus hang costs the GUI nothing that the persistent log did not already capture. No interrupt
  controller work. FIFO poll every 8 ms (1 ms while events keep arriving).
- **Phase 1, identify (no write to the touch IC).** Steps, each printed as `SWEET TOUCH:` lines:
  1. map; print the sysreg USI11 mode, then CON/DAT/PUD of the seven pins before anything is changed;
  2. mux check: the AP owns the bus only if gpp26-1 is an output driving low. Otherwise stop, unless `sweettouchmux=1`;
  3. USI11 to SPI mode, release `USI_CON` reset, CS pin high then output, clk/mosi/miso to function 2, SPI engine init;
  4. read the chip-id register and print four bytes. The rails (LDO25M/LDO26M) cannot be read from the AP (ACPM), so the
     identify read *is* the rail test: the printed pattern tells floating-high / floating-low / answering.
  `sweettouch=1` stops here.
- **Phase 2, only if the id answered** (default `sweettouch=2`): reset pulse on gpp26-0 (low 10 ms, high), wait up to
  1.5 s for `EVT_ID_CONTROLLER_READY` (0x03), enable active scan, poll the event FIFO and call
  `sweet_hid_touch(finger, down, x, y)` exactly like the old driver (pixel coordinates 1080x2400; SweetHID applies the
  rest, 0069 edge handling included).
- **Thread exit**: `thread_terminate(current_thread())` after every early exit (the kernel's own way for a kernel thread to end itself; the keys driver instead loops forever).

Boot-args: `sweettouch=0|1|2` (0 off, 1 identify only, 2 default), `sweettouchdelay=S`, `sweettouchmux=1`.
`tools/build-xnu-image.sh` no longer passes `sweettouch=0` (removed with the patch 0088 merge), so the driver runs in
mode 2 after the delay on every bluejay image unless `sweettouch=0|1` is appended.

## 2. Facts

### 2.1 Board wiring (all **read from source**)

| Fact | Value | Source |
|---|---|---|
| Chip | STMicro FTM5 ("ALIX"), id bytes `0x36 0x48` | DS-bj `gs101-bluejay-touch.dtsi:10`; FTM5 `fts_lib/ftsHardware.h:28-29` |
| Bus | `spi_11` = USI11_USI_PERIC1, engine `0x10D40000`, size 0x1000 | DS-gs `gs101-usi.dtsi:603-621` |
| USI mode register | `sysreg_peric1` `0x10C21000` + `samsung,usi-offset` `0x0c` = `0x10C2100C` | DS-gs `gs101-usi.dtsi:66` (sysreg), `:618` (offset) |
| USI mode value | SPI = `BIT(1)` = 2, mask UART\|SPI\|I2C = 7 | Linux `exynos-usi.c:19-25` |
| USI_CON / USI_OPTION | engine + `0xc0` + 4 / + 8; `USI_CON` bit 0 = reset | Linux `exynos-usi.c:28-34, 140-147` for the register offsets; the `+0xc0` block position is read from the mainline `usi1..` nodes (`gs101.dtsi:378`, `reg = <0x109000c0 0x20>`), but mainline has no `usi11` node, so applying it to USI11 is a **guess by analogy**. The driver writes `USI_CON` only if bit 0 reads 1 |
| Max SPI clock | 10 MHz | DS-bj `gs101-bluejay-touch.dtsi:9`; DS-gs `gs101-fts-touch.dtsi:20` |
| SPI pins | CLK/MOSI/MISO = `gpp23-0..2`, function 2 | DS-gs `gs101-common-touch.dtsi:65-70`, `gs101-pinctrl.dtsi:1024-1029` |
| CS | `gpp23-3`, GPIO output (software CS), active low | DS-gs `gs101-fts-touch.dtsi:8-14, 34`; `gs101-pinctrl.dtsi:1030-1036` (`spi11_cs` function 1). Active low is the SPI-core default for `cs-gpios` without `spi-cs-high` (**guess** that nothing overrides it; the dtsi has no `spi-cs-high`) |
| IRQ | `gpa9-0` (XEINT_20) | DS-gs `gs101-fts-touch.dtsi:23` |
| Reset | `gpp26-0` | DS-gs `gs101-fts-touch.dtsi:24` |
| AP/AOC mux | `gpp26-1` "TS_SPI_SEL", **low = AP**, high = AOC | DS-gs `gs101-common-touch.dtsi:12-20` |
| Rails | `vdd-supply` S2MPG10 LDO25M (1.8 V DVDD), `avdd-supply` LDO26M (3.3 V AVDD) | DS-gs `gs101-common-touch.dtsi:126-127`, header comment `:12-14`; `05-fact-check.md` |
| SPI mode | CPOL 0, CPHA 0, 8 bit (**guess from absence**: no `spi-cpol`/`spi-cpha` in the dtsi and no `spi->mode`/`bits_per_word` assignment in `fts.c`; grep found only `spi_setup` at `fts.c:6610`) |
| Feedback delay | `samsung,spi-feedback-delay = <1>` | DS-gs `gs101-fts-touch.dtsi:35` (written to `FB_CLK`, Linux `spi-s3c64xx.c:772-785`) |
| Max coordinates | 1079 x 2399 (panel is 1080x2400) | DS-bj `gs101-bluejay-touch.dtsi:11` |

### 2.2 Pad controller and bank addresses (**read from source**)

- Register layout (`exynos850_bank_type_off`, Linux `pinctrl-exynos-arm64.c:47`): CON 4 bits/pin at +0x00 (all banks here
  have <= 8 pins, one register), DAT 1 bit/pin at +0x04, PUD 4 bits/pin at +0x08. The alive type
  (`pinctrl-exynos-arm64.c:56`) has the same CON/DAT/PUD offsets. Function codes: input 0, output 1, 2..: DS-gs
  `samsung.h:65-67`.
- PERIC1 pinctrl `0x10C40000` (Linux `gs101.dtsi:916-918`; DS-gs `gs101-pinctrl.dtsi:897`) with `gpp23` at +0x60 and
  `gpp26` at +0xc0 (`gs101_pin_peric1` in `pinctrl-exynos-arm64.c`). ALIVE pinctrl `0x174D0000` (`gs101.dtsi:1414-1416`)
  with `gpa9` at +0xc0 (`gs101_pin_alive`). The three bank register bases are in `soc.json` as `gpp23_bank`,
  `gpp26_bank`, `gpa9_bank`.

### 2.3 SPI engine (**read from source**, Linux `spi-s3c64xx.c`)

- Registers `CH_CFG 0x00, CLK_CFG 0x04, MODE_CFG 0x08, CS 0x0c, INT_EN 0x10, STATUS 0x14, TX_DATA 0x18, RX_DATA 0x1c,
  PACKET_CNT 0x20, PENDING_CLR 0x24, SWAP_CFG 0x28, FB_CLK 0x2c` (`:29-40`); bit layouts `:42-110`.
- gs101 port config (`:1617-1628`): 64-entry FIFO, status RX level bits 23:15 / TX level 14:6, `use_32bit_io` (8-bit words
  are written as one 32-bit store per byte, `:440-475`), `clk_from_cmu` (no internal prescaler: `s3c64xx_spi_config`
  `:693-760` sets the rate through the CMU clock, `cur_speed = clk_get_rate / clk_div`, `clk_div = 4`).
- PIO flow used by the driver: flush FIFOs and software-reset (`:227-264`), set `PACKET_CNT = len | EN`, fill TX FIFO, set
  `CH_CFG` RX+TX on (`:488-545`), wait RX level >= len, read out (`:614-690`). hwinit values (`:1118-1150`).
- The driver does **not** touch `CLK_CFG` (not used on gs101) and cannot set the SCK rate; see unknowns.

### 2.4 FTM5 protocol (**read from source**, FTM5 = `ftm5/`)

- SPI build: `SPI_INETERFACE` `fts_lib/ftsHardware.h:60`. HW register read opcode `0xFB` (`:195`), write `0xFA` (`:183`),
  address 4 bytes big-endian (`fts_lib/ftsIO.c:633-664`, `ADDR_SIZE_HW_REG = BITS_32` `ftsCore.h:34`), the first byte of a
  read reply is dummy (`DUMMY_HW_REG` `:212`, `DUMMY_FIFO` `:218`). Transaction shape: one `spi_message` with a write
  transfer then a read transfer, CS held (`ftsIO.c:174-262`), `SPI_DELAY_CS` = 10 us (`ftsHardware.h:64`).
- Chip-id register `ADDR_DCHIP_ID = 0x20000000` (`ftsHardware.h:270`). The downstream tree only *compares* the DT value
  `st,dchip_id` (`fts.c:6416-6423`) and never reads this register back, so reading the id from it is **guess** (taken from
  the register map, not from a call site). Byte order is therefore also a guess: the driver accepts both orders and says which.
- Event FIFO: opcode `0x87` (`ftsHardware.h:171`), 8-byte events (`:161`), depth 32 (`:163`). `fts_interrupt_handler`
  (`fts.c:4516-4575`) reads the first event, takes `events_remaining = byte7 & 0x1f` (cap 31), reads the rest with a second
  command, stops at event byte 0 == 0, dispatches on `byte0 >> 4` (`:4570`).
- Event IDs `0x03` controller ready, `0x13` enter, `0x23` motion, `0x33` leave (`ftsSoftware.h:377-383`). Enter/motion
  packing (`fts.c:3059-3069`): `id = byte1 >> 4`, `type = byte1 & 0xf`, `x = (b3 & 0xf) << 8 | b2`, `y = b4 << 4 | b3 >> 4`,
  pressure `b5`. ids >= 10 are rejected (`fts.c:3061-3066`, `TOUCH_ID_MAX` `fts.h:203`); coordinates clamp to the max
  (`fts.c:3089`). Types: finger 1, glove 2, palm 4, grip 7, hover 0 (`ftsSoftware.h:550-556`).
- Reset: `fts_system_reset` with a reset gpio drives it low, `msleep(10)`, high, then waits for controller-ready
  (`fts_lib/ftsCore.c:66-108`). Enabling scan: `setScanMode(SCAN_MODE_ACTIVE, 0xFF)` (`fts.c:5624-5627`) sends the 7-byte
  `0xFA 0x20 0 0 0 0 0` and then `0xA0 0x00 0xFF` (`ftsCore.c:339-366`; the first write's purpose is not explained in the
  source, it is copied as is).
- Palm and grip are reported to Android as `MT_TOOL_PALM` (`fts.c:3122-3138`); the driver releases such a contact instead.
  Stylus (type 3) is treated as a finger (**guess**: `STYLUS_MODE` is not known to be defined).

### 2.5 What is *not* a fact

- Nothing about the phone's real state at XNU handoff has been read.
- Existing observations that matter: none for touch. (`hardware/probe-log-2026-10-21-run1.txt` and the HANDOFF have no
  touch pad, USI11 or SPI11 dump.)

## 3. Unknowns only a hardware run answers

1. **Is the SPI11 bus owned by the AP at handoff?** `gpp26-1` is printed before anything is touched. If it is not an output
   driving low, the driver stops and prints how to override (`sweettouchmux=1`). Whether the AOC (always-on coprocessor)
   fights for the bus afterwards is not testable offline.
2. **Are LDO25M/LDO26M on?** They live on the master PMIC S2MPG10 behind ACPM; the AP cannot read them. A floating bus
   (all `ff` or all `00` in the identify line) means off or in reset; the fix would then be an ACPM client (not written).
3. **Is the USI11 clock running and what rate?** The clock comes from the CMU (`VDOUT_CLK_PERIC1_USI11_USI`,
   `GATE_PERIC1_TOP0_USI11_USI`, `gs101-usi.dtsi:~610`). Its gate and divider at handoff are unknown and not read (no
   verified CMU register offsets here). If the gate is closed, the first read of an SPI/USI register can raise an SError or
   hang the SoC - that is why the thread starts late and prints each step *before* it happens. SCK = ipclk / 4
   (`gs101_spi_port_config.clk_div`); if ipclk > 40 MHz the bus exceeds the IC's 10 MHz and identify returns garbage. The
   driver prints `CLK_CFG` and the status register; a divider setting via CMU would be the next patch.
4. **State of the USI11 sysreg and pads at handoff.** Printed first. The driver writes the USI mode (SPI) and the pad
   functions, and nothing else on the AP side.
5. **Chip-id register layout** (see 2.4) and whether the `0xFB` read works without the vendor's earlier init sequence.
6. **Does the IC scan without the vendor init?** The driver does only reset + `setScanMode`. The vendor `probe` also runs
   firmware/limits checks, `readSysInfo`, `fts_chip_initialization` (`fts.c:121`) and feature settings. If flash holds a valid
   firmware the IC probably scans after reset (**guess**); if it prints `controller ready` but no touch events, this is the
   suspect.
7. **Event cadence without the interrupt line.** Polling reads the FIFO every 8 ms; if the IC wants the INT line serviced
   to refill its FIFO, events might stall (the line level is printed in the pin dump only, never used).
8. **Whether pad DAT of an EINT-function pin (`gpa9-0`) reads the level.** Only printed, not relied on.
9. **Coordinate orientation / HID mapping.** The panel max (1079x2399) equals the screen minus one, so mapping is the identity
   (host-tested); that SweetHID then lands the finger under the glass correctly is the same path the Redmi driver used
   (0058/0060/0069) but untested on this panel.

## 4. Run plan (driver runs it; the phone is touched only by the human/driver, `adb`/`fastboot` only on a `bluejay` device)

Build:

1. CI: `gh workflow run xnu.yml`, wait for green, `gh run download <id> -n xnu-kernel` (the patch has not been through CI).
2. `python3 -I tools/mkadt.py A.adt --board bluejay` (needs `research/` as for the other runs) - the ADT must contain
   `/sweet-touch` (`python3 -I tools/adt.py A.adt` style dump, or the check used in this change).
3. `tools/build-xnu-image.sh` no longer passes `sweettouch=0`. PE_parse_boot_argn keeps scanning after a match, so the
   last occurrence wins (**read from source**, pexpert/gen/bootargs.c, loop continues at `gotit`): an appended `sweettouch=1` overrides. Use `--qr off` so the screen keeps the GUI.

Boot, in this order, one boot each, reading the persistent log / USB log for `SWEET TOUCH:` lines:

| Run | Extra boot-args | Question | Read |
|---|---|---|---|
| A | `sweettouch=1 sweettouchdelay=45` | AP owns bus? pads/USI state? does anything answer? | `AP/AOC mux:` line, `pin` lines, `sysreg USI11 mode`, `SPI regs`, `identify:` line |
| B | only if A stopped at the mux: `sweettouch=1 sweettouchmux=1` | does it answer once the mux is forced? | `identify:` line |
| C | only if an `identify:` said `FTM5 answers`: no `sweettouch` arg (= 2), `sweettouchdelay=45` | reset, ready event, events | `controller ready`, `ev ...` lines, `SWEET HID: touch N down/up at x,y` |

Decode of the `identify:` line: `FTM5 answers` -> go to C. `no answer, MISO high` -> rails off, chip in reset or bus not
ours (check the A mux line; needs ACPM if rails). `no answer, MISO low` -> same, or the pad is not the SPI function
(compare the pin lines). `unexpected value` -> clock too fast/slow or wrong register layout: record the four bytes. A boot
that dies right after `mapping SPI` or `USI_CON=` points at an ungated-clock access (unknown 3). Between runs record
results in `docs/HANDOFF.md` with the build id, tagged *observed on hardware*.

Not covered: Wi-Fi (milestone 2), multi-touch beyond what SweetHID already supports, suspend/resume, firmware loading.

## 5. CMU clock step (patch 0089, `sweettouchclk`)

*Observed on hardware* (xnu-0092 run, HANDOFF 2026-10-22): USI11 mode/pad/SPI register reads work, the first SPI transfer
times out. *Guess*: SCK source gated. Chain and sequence: `docs/research/19-gs101-usi11-clock.md` (section 6). Addresses and bit
numbers are in `soc/google/gs101/soc.json` `cmu_peric1` (sources there), reach the driver through ADT `/sweet-touch`
(`cmu-reg`, `cmu-*` offsets, `cmu-*-bit`); nothing is hard-coded in C. Without those properties the driver skips the CMU.

Boot-args (all read at start, together with `sweettouch*`):

| Arg | Effect |
|---|---|
| (none) | `SWEET TOUCH: cmu ...` lines are always printed after the MMIO mapping, before any write (reads only) |
| `sweettouchclk=1` | after the mux check, before the SPI engine init: USI11 user mux bit 4 (BUSY bit 16 polled for at most 1 ms; skipped if already 1), then gates ipclk_4, pclk_4, rstnsync: set MANUAL (20), set CG_VAL (21), clear AUTO (28). Each write is read back and printed `before -> wrote -> after`; a mismatch aborts the rest of the sequence (the SPI setup and identify still run) |
| `sweettouchclk=2` | as 1, then QCH `|= ENABLE | CLOCK_REQ` (doc 19 step 6) |

Never written: OPTION `0x800` (doc 19 step 7) and the divider `0x180C`.

What to read in the log:

- `cmu base 0x10c00000: OPTION[0x800]=.. usi_mux[0x640]=.. bus_mux[0x600]=.. bus_auto[0x604]=.. usi_auto[0x644]=.. div[0x180c]=..`
  and `cmu gates: ipclk_4[0x202c]=.. pclk_4[0x204c]=.. rstnsync[0x2068]=.. QCH[0x302c]=..`: mux bit 4 = 0 means oscclk (24.576 MHz)
  and not the TOP source; gate bit 20 = 0 means MANUAL off (bit 28 decides); bit 21 = CG_VAL; divider bits [3:0] = divratio.
- With `sweettouchclk>=1`: `cmu <name> [off]: before -> wrote -> after` lines, then `cmu: clock sequence done` or the abort line.
- On an identify timeout: `identify timeout: SPI status=.. ch=.. clk=..` followed by the full `cmu` lines again (compare with the
  first dump to see whether anything changed the gates behind the driver).

Run plan: run A (`sweettouch=1 sweettouchdelay=45`, add `sweettouchmux=1` if A stops at the mux) to get the baseline dump; then
`sweettouchclk=1`; then `sweettouchclk=2` only if still timing out. Record results in `docs/HANDOFF.md` tagged *observed on hardware*.
Not hardware-tested: the sequence, and whether the TOP-level chain (CMU_TOP `0x1E0810E8/0x1E0818E0/0x1E082104`) is on (never written).

## 6. SPI engine does not transmit (patch 0090, `sweettouchvar`)

*Observed on hardware* (HANDOFF 2026-10-22, patches 0088+0089): CMU_PERIC1 chain already open (OPTION 0xf1000000, USI11 mux sel 1, gates
0x200000, QCH 0x3f0002), USI11 sysreg mode 2, USI_CON reset bit was 1 and is cleared, pads clk/mosi/miso func 2, SPI registers writable
(MODE_CFG 0x1ff80000, FB_CLK 1). `tg_spi_burst()` times out with STATUS=0x140 = TX level 5 (bits 14:6), RX level 0: the 5 command bytes
are in the TX FIFO and never shift out. Not yet run: patch 0090 itself (compiled with `clang -fsyntax-only` against stub headers only).

What 0090 prints (always, after the SPI setup, before identify; reads only): `diag after setup:` USI_CON, USI_OPTION, USI mode; CH_CFG, CLK_CFG,
MODE_CFG, CS_REG, INT_EN, STATUS; PACKET_CNT, PENDING_CLR, SWAP_CFG, FB_CLK; then `pin clk|mosi|miso|cs` func/dat/pud.
With `sweettouchvar=1` and a failed first identify it runs variants A..E in order (each: change, SPI engine reset = hwinit + flush, CS low,
one 5-byte burst with its 20 ms bound, CS high; E uses no CS GPIO, see below), prints `VARIANT X: completed|timeout, status= ch= (at timeout ...) clk=`, stops at the
first completed one and identifies again. Variants are cumulative (A's CLK_CFG, C's USI_OPTION stay set for the later ones; E is last so it also runs with them). The run
plan: `sweettouch=1 sweettouchdelay=45 sweettouchvar=1`. Only SPI/USI registers and the CS pad are written; nothing dangerous.

Sources, Linux master via git.kernel.org (fetched 2026-10-22; line numbers of that copy):

| Variant | What | Linux source | Does Linux do it on gs101? |
|---|---|---|---|
| A | `CLK_CFG` (0x04) bit 8 ENCLK, bits 7:0 PSR = 3 | spi-s3c64xx.c `S3C64XX_SPI_ENCLK_ENABLE` :52, `PSR_MASK` :53, `s3c64xx_spi_config` :758-:766 | **No**: only `if (!clk_from_cmu)`; `gs101_spi_port_config` :1597-:1608 has `clk_from_cmu = true`, `clk_div = 4`, `quirks = CS_AUTO` |
| B | CH_CFG TX/RX on before the FIFO load | `s3c64xx_enable_datapath` :490-:557: `s3c64xx_iowrite_rep` :523 fills the FIFO, `writel(chcfg, CH_CFG)` last :555 | Linux order = **FIFO first, channel on last**. The normal path in 0088 already does that; B is the reverse |
| C | `USI_OPTION` (usi+0x08) bit 1 CLKREQ_ON set, bit 2 CLKSTOP_ON clear, after USI_CON reset released | exynos-usi.c `USI_OPTION` :42, bits :46-:47, `exynos_usi_enable` :173-:197 | **No**: only if DT has `samsung,clkreq-on` (:264); the gs101 usi11 node (gs101.dtsi :1469-:1481) has none |
| D | USI_CON reset set then cleared, SW_CONF written again (USIv2 SPI = BIT(1) = 2) | exynos-usi.c `USI_CON` :41, reset bit :45, `exynos_usi_unconfigure` :218-:243, `exynos_usi_set_sw_conf` :141-:158, `USI_V2_SW_CONF_SPI` :35 | Linux writes the mode once, then releases reset once |
| E | `CS_REG` (0x0c) = `CS_AUTO` (bit 1) \| `NSC_CNT_2` (2 << 4); cs GPIO not driven; cs pad switched to function 2 for this variant only, back to GPIO output/high afterwards | spi-s3c64xx.c `s3c64xx_spi_set_cs` :355-:377 (write at :369-:371), `CS_NSC_CNT_2` :70, `CS_AUTO` :71, `QUIRK_CS_AUTO` :24 set in `gs101_spi_port_config` :1607; pad: DS-gs `gs101-pinctrl.dtsi:1030-1041`: `spi11_cs` = gpp23-3 function 1 (GPIO out), `spi11_cs_func` = function 2 (engine NSSOUT) | **Yes**, per transfer |

Note on E: the task said the CS pad "if it must be func 1 `spi11_cs`"; the vendor pinctrl has both, and function 1 is the GPIO output
(what 0088 uses). Engine-driven CS needs `spi11_cs_func` (function 2 = the ADT `spi-pin-func`), so E sets function 2 and prints `pin cs(E)`.
That pairing (engine CS with function 2) is a *guess* about the hardware; the file says nothing about which one Linux selects with `CS_AUTO`.

Confirmed from source: the USI register block is at SPI base + 0xc0, not a guess. gs101.dtsi :1469 `usi11: usi@10d400c0` `reg = <0x10d400c0 0x20>`,
child `spi_11: spi@10d40000` `reg = <0x10d40000 0x30>`; USI_CON = +0x04 and USI_OPTION = +0x08 of that block. The STATUS bit layout
(TX level 14:6, RX level 23:15) is `S3C64XX_SPI_ST_*_FIFO_RDY_V2` :82-:83.

### Best explanation of why the TX FIFO does not drain, ranked

All tags: *guess* unless a source is named; none is confirmed by a hardware result yet.

1. **CS_REG manual mode / NSSOUT.** Evidence for: Linux on gs101 runs with `S3C64XX_SPI_QUIRK_CS_AUTO` (:1607) and `s3c64xx_spi_set_cs`
   :357-:377 then writes `CS_REG |= CS_AUTO | NSC_CNT_2` (bits 1, 5:4) and never writes `SIG_INACT`; 0088 never writes CS_REG at all,
   leaves it at its reset/ABL value (`hwinit` :1127-:1129 would write `SIG_INACT` = 1 only for non-CS_AUTO ports) and drives CS as a
   plain GPIO. If CS_REG.bit0 (NSSOUT inactive) is set and AUTO is off, the engine may hold the shifter. Evidence against: on the older
   Samsung IP the master clocks regardless of NSSOUT. The `cs=` value in the diag line decides it; **covered by variant E only** (the task
   named four then). Now variant E (`CS_REG = CS_AUTO | NSC_CNT_2`, engine CS); `CS_REG = 0` is not tried.
2. **Missing USI_OPTION clock request (variant C).** Evidence for: with CLKREQ_ON clear the USI block may gate its own clock whenever
   it thinks it is idle (the comment in exynos-usi.c :186-:188: "Continuously provide the clock to USI IP w/o gating"); the CMU
   QCH (0x3f0002: ENABLE 0, CLOCK_REQ 1, IGNORE_FORCE_PM_EN 0, EXPIRE_VAL 0x3f) can still gate. Evidence against: mainline gs101 does not set it, and
   register access works (pclk). The diag line shows USI_OPTION as ABL left it.
3. **SCK source not running although the PERIC1 gates are open (CMU_TOP).** Evidence for: the PERIC1 dump says the chain below is open,
   but the TOP gate/mux/divider (CMU_TOP 0x1E0810E8, 0x1E0818E0, 0x1E082104; doc 19 section 1) were never read; only the AMBA clock
   (pclk) is needed for register access, SCK needs ipclk. Evidence against: other PERIC1 USI users share that source. Cheap follow-up: dump the three
   CMU_TOP registers (needs ADT properties via mkadt, like 0089).
4. **ENCLK / PSR (variant A).** Evidence against: Linux does not set it on gs101 (`clk_from_cmu`); the register may not even exist there. Evidence for: it is the one bit of the
   original Samsung IP that gates the SCK divider, and `CLK_CFG` is only dumped, never decided, by 0088 (observed value is in the log's
   `clk=` field, not recorded in this document). Cheap to test, so it stays.
5. **Pad not routed / AOC owns the bus.** Evidence against: the mux pin check passed (the AP drives it low, observed) and the SPI pads
   read func 2; an unrouted pad would still shift the TX FIFO out (it only loses the data). Not a cause of a stuck TX level.
6. **Channel order (variant B).** Evidence against: 0088 already uses the Linux order; PIO FIFO-first is what the mainline driver does on this exact IP.
7. **USI reset toggle / SW_CONF ordering (variant D).** Evidence for: the mode was written before USI_CON was released, in Linux the
   order is the same (`configure` :200-:215 mode then enable); low likelihood, included because it is free.

What I guessed: the ranking (no hardware data behind it); that `PSR = 3` is a harmless value; that CS handling belongs at rank 1 (read from
Linux, not shown on this phone); the variant order (cheapest-first was not applied, they follow the task list).
