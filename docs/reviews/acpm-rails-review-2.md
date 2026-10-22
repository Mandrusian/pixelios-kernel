# Review 2: ACPM rails client (`shim/acpm.c/.h`, `railsr`/`rails`), main @ ec9c13d, flags `layout_verified=1`, `elem_unit_verified=1`

Reviewer: Claude Sonnet 5.5, independent second read. Read-only: no device, no adb/fastboot, no tools/probe-run.
Date 2026-10-22. Hard rule 7 applied: every fact below was checked against the Google kernel source in `~/pixelios-artifacts/scratch/{acpm-src,touch-src}/`,
the acpm3 log, or a host run.
Tags: **[src]** read from source, **[obs-hw]** in the acpm3 hardware log (`logs/probe/20261022T145330Z-acpm3_/usb.txt`, the ACPM lines),
**[host]** host test or scratch test run here, **[guess]**.

## Verdict

- **OK to run `railsr` on hardware: YES.** No Critical or High finding. Every write on that path is bounded, validated first and never rolled back (Q1, Q3).
  `railsr` is a WRITE step: per read it writes 4 slot words, the request front, doorbell INTGR1, reply rear and INTCR0 (8 writes). Run it with that understanding.
- **OK to run `rails` later: WITH CHANGES.** Nothing in it can touch a register other than 0x5C/0x5D bit 7 and the vsel windows are right, but fix M1 and M2
  first (a wrong "request stays queued" report; a rail that is already on makes the second rail skipped), and do M3 so a `railsr` image contains no UPDATE code.
  Also see the order in "Before `rails`" below: `railsr` must come back clean first.

## What I ran, and what I only read

| Ran (host only) | Result |
|---|---|
| `bluejay` `board_config.h` regenerated into a `/tmp` dir with `tools/boards.py`; `diff` against `shim/out/board_config.h` | identical **[host]** |
| `acpm_host_test` (`gcc -O1 -Wall -Wextra -Werror ... acpm_host_test.c`, as build.sh:252) | `PASS acpm host test` **[host]** |
| `rails_host_test` with `-DRT_VERIFIED=0` and `-DRT_VERIFIED=1` (build.sh:253) | both PASS **[host]** |
| `python3 -m unittest tools.test_boards_acpm` | 5 tests OK **[host]** |
| Scratch test in `/tmp` (not in the repo): acpm_host_test.c with the **exact acpm3 geometry** (15 channels, table +0x9210, rear/front/base offsets, 5 stale ABL slots, rear=front=3) | PASS; first READ writes `0x2071320..0x207132c`, `0x20712ec`=4, `0x17610040`=4, `0x2071288`=4, `0x17610024`=4 **[host]** |
| Scratch test: `touch_probe_rails(2)` with L25M already on | shows M1 and M2 below **[host]** |
| `touchprobe.c` compiled with host gcc `-DTOUCH_PROBE -DTOUCH_RAILS`, `nm` | `U acpm_pmic_update` (M3) **[host]** |

Not run: `shim/build.sh` (heavy, overwrites images), `tools/check-all.sh`, `tools/test-bluejay-boot.py` (source/object audit), the mutation self-tests, any cross build. I did not re-run review 1's mutations.

## Requirement 1: the acpm3 dump against the code **[obs-hw] vs [src]**

Dump: `init[0..7]=00008db8 00000008 00009210 00000026 00000000 00000008 0000000f 00000000`, `n=15 tab=+0x9210 idx=2`,
`desc[0..17]=00000002 00000004 00000008 00000001 000382e8 000382ec 000382f0 0 0 0 00038288 0003828c 00038290 00000005 00000010 00000002 a7600000 00000001`,
`rear/front = 3/3` on both queues.

| Item | Code (`acpm.c`) | Dump | Verdict |
|---|---|---|---|
| `ipc_channels` at INIT+0x08, `ipc_ap_max` at INIT+0x18 (`INIT_CH_TAB`, `INIT_AP_MAX`) | `:35-36`, reads `init[2]`, `init[6]` | `0x9210`, `0xf`=15 (`num_ipc_channels` 0x26 at +0x0c is >= 15, consistent with framework.h:36-81) | matches |
| stride 0x48, `D_ID` at +0x00 | `:38-39` | the descriptor at `tab + 2*0x48` has id 2 and ids at idx 0,1 are not 2 (the loop reached idx 2) | matches; stride evidence is that one hit (**obs-hw**, not a guess any more) |
| `D_TYPE` +0x0c = 1 | `:47`, `:256` | `d[3]=1` | matches |
| AP->APM = descriptor `rx_*` (`d[4..6]`), replies = `tx_*` (`d[10..12]`) | `:40-45`, `:265-285` | `req rear@0x20712e8 front@0x20712ec base@0x20712f0`; `rsp rear@0x2071288 front@0x207128c base@0x2071290` | matches [src] `acpm_ipc.c:970-983`. Buffer content supports it **[obs-hw]**: the `req buf` slots hold commands (word1 FUNC 1 = WRITE, 0 = READ), the `rsp buf` slots hold replies with RETURN 0 and DEST 4 where the request was a READ (slot 3: req `000001ca 0`, rsp `000001ca 00000400`) |
| `q_len` +0x34, `q_elem_size` +0x38 in **bytes** | `:46`, `:48`, `:260-264` | `5`, `0x10`; five consecutive 16-byte slots, one command per line, words 2-3 zero | 16-byte stride is what the dump shows; consistent with [src] `acpm_ipc.c:762` / `memcpy_align_4` (bytes) |
| buffers fit and do not overlap | `:276-289` | req 0x382f0..0x38340, rsp 0x38290..0x382e0, pointer words 0x38288, 0x3828c, 0x382e8, 0x382ec: all disjoint, all inside the 0x40000 window | OK (my arithmetic, then **[host]** replay validated it) |
| queue-full conditions | `:390-392` | rf=3, sf=3, rr=3: `(3+2)%5=0 != 3`, `nf=4 != 3`. Second read: rf=4: `(4+2)%5=1 != 4`, `nf=0 != 4` | OK; matches [src] `acpm_ipc.c:712`, `:723` (tx_front/rx_front/tx_rear = req front / rsp front / req rear) |
| slot address | `:395` `req_base + 16*rf` | first read writes slot 3 = `0x20712f0 + 0x30 = 0x2071320`; second read slot 4 = `0x2071330` | OK **[host]** |
| `front := (rf+1) % q_len` | `:390`, guard `:333` | 3 -> 4, then 4 -> 0 | OK **[host]** |
| sequence number | `:367-369`: 1..63, never 0 | ABL's own stale requests carry seq 0 (word0 `0x1cb`), and the replies in the rsp buffer echo word0 byte for byte (5 of 5 slots) **[obs-hw]** | the firmware echoes word0, so the shim's "reply seq == request seq" check is sound. The shim's seq 1..63 matches [src] `acpm_ipc.c:747-750` (Linux panics on 0, the ABL evidently does not use it) |
| READ command word | `:369`: `reg | 1<<8 | 0<<12 | seq<<16`, `w1 = 0` | ABL's words have the identical form (`0x1ca` = REG 0xCA, TYPE 1, CH 0), `w1=0` is a READ whose reply DEST holds the value | the encoding is **[obs-hw]** for READ and WRITE (w1=1 | val<<8: `00000b01`, `00001401`) |
| UPDATE command word | `:510`: `2 | 0x80<<8 | 0x80<<16 = 0x00808002` | never seen in the dump | **[src]** only (`acpm_mfd.h` shifts/masks, `mfd_func`, `acpm_mfd.c:209-255`; Linux sends the same value+mask: `s2m_enable`) — the first `rails` is the first UPDATE ever observed |
| `cmd[3]` = 0 | `:371-372` | ABL's own slots have words 2,3 = 0 **[obs-hw]**, so the firmware does not need the Linux millisecond timestamp | review 1 L2 resolved |
| reply decode | `:448-449`: RETURN `rep[1]>>24`, DEST `(rep[1]>>8)&0xff` | slot 3 `rep[1]=0x00000400` decodes to ret 0, value 4 | OK **[obs-hw]** |
| `INTSR0` | read, `0x1761002c = 0` | accessible and idle | OK |
| `ap_poll` | printed, not checked | `1` | polling channel, as in [src] `acpm_ipc.c:961, :1001`; no IRQ from the APM expected |

Review-1 findings, each checked in the current code:

| ID | Status | Evidence |
|---|---|---|
| C1 (q_elem_size words vs bytes) | **Fixed** | `elem_unit=1`, `esz_b == 16` required (`:260-264`), slot `16*rf` (`:395`); `ACPM_W_ESZ` tests for 0,3,4,12,17,20,32,64 and the wrapped `0x40000004*4` (host test `:329-342`) **[host]**; the dump shows the 16-byte pattern **[obs-hw]** |
| H1 (undo races the APM) | **Fixed** | No write to `front` after the front write: `g.slot_ok = g.front_ok = 0` (`:403`), no undo anywhere; host test asserts `front_writes() == 1`, `busy_writes == 0`, `replay_hazard == 0` for timeout, slow-APM and fault paths (`:429-514`) **[host]** |
| M1 (dump only when valid) | **Fixed** | `rails_describe` prints descriptor and both buffers whenever `have_desc` (touchprobe.c:1196-1255); also reasons for each refusal **[host]** (`rails_host_test.c:72-95`). The acpm3 log shows it |
| M2 (fake APM shares assumptions) | **Fixed** | async APM, bytes, rear only advanced at the end, `busy_writes` / `replay_hazard` verdicts (`acpm_host_test.c:70-156`) |
| M3 (vsel windows) | **Fixed** | derived and rechecked in `tools/boards.py _acpm_check`; null with a verified flag fails the build; re-derived below |
| M4 (update code linked in `railsr`) | **NOT fixed** (author disagreed, HANDOFF:524) | see M3 below |
| L1 idx==id, TYPE_QUEUE | Fixed (`:254-257`, tests `:320-322`) |
| L2 `cmd[3]=0` | Unchanged; now supported by the dump (above) |
| L3 1 ms DVDD->AVDD gap, reset hold | Gap added (`touchprobe.c:1287`); reset is still not driven (see L6) |
| L4 pinned constants | Fixed differently: `boards.py _ACPM_PINS` + `test_pins` (`acpm_host_test.c:517-532`), not `_Static_assert`; equivalent for the generated macros **[host]** |
| L5 guard widening | Fixed: `acpm_guard()` factored out, `test_guard` scans every address in SRAM and mailbox windows x 9 values x 3 stages **[host]** |
| L6 stale reply blocks forever | Unchanged by design (BUSY refusal, zero writes) |

## Requirement 2: `mbox_addr` = `0xa7600000` (`d[16]`)

- **[src]** Linux never reads `ipc_channel.ch.mbox_addr`: `grep mbox_addr` over the Google tree hits only the struct (`framework.h:110`) and two firmware-side
  function-pointer prototypes (`:196`, `:221`); `channel_init` (`acpm_ipc.c:945-999`) builds the channels without it. The doorbell is
  `acpm_ipc->intr + APM_INTGR` where `intr` is DT `reg` #0 = `0x17610000` (`gs101.dtsi:496-499`, `acpm_ipc.c:410-413`, `acpm_ipc.h:105-117`).
  `acpm.c` has no `D_MBOX` define and never uses `d[16]` (it only dumps it). So **for the code it is harmless**.
- What the value *is*: **[guess]**. `0xa7600000 - 0x90000000 = 0x17600000`, which is not `0x17610000` either (differs by 0x10000), so a simple "APM-side address window" story does not
  reproduce it exactly. I could not decide it from source. I also searched `abl_code.bin`: no 32-bit literal of `0xa7600000`, `0x17610000`, `0x17600000`, `0x2039000`, `0x2043000`;
  one `movk x0, #0x1761, lsl #16` at ABL+0xbc20 on top of a `movn x0, #0xffff`, which builds `0xffffffff17610000`, not a clean address: inconclusive **[host]**.
- Consequence: the `INTGR1` doorbell address `0x17610040` rests on the Linux DT for this same SoC/firmware **[src]**, and `INTSR0` at `0x1761002c` reading 0 shows only that the block is
  readable **[obs-hw]**. The first `railsr` doorbell is the experiment. If the doorbell does not reach the APM, the result is `ACPM_E_TIMEOUT` with a published request (M4 below), not a wrong write.

## Requirement 3: hunt results

- **Write before validation:** none found. `put()` goes through `w32`, which returns -2 while `g->armed == 0`; `armed` is set only inside `acpm_guard()` (`:328`), called at `:396` after
  describe, both flags, empty queues, full-condition and timer checks (`:381-395`). **[src]** + **[host]**: every refusal case in `test_refusals` asserts `nw == 0 && nbell == 0` (`acpm_host_test.c:285-292`).
- **Refusal that still writes:** the only "refusals" that follow writes are the UPDATE gates after the pre-read: `ACPM_E_ON` and `ACPM_E_RAIL` (`acpm.c:505-508`). The pre-read (a READ)
  has already written its 8 words; the refusal itself writes nothing. That is by design and tested (`test_update_gates`: `nw == 8`).
- **Unbounded waits:** none in `acpm.c`. The reply poll ends on 50 ms (`lim = freq * timeout_us / 1e6`, `:415`, `:424`) **or** `ACPM_MAX_POLLS` (1,000,000 polls, `:424`), so a dead timer ends too
  (`time_frozen` test `:508-513`). `counter_freq()` is the constant `TIMER_FREQ = 0x1770000` = 24.576 MHz for gs101 (`console.c:27-29`, `board_config.h:9`). The shim's `wait_ticks` loops
  (`touchprobe.c:1187,1189,1287,1293`) wait on CNTPCT and use the same counter the rest of the shim relies on. What no guard can bound is a bus hang inside `try_read32/try_write32`; `rails_pre_read`
  announces the first two reads for exactly that, and the acpm3 run proved every read address works **[obs-hw]**. The first *write* addresses (SRAM slot, mailbox INTGR1) are not proven writable: a fault is caught by
  `try_write32`'s SError guard (`shim.c:144-153`); a silent hang is the residual risk **[guess]**.
- **Rollback after the front write:** none (H1 fixed). Failure after the front write only reads (`snap`, `:343-354`).
- **Writes outside the allowlist:** `test_guard` exhaustively checks the guard `acpm_guard()` builds against independently computed addresses (`want_slot = SRAM + 0xC000 + 16*4` etc.) **[host]**.
  The audit proves the write PATH; the addresses are data from SRAM, validated by `acpm_describe` (aligned, inside the 256 KiB window, indices < q_len, six regions pairwise disjoint) and, for this phone, now
  matched by the dump **[obs-hw]**.
- **Rails other than 0x5C/0x5D:** the allowlist does not constrain the *content* of the slot words (a slot word is allowed with any value), so the register/function restriction lives in
  `rail_of()` + the argument checks in `read1` / `acpm_pmic_update` (`:468-511`) and nowhere in `xfer()` (L1 below). In the callers: reg must equal `rail[0].reg` or `rail[1].reg` (0x5C/0x5D, pinned by
  `_ACPM_RAIL_REGS`), READ uses FUNC 0, UPDATE requires `val == mask == 0x80` (set bit 7, never clear) **[host]** (`test_refusals :361-377`). Register ownership: only the LDO25M/LDO26M descriptors
  use 0x5C/0x5D with `_MASK(_7)` (`s2mpg10-regulator.c:275-278`; `S2MPG10_PM_L25M_CTRL = 0x5C`, `L26M = 0x5D`, `s2mpg10-register.h:108-109`); the regulator driver sends to `iodev->pmic` whose I2C address
  (= TYPE) is `0x01` (`s2mpg10-regulator.c:879`, `s2mpg10-core.c:30,492`) **[src]**. Consumers of those two LDOs are only the two touch nodes (`gs101-common-touch.dtsi:126-137`) **[src]**.
- **vsel windows re-derived [src]:** group 4 (`LDO25M`) = 700000 uV + 25000 uV x code, group 5 (`LDO26M`) = 1800000 uV + 25000 uV x code (`s2mpg10-register.h:316-323`, `REG_DESC` -> `_MIN(g)/_STEP(g)`,
  `s2mpg10-regulator.c:206-221`), `vsel_mask = n_voltages - 1 = 63`. DT limits (`gs101-pmic.dtsi:535-553`): L25M 1.60-1.95 V: (1600000-700000)/25000 = 36 = **0x24**, (1950000-700000)/25000 = 50 = **0x32**.
  L26M 2.5-3.3 V: (2500000-1800000)/25000 = 28 = **0x1C**, (3300000-1800000)/25000 = 60 = **0x3C**. soc.json has 36/50 and 28/60; the generated header has `RAIL0_VSEL_MIN 36 ... RAIL1_VSEL_MAX 60` **[host]**.
  Nominal 1.8 V = 0x2C and 3.3 V = 0x3C (`gs101-common-touch.dtsi:14-15`), both in range; the L26M upper bound equals the nominal. The gate checks the code read from the register (`cur & 0x3f`), before the enable.

## Findings

### Critical
None.

### High
None.

### Medium

**M1. `rails`: a rail that is already on aborts the sequence and the second rail is skipped.** `shim/touchprobe.c:1288-1296`: `acpm_pmic_update` returns `ACPM_E_ON` (10) for "already enabled" (`acpm.c:505-506`),
and the loop condition `!e` ends the loop, so L26M is never attempted even though it is off. **[host]** scratch run with L25M = 0xA8, L26M = 0x30: result `L25M 0xa8->0xa8 L26M 0->0x30 on=1/2 REFUSED-or-FAILED`, no update for L26M.
Not a safety problem (it writes less); it defeats the purpose of `rails` if the ABL ever leaves one rail on. Fix: treat `ACPM_E_ON` as success for the loop (`if (e == ACPM_E_ON) e = ACPM_OK;` after the print) and add the case to
`rails_host_test.c` (RT_VERIFIED=1: L25M on, L26M off, expect `pmic[0x5D] == 0xB0`).

**M2. Refused update paths leave `acpm_xfer` uninitialised, and the log then prints garbage plus a false "the request stays queued" line.** `acpm.c:462-466` (`refuse()` sets only `n/err/why`), reached from
`:474` and `:493-508`; `touchprobe.c:1259-1262` leaves `struct acpm_trace t` uninitialised (only `seq` and `n` are set); `say_xfer` (`:1142-1161`) prints `seq/req/rep/published/state` from the unset struct and
`if (x->err && x->published)` prints "the request stays queued; channel left as is". **[host]** scratch output for the ON case: `ACPM update L25M seq=0 req=00000000 e3590450 ...`, `published=16777216`,
`ACPM update L25M STATE seq=0 req rear=0 front=16777216 ...`, `the request stays queued`. On hardware this would be stack contents, and it would tell a human that a published request is pending when none is,
exactly the line the human acts on. Only the update path (`rails`) is affected: `railsr` goes through `xfer()`, which initialises every field before use (`acpm.c:367-378`). Fix: `memset(&t, 0, sizeof t)` in `touch_probe_rails`
(then set `seq`), and make `refuse()` zero `published`, `state`, `req`, `rep`, `ret`, `value`, `t_bell`, `t_end`; add a host assertion that a refused `t->b` has `published == 0`.

**M3. The UPDATE code is compiled into, and linked into, `railsr` images (review 1 M4, still open, and more relevant now that both flags are 1).** `touchprobe.c:1289` calls `acpm_pmic_update` under
`#ifdef TOUCH_RAILS` only; `TOUCH_RAILS_WRITE` gates just the call in `shim.c:691-693`. **[host]** `nm` of `touchprobe.o` built with `-DTOUCH_PROBE -DTOUCH_RAILS`: `U acpm_pmic_update`. With the flags at 1 the
only thing separating `railsr` (READ) from `rails` (bit-7 UPDATE) is that one `#ifdef` in `shim.c`. Fix: wrap step 2 of `touch_probe_rails` (`touchprobe.c:1281-1306`) in `#ifdef TOUCH_RAILS_WRITE`, and wrap the
definition of `acpm_pmic_update` in `acpm.c` (and its declaration use) the same way, so a `railsr` image contains no code that can build `0x00808002`. Add an object-audit/gate check that `acpm.o`/`touchprobe.o`
of a `railsr` build do not define/reference `acpm_pmic_update`.

**M4. A timeout or doorbell failure leaves a published request that outlives the run.** `acpm.c:402-413`, `:424-428`. After the front write there is no rollback (correct, H1); the request stays in `[rear, front)`.
Effects: (a) the next shim run refuses with `ACPM_W_BUSY` (zero writes; correct); (b) **[guess]** the pending request survives an AP reset (the APM runs in ALIVE) and is executed on the next doorbell on channel 2
(Linux or ABL), with its reply queued in front of that user's own reply, which an ABL that takes the first reply entry would mis-attribute. For a READ this changes no PMIC state; for a `rails` UPDATE it would enable
the rail late (volatile). The residual risk is highest for the very first doorbell, because the mailbox address rests on the DT only (Requirement 2). I do not propose a rollback. Instead: (i) `railsr` already stops
after the first failure (`touchprobe.c:1272`, `shim.c:688-694`); keep it. (ii) The first `railsr` result decides everything, so read it with `tools/xrun --dry-run`/the report, not by re-running. (iii) State this residual
risk in the handoff and AGENTS.md instead of "nothing was written" wording; the current `say_xfer` text for a refusal says "nothing written" only when `x->n == 0`, which is right.

### Low

**L1. `xfer()` does not validate what it is asked to send.** `acpm.c:357-372`: `reg` and `w1` are caller-supplied, and the guard allows any value into the slot. Today the two callers are correct (above), but the audit's
guarantee ("only these registers") then rests on two call sites. Fix (3 lines, before `acpm_describe`): `if (rail_of(c, reg) < 0 || (w1 != FUNC_READ && w1 != (FUNC_UPDATE | UPD_VAL_MASK << 8 | UPD_VAL_MASK << 16))) return fail(x, ACPM_E_ARG, ACPM_W_ARG);` plus a host test via the static `xfer`.

**L2. The doorbell is allowed from the moment the guard is armed.** `acpm.c:83`: `pa == g->bell_pa` is allowed with `v == bit` irrespective of `published`. Only one call site exists; tighten with a `bell_ok` that opens at the front write (as `rear_ok` does), so the guard cannot ring before the request is published.

**L3. The reply is matched on sequence number and count only.** `acpm.c:450`. The dump shows the firmware echoes word0 verbatim **[obs-hw]**; comparing `(rep[0] & 0xffff) == (req[0] & 0xffff)` as well (REG, TYPE, CHANNEL) costs one condition and would flag a reply that belongs to another transaction.

**L4. Stale text now contradicting the state of the tree.** `acpm.c:3-4` ("NEVER run on hardware"), `acpm.c:33-34` and `:38` ("[guess] until layout_verified"), `:48` ("still [guess] until verified on a dump"),
`shim/AGENTS.md:26` ("The two flags ship at 0"), `docs/HANDOFF.md:506,530` (the closed-gates statements, partly marked superseded). Update them to the 2026-10-22 state, tagged `[obs-hw acpm3]`.

**L5. Evidence tag missing a build id.** `soc/google/gs101/soc.json` notes (`acpm`, `touch`) say "observed on bluejay ... run acpm3" with a date but no build id (rule 5: date + build id). Add the build id from `logs/probe/20261022T145330Z-acpm3_/build.log`.
Also note the verification basis is a single boot, and `layout_verified` covers only the fields used (id, type, rx/tx pointers, q_len, q_elem_size, ap_poll), not the unused ones (`mbox_addr`, `credit`, `rx_indr_buf*`).

**L6. Touch-IC power-up conditions beyond the order are unproven (carry-over from review 1 L3).** `touchprobe.c:1281-1296`: DVDD -> 1 ms -> AVDD matches `ftm5_fts.c:5395-5422` **[src]**, but the shim does not hold the touch reset
low while the rails come up and the 10 ms settle (`:1293`) is a **[guess]**. Harmless for the PMIC (the rails stay on until reboot), but the later "chip answered" result depends on it.

## Before `railsr` (what to read, what to expect) and before `rails`

`railsr` (build: `--touch-probe=railsr`, no `run`, no `TOUCH_RAILS_WRITE`; device check must report bluejay):

1. Expected writes per READ, with the acpm3 geometry **[host]**: slot words at `0x2071320` (slot 3; second read: slot 4 at `0x2071330`), then `0x20712ec := 4` (second read: `:= 0`), `0x17610040 := 4`, `0x2071288 := 4`,
   `0x17610024 := 4`. The log prints `writes=8 published=2`.
2. Expected reply for `L25M`: `rep=0001015c 0000xx00 ...` (seq 1 echoed; `ret=0`; `val` = the register, bit 7 = 0 if the rail is off). Then `L26M seq=2`.
3. PASS means: `err=0`, `ret=0`, `us` well under 50000, the queue ends with rear == front on both queues, `val & 0x3f` inside the windows (0x24-0x32, 0x1C-0x3C). Anything else (TIMEOUT, `STATE` lines, `ACPM_E_REPLY`): stop,
   do not rerun, read the STATE line, and treat M4.
4. If L25M or L26M already has bit 7 set, the premise in doc 21/23 ("touch rails are off") is wrong; do not go on to `rails` until that is understood.

`rails` (later): fix M1, M2, M3 first, re-run `acpm_host_test` / `rails_host_test` (both verified modes) and `tools/check-all.sh`, and have `railsr` come back clean with in-window vsel codes. The first UPDATE is **[src]** only (word `0x00808002`, same as
`s2m_enable`), so read the UPDATE reply and the readback line once, before building anything on top.
