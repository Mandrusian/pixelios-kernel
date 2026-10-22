# Automation scripts (xrun, ci-kernel, stack-apply)

Three scripts replace the by-hand test loop. Each prints ONE report at the end (about 40 lines, no progress spam).
Line 1 is always `RESULT: <PASS|FAIL|PARTIAL|ERROR> <one sentence>`; read that first. Progress goes to a log file whose
path the report prints. Exit codes: **0** PASS, **1** FAIL or PARTIAL, **2** ERROR (the tool could not do its job:
no device, no network, bad arguments). `tools/pix xrun|ci|stack` are wrappers.

Evidence tags: everything in these reports is *observed* by the script on this run (camera, USB, log text, CI);
what a result *means* for the port is still the driver's judgement.

## tools/xrun: one hardware cycle

```
tools/xrun --kernel K [--adt A | --board bluejay] [--cpus N] [--qr S|off] [--restart S] [--args "boot-args"] [--rw] [--gui] [--usbnet [--usbdma]]
           [--expect REGEX]... [--expect-absent REGEX]... [--duration S] [--label NAME] [--no-flash-check]
           [--early] [--stop-on REGEX]... [--hard-timeout S] [--reader "qr-log.py options"]
tools/xrun --dry-run --from-dir logs/runs/<dir> [--stop-on REGEX]...   # rebuild the report of an old run, no phone (copy the dir first: report.txt/merged.log are rewritten)
```

Early stop (*read from source*, not yet run on hardware): `--stop-on REGEX` (repeatable; `--early` = all `--expect` patterns) ends the capture as
soon as every pattern is in the merged QR/usbnet log (checked every 10 s), instead of waiting for the self-reset. Then, if the phone is on USB (18d1)
xrun runs `to-fastboot.sh 60`; if not, the report gets ONE line `PHONE: still running; self-reset due in ~Ns / Power+VolDown hold`. RESULT is PASS when the
expects matched, with `EARLY: stopped at +Ns (full run would have waited Ms)`. A dry-run replay with `--stop-on` truncates the log at the moment the last
pattern first appeared and prints `EARLY: NOT reached ...` when it never would have. `--hard-timeout S` caps the whole run from xrun start (default: none, `--duration` still
applies); a timeout FAIL says `HARD TIMEOUT` and carries `PHONE: usb ids [...] adb [...] fastboot product [...]` from a bounded probe (lsusb 5 s, adb 8 s, fastboot 8 s).

Cycle: `to-fastboot.sh` (ERROR `needs Power+VolDown hold` if the phone is not reachable, ERROR if it is not bluejay) ->
`mkadt.py --board` -> `build-xnu-image.sh` -> camera/QR capture for the whole run -> `fboot.sh --expect xnu` -> waits on
its own until the phone re-appears on USB (vendor 18d1; the self-reset worked) or `--duration` (default restart+120 s)
runs out -> report. `--no-flash-check` skips the to-fastboot step (phone already in fastboot).
`--usbnet`: boot-args `sweetusb=2` (`--usbdma` adds `sweetusbdma=1`), xrun starts `tools/usbnet-log.py` (UDP 5140, no root) and prints the one sudo line
(`tools/sweet-usbnet.sh --stage`) the user runs once the phone enumerates; the report merges `usbnet.log` with the QR log (docs/research/09).
Everything lands in `logs/runs/<UTC>-<label>/` (gitignored): `report.txt`, `report.json`, `merged.log`, `snaps/` (raw QR
snapshots), `frames/` (every 4th camera frame + the last 20), `run.json`, `A.adt`, `image.meta.json`, `xrun.log` (progress).

Sections, fixed order:

| Section | Read it as |
|---|---|
| `RESULT` | PASS = every `--expect` found, every `--expect-absent` absent, no panic marker, reset observed (if `--restart`). FAIL = one of those broke, with a log that starts at boot and has no gaps. PARTIAL = an `--expect` is missing but the log is incomplete (the line may be outside the snapshots) or there is no QR log at all |
| `RUN` | kernel sha256, git head, boot-args, cpus/qr/restart; boot -> first QR snapshot; `reset observed at +Ns` or `NOT observed`; **log coverage** (snapshots, gaps, first/last line) |
| `EXPECT` | per regex: FOUND with the first matching line and its time (the snapshot that first contained it, seconds after the boot command), or MISSING |
| `MARKERS` | built-in counters (restart armed, UFS errors, display on, panic, lockscreen, SpringBoard/backboardd lines, check-in timeouts), FB swap count, `SWEET TOUCH/KEYS` lines (max 12), `SWEET CPU` lines |
| `CRASHES` | per process: count, signal, first pc, reason (from `CRASH <proc>[pid] sig N`) |
| `SCREEN` | mean camera luma per frame as segments (`bright 0-95 s, black from 97 s`) and `black-at`. No ML: mean luma < 25 = black |
| `DIFF` | markers/crashes that appeared or disappeared since the previous run with the same `--label` |

Limits you must know:
- **QL2** (kernel default, `sweetqr_fmt=2`, docs/research/24): the whole log from its first byte, compressed, loss-tolerant; the reader keeps
  `snaps/qr2-<sid>.txt` + `.json` (the log so far, `[...gap...]` where parts have not arrived yet). The report's "log coverage" line says
  `QL2 1 session(s): N of M reduced bytes recovered, blocks, frames, gaps`; with gaps an `--expect` that is missing is PARTIAL, not FAIL (the line may be in a gap). Wait longer
  (`--duration`) or use `--stop-on` to let more repair frames arrive. Camera/decoder knobs go in `--reader "--workers 3 --tiles 2 --average 3"`, phone-side
  knobs in `--args "sweetqr_ver=12 sweetqr_ms=500 ..."` (docs/QR_LOG.md). QL1 (`--args sweetqr_fmt=1`): only the **tail** (last 12 kB, digit-only-different
  repeats dropped), one snapshot per ~48 s; early/mid-boot lines are lost there.
  A run that must see lines while the phone is still alive has to start the QR takeover before that moment (`--qr SECONDS` counts from the root
  mount; QL2 reads the log from the start in the background, so a late takeover does not lose early lines). With `--qr off` there is no log: only reset and screen
  evidence, so `--expect` gives PARTIAL. Crash counts are lower bounds when the log has gaps.
- QR takes over the screen, so SCREEN `black`/`bright` during a QR run says little about the GUI.
- Times are relative to the `fastboot boot` command; snapshot time = when the camera finished decoding it (a few seconds
  after the phone drew it).

## tools/ci-kernel: the kernel for a commit

```
tools/ci-kernel [--commit SHA|HEAD] [--dispatch] [--expect-string STR]... [--out DIR] [--wait MIN]
```
Finds the `xnu.yml` run for the commit (waits up to 25 min), downloads `xnu-kernel` to `~/pixelios-artifacts/xnu-<short>/`
(reuses an intact earlier download), checks `SHA256SUMS`, runs `strings -a` for each `--expect-string`, prunes
`xnu-<7 hex>` dirs beyond the 3 newest (not with `--out`; dirs named `xnu-0089` are never touched). A failed CI run is
FAIL plus the last 25 lines of the failing step. Report: RESULT, RUN (id, url), KERNEL (path, sha, size), one line per string, PRUNED.

## tools/stack-apply: do the patches apply?

```
tools/stack-apply [--only-new PATCH] [--keep]
```
Applies the three stagers and every `xnu/patches/*.patch` in order on a pristine `xnu-8796.141.3` (shallow clone cached in
`~/pixelios-artifacts/xnu-pristine`, scratch copy under `~/pixelios-artifacts/scratch/`, deleted afterwards). Takes about 10 s plus the
stager downloads. FAIL names the first failing patch and prints 10 lines of the `git apply` error. `--only-new 0088`
still applies everything but states the verdict for that patch. It does not build; CI does.

## tools/phone-state: what state is the Pixel in?

```
tools/phone-state [--expect android|recovery|fastboot|none] [--wait S]   (also: tools/pix phone)
tools/phone-state --self-test     # stub lsusb/adb/fastboot on a fake PATH, incl. a hanging adb; run by check-all.sh
```
Every probe runs under `timeout` (8 s max; a bare `fastboot getvar` against a phone in recovery hangs 120 s). Order: `lsusb` for
18d1; `adb devices` + `getprop ro.product.device`/`sys.boot_completed`; `fastboot devices` + `getvar product` only when the usb id
is 18d1:d001 or 18d1:4ee0 (guess: other tools only assume vendor 18d1). Any non-bluejay device is `state=other`
(`OTHER-DEVICE (not touched)`). Last line: `RESULT: PASS|FAIL|ERROR state=fastboot|recovery|android|booting|absent|other product=bluejay|unknown`,
exit 0/1/2; ERROR = a probe timed out. Without `--expect`, PASS means "bluejay present". `--wait S` re-probes every 2 s (cap 300).
Serials are never printed. `to-fastboot.sh`, `fboot.sh` and `xrun` do not use it yet (own refusal/loop logic; swapping is not trivial).
Verified with stubs only (self-test), not on hardware.
## tools/tool-proposals: the proposal register

```
tools/tool-proposals list [--index]            # id, tool, status; --index = the table in docs/TOOL-PROPOSALS.md
tools/tool-proposals check NAME_OR_KEYWORD     # also: tools/pix proposals ...
tools/tool-proposals new NAME [--extends X] [--related TP-NNN] [--force]
tools/tool-proposals validate
```
`check` searches `tools/` file names, `tools/pix` subcommands, the headings of this file and the `tool`/`usage` fields of every proposal; one line
per hit (`EXISTS tools/x`, `PROPOSED TP-NNN (status)`), last line `RESULT: EXISTS|ALREADY-PROPOSED|NONE ...`, exit 0 NONE / 1 hit / 2 error.
`new` refuses on a hit unless `--force`, writes the next free `docs/tool-proposals/TP-NNN-slug.md` from the skill template and prints the
path; it does not edit the index, and the placeholders make `validate` fail until they are filled. `validate` checks fields, status values,
ids (unique, contiguous, match the file name), `extends`/`related` targets, the five sections, and that the README index equals
`list --index` (an `(amended)` suffix on the status is generated from a non-empty `## amendments`). Last line `RESULT: PASS|FAIL ...`, up to 10
problems, exit 0/1/2. `tools/validate-repo.py` runs it, so the gate fails on a broken register. Unit-tested (`tools/test_tool_proposals.py`).

## tools/hang-recover: wait for the phone after a reboot, bounded (TP-006)

```
tools/hang-recover [--wait S] [--expect fastboot|recovery]   (default 90 s, cap 300; also: tools/pix recover)
tools/hang-recover --self-test    # stub lsusb/adb/fastboot; run by check-all.sh (hang-recover step)
```
*Observed*: after a shim probe or an XNU run the phone leaves USB for seconds to a minute while it reboots, then shows up as
recovery; an early "hang" verdict was wrong. So: loop until the deadline, `tools/phone-state` every 3 s (never a bare adb/fastboot),
one line per state change (`+12s recovery`). Seeing recovery/android with `--expect fastboot` runs `tools/to-fastboot.sh` once
(bounded 60 s). Last line `RESULT: PASS state=fastboot after Ns` (exit 0), or `RESULT: FAIL still absent after Ns` (exit 1) followed by
"Hold Power+VolDown ~10 s until the bootloader screen / fastboot shows, then rerun tools/hang-recover" and "if the screen shows a
bootloader menu but USB is absent, replug the cable". A non-bluejay device = exit 2, not touched. Software only: the relay/hardware
reset half of TP-006 is not built (open for the user's decision). Verified with stubs only (self-test), not on hardware.

## tools/check-all.sh: the quality gate

`tools/check-all.sh` (full) is what must print `GATE PASS <hash>` before a push. Output is one line; `shim/out/gate.log` has the
full log plus a per-step timing table. Two lanes run in parallel (cap of 2 jobs): lane A = the six bluejay shim builds (they share
`shim/out`, so they stay serial) + host-tests + boot-safety, lane B = unit, rootless, validate. The log is merged A then B, so it is
deterministic. Measured on the dev machine (*observed*, 2026-10-22, 01a978f): 50 s serial -> 37 s with lanes; the builds
(6 x ~6 s) dominate.

`tools/check-all.sh --quick` is for iterating: it diffs the tree against the commit in `shim/out/gate.stamp` (written only by a
full PASS on a clean tree) and runs only the needed steps, selected by `tools/gate_select.py` (tested by `tools/test_gate_select.py`):

| Changed paths | Steps |
|---|---|
| `shim/`, `soc/`, `device/`, `tools/imgmeta*`, `tools/mk*`, `tools/test-bluejay-boot.py`, `tools/check-all.sh`, xnu patches 0087/0088 (the host tests extract headers from them) | 6 builds, host-tests, boot-safety |
| `tools/`, `soc/`, `device/` | unit, phone-state, hang-recover, rootless |
| `tools/`, `soc/`, `device/` | unit, phone-state, probe-run, rootless |
| anything (docs, other `xnu/` patches) | validate (patch numbering/header format, script syntax), always |

No stamp, or a stamp that is not an ancestor of HEAD: the full gate runs. A quick PASS line says `quick: ran N of 15 steps`;
it is not a substitute for the full gate before a push.

## Runtime MMIO pokes: `sweettouchpoke=` (xnu patch 0093)

Test a register hypothesis without a CI kernel rebuild: `tools/xrun ... --args "sweettouchclk=3 sweettouchpoke=r:10c00000;m:1e080000=1=1;d:1000;r:1e080000;"`.
Script = hex ops, **every op ends with `;`** (a script cut by the boot-args limit is a syntax error), no spaces: `r:ADDR;`, `w:ADDR=VAL;`,
`m:ADDR=MASK=VAL;` (read-modify-write), `d:US;` (0..1000000). Runs in the touch thread after `tg_cmu_enable()` and right before the SPI setup
(skipped with one line when the mux check leaves the bus alone); each op prints `SWEET TOUCH: poke N ...`. `w` does not read first; inside
SPI11 neither `w` nor `m` reads back (RX_DATA pops on read).
ADDR: physical, 4-aligned, inside SPI11 (0x1000), the USI11 sysreg page (0x1000 size is a guess), CMU_PERIC1, CMU_TOP or a touch pin bank.
CMU_PERIC1 and CMU_TOP are read-only except the words in the ADT: PERIC1 usi mux, bus mux, both auto regs, ipclk/pclk/rstnsync gates, QCH
and DIV (OPTION never), TOP mux/div/gate. Max 32 ops, 240 characters; anything bad prints one `poke: rejected (...)` line and runs nothing.
The whole boot-args line is 607 characters; `tools/mkpayload.py` now exits with an error beyond that. Parser and range table:
`bsd/dev/sweet_touch_poke.h`, tested by `shim/touchpoke_host_test.c`. Run on hardware (2026-10-22, runs touchx3 and later: `poke: 3 ops` lines, reads back the written values); a string without the final `;` is rejected (AP-10).

## tools/probe-run: one shim probe cycle, one report

```
tools/probe-run [--touch-probe[=OPTS] | --build-args "..."] [--expect REGEX]... [--timeout S] [--label NAME]   (also: tools/pix probe)
tools/probe-run --dry-run --from-dir DIR [--expect REGEX]...   # re-read DIR/usb.txt, writes nothing
tools/probe-run --self-test      # stub phone-state/to-fastboot/fboot/build/usb-log; run by check-all.sh
```
Steps, all bounded: `phone-state --expect fastboot --wait 20` (else `to-fastboot.sh` once, else ERROR with the instruction) -> `cd shim && ./build.sh
--board bluejay --avb --usb-gadget ARGS` (`--touch-probe` adds `--pstore-fast`, as the section below) -> `usb-log.py capture` in the background
**before** -> `fboot.sh --expect probe shim/out/shim-boot-avb.img` -> poll the log until every `--expect` regex matches or `--timeout` (60 s; default expect
for `--touch-probe`: `SWEET TOUCH RESULT`) -> wait up to 30 s for the phone back in fastboot or recovery (recovery -> `to-fastboot.sh`). The phone
legitimately leaves USB while the shim reboots, so absence right after the result line is normal: PASS with a "not back" note.
Run dir `logs/probe/<UTC>-<label>/`: `usb.txt`, `build.log`, `fboot.log`, `report.txt`. Report: `RESULT: PASS|FAIL|HANG|ERROR ...`, up to 12 key lines
(touch probe: the `SWEET TOUCH RESULT` lines, then `VARIANT`/`identify`/`loopback`/`rst`/`ACPM` lines), `dir: ...`. Exit 0 PASS / 1 FAIL or HANG / 2 ERROR.
FAIL = line missing, phone back. HANG = line missing and phone not back in 30 s (hold Power+VolDown). ERROR = not in fastboot at start, build or fboot failed.
Only bluejay (fboot.sh/phone-state refuse others); never raw `fastboot boot`. Verified with stubs and a dry-run of a real capture only, not on hardware.

## shim --touch-probe: touch SPI11/USI11 bring-up in seconds, no iOS boot

```
cd shim && ./build.sh --board bluejay --avb --pstore-fast --usb-gadget --touch-probe[=run[,mux][,cs][,osc|,top][,loop][,rst][,misopu][,railsr|,rails]]
tools/fboot.sh --expect probe shim/out/shim-boot-avb.img     # then read the RAM/USB/pstore log as for the other probes
```
Source `shim/touchprobe.c/.h`, host test `shim/touch_host_test.c` (run by `build.sh`), constants from `soc.json`/`board.json`
through the generated `TOUCH_*` macros (`tools/boards.py`), the same keys `tools/mkadt.py` puts into `/sweet-touch`. Options are
build-time (the shim builds in seconds); an unknown word, `osc` with `top`, or options without `run` make `build.sh` exit 2.
- `--touch-probe`: phase 1, **reads only**: CMU_PERIC1 + CMU_TOP regs, the USI11 sysreg mode, the seven pads. The USI/SPI registers
  are read **only if the sysreg mode already is SPI** (at handoff it is 0: `diag before: skipped: usi mode=0`; a hang cannot be
  caught by `try_read32`). A bus-faulting register prints `unreadable pa=...` and the dump goes on.
- `--touch-probe=run`: phase 2 after a pstore checkpoint, the writes of XNU `sweettouchclk=3 sweettouchvar=1` (patches 0088-0092:
  CMU_TOP gate, USI mux + three gates + QCH, USI mode, SPI pads, engine init, chip-id read, variants A-E until one burst completes).
  SPI/USI registers are read only after the mode was written. At handoff the mux pad gpp26-1 is an **input** (`func=0 dat=0`), so
  plain `run` writes nothing and says `leaving the bus alone`; add `mux` for that.
- Options (docs/research/21 section 3), order inside a run: dump -> `mux` -> clock chain -> `osc`/`top` -> SPI setup -> `cs` (variant F)
  -> `loop` -> `rst` -> identify (+ `misopu`) -> variants A-E (only without experiment options) -> restore:

  | Option | What it writes | Notes |
  |---|---|---|
  | `mux` | gpp26 pin 1 only: DAT := 0 first, then CON := output, as XNU 0088 `sweettouchmux=1` | read-back checked; a mismatch stops the run |
  | `cs` | X1: SPI setup is the ABL `spi_init` (USI_CON/OPTION, FB_CLK; no MODE_CFG/INT_EN/SWAP writes); variant F = GPIO CS low, `CS_REG &= ~1`, `PACKET_CNT = n\|0x10000`, TX fill, `CH_CFG \|= 3`, poll RX >= n, `CH_CFG &= ~3`, `PACKET_CNT = 0`, SW_RST, GPIO high, `CS_REG \|= 1`; identify then uses the same style | |
  | `osc` | X2: USI mux `0x10C00640` bit4 := 0, poll BUSY (bit16), divider `0x10C0180C[3:0]` := 5, poll BUSY | restored if no burst moved data |
  | `top` | X6: bit4 := 1, divider := 9 (40 MHz ipclk) | divider is writable only with `osc`/`top` |
  | `loop` | X3: `MODE_CFG` bit3 (self-loopback) with `FB_CLK` 0 then 1, 5-byte TX/RX compare | restores MODE_CFG / FB_CLK |
  | `rst` | X4: PUD := 0 on clk/mosi/miso/cs/reset, reset pad low 10 ms then high, polls the FIFO (cmd 0x87, same shape as XNU) up to 1.5 s for event 0x03 | the pulls are restored at the end; first 8 non-zero events logged |
  | `misopu` | X5: MISO PUD := 3 during the identify | restored |

  Any experiment option drops the QCH write (research 21: it hands the clock to the Q-channel handshake) and the A-E ladder.
- `railsr` / `rails` (docs/research/23, `shim/acpm.c`; need no `run`): the ACPM client. Section `TOUCH rails (ACPM)`, in three steps with a pstore
  checkpoint after each, before `run`: (0) describe the IPC channel (reads only: INIT, channel table, the descriptor of channel 2, queue pointers, INTSR0;
  `SWEET TOUCH: ACPM ...` lines), (1) READ the two enable registers (`railsr`), (2) only with `rails`: UPDATE bit 7 (value = mask = 0x80, never clear) of 0x5C then
  0x5D, ~10 ms, read both back. Every transaction prints `seq`, `req=` / `rep=` words, `ret`, `val`, `err`, `writes`, `published`, `ticks` and `us`. Replies are
  bounded to 50 ms, no retries. Nothing is ever rolled back once the request queue's front is written (the APM may be running the request): a timeout / fault then
  prints `STATE seq=.. req rear=.. front=.. rsp rear=.. front=..` and leaves the channel as it is (the next run refuses BUSY until a human has looked).
  **Gates (soc.json `acpm`)**: while `layout_verified` / `elem_unit_verified` are 0 (the shipped state) or a rail `vsel_min/vsel_max` is null, the shim prints
  `ACPM unsure: <what>` and writes NOTHING. The describe step prints the descriptor words, the pointers, the first 48 raw words of both queue buffers (one 16-byte slot per
  `ACPM req|rsp buf+OFF` line) and, on a refusal, the exact reason, WHATEVER the validation said. `vsel_min/max` ship set (L25M 0x24-0x32, L26M 0x1C-0x3C, derived from the regulator
  source and the DT limits, checked by `tools/boards.py`). Staging: `railsr` (gates closed) -> compare the dump (doc 23 erratum, `docs/HANDOFF.md` SESSION 12) -> set the two flags ->
  `railsr` again (a queue-slot write + doorbell per register, the APM reads the register: no PMIC change) -> `rails`. Both flag-set steps WRITE to the APM's SRAM: they need the Opus re-review of the fixes first.
  Last lines: `SWEET TOUCH RESULT: rails read: L25M=off L26M=off ok` / `rails update: L25M 0x23->0xa3 L26M ... on=2/2 ok`.
  Status: *observed on hardware* (run rails2, 2026-10-22: UPDATE accepted, readback `on=2/2`; the rails are off again after a reset, so the shim switches them on before the XNU jump with `--rails`); also host tests (`acpm_host_test.c`, `rails_host_test.c`).
- Safety: every write goes through one helper `w32`, which asks `w32_allowed` (per register, the exact bits that may change; a write
  that changes another bit is refused, so a bit-wise RMW bug cannot spill). Not writable: CMU OPTION, any other divider, CMU_TOP mux/div,
  the irq pad, the mux pad's pull, SPI STATUS/RX_DATA, USI wrapper words other than CON/OPTION. `tools/test-bluejay-boot.py` audits the
  source (no asm, no volatile, no pointer casts, `if (!w32_allowed(...)) return` before the write, allowlist depends on the address) and
  the object (stores only stack/argument based, `try_write32` only in `w32`), with mutation self-tests.
  All polls have a time limit and a poll budget; the fixed delays (a few us, the 10 ms reset pulse, 5 ms between ready polls) use
  `wait_ticks()` and so are not protected against a dead counter.
- Output: the XNU driver's own lines (`SWEET TOUCH: cmu base ...`, `pin clk ...`, `VARIANT F: ...`, `identify: ...`), label `SWEET` + text
  `TOUCH: ...`, so `grep 'SWEET TOUCH:'` of the RAM/USB log feeds the same parser as the XNU log; lines over the 96-byte RAM-log line are
  split at spaces with key=value tokens whole. The UART copy reads `SWEET: TOUCH: ...`. The last line is
  `SWEET TOUCH RESULT: opts=0x.. mux=.. clk=.. F=.. loop=.. rst=.. id=.. moved=F,L,R,I` (F variant F, L loopback, R reset FIFO read,
  I identify; `moved=-` means nothing moved; `id=chip-id-ok` is the FTM5 id).
- Status: *observed on hardware* (run touch1, 2026-10-22: identify `36 48 03 00` with rails on); the recipe is in XNU patch 0095 and boot-arg `sweettouchcs=1`.
