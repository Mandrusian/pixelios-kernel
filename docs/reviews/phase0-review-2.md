# Phase 0 review 2: `t3/bluejay-probe-shim` after the review-1 fixes

Reviewer: Claude (review sub-agent), 2026-10-20. Scope: everything in
`git diff main...t3/bluejay-probe-shim`, branch tip `a3a9da0`, base `f5f40d6`
(13 commits, `cf7566e..a3a9da0`). This is a read-only review: no `adb` and no
`fastboot` command was run, and nothing was flashed. The only file written is
this report. The build writes only into the gitignored `shim/out/`.

Line numbers refer to `a3a9da0` in worktree
`<worktree>` (the same worktree the branch
was reviewed in).

## Verdict summary

| ID | Round-1 finding | Verdict | Evidence |
|----|-----------------|---------|----------|
| C1 | Probe fault recovery never fires | **FIXED** | `shim/shim.c:371-386`, `:105`, `:117` |
| C2 | EL2→EL1 drop inherits firmware state | **FIXED** | `shim/start.S:43-86`, `:100-137` |
| H1 | WFI waits can sleep forever | **FIXED** | `shim/console.c:32-52`; all waits use it |
| H2 | UART TX polls forever | **FIXED** | `shim/console.c:74-106` (residual: `:85`) |
| H3 | FDT cell-size heuristic | **FIXED** | `shim/fdt.c:74-107`, `:185-195`; test `:119-153` |
| M1 | Sweet MMU fail-safe | **FIXED** | `shim/mmu.c:89-90` |
| M2 | Watchdog order/logic | **FIXED** | `shim/shim.c:223-224`, `:130-158` |
| M3 | 16 KiB PRDT vs 4 KiB data unit | **FIXED** | `shim/ufs.c:112-128`, `:188-193`, `:324-329` |
| M4 | Framebuffer stale state / loose match | **FIXED** | `shim/fdt.c:282-333`; test `:186-222` |
| L1 | `stage` NULL in panic path | **FIXED** | `shim/shim.c:221` |
| L2 | Unbounded FDT walk | **FIXED** | `shim/fdt.c:113-120`, `:135-136`, `:175-183` |
| L3 | Mixed clock sources | **FIXED** | `counter_freq()` in every bluejay wait |
| L4 | CPU_ON reporting / retry logic | **PARTIAL** | `shim/shim.c:272-281` |
| L5 | GICR test runs on bluejay CPU1 | **FIXED** | `shim/shim.c:425-435` |
| L6 | Wrong slot-9 comment | **FIXED** | slot 9 no longer special-cased |
| L7 | Boot image / LZ4 fallback | **FIXED** | `shim/build.sh:57-73`, `tools/test-bluejay-boot.py:88-89` |
| L8 | DECON read-back | **FIXED** | `shim/shim.c:166-178` |

No REGRESSED and no NOT FIXED verdicts. One PARTIAL (L4) and eight new
findings, one of them High (N1: the write-opcode safety check does not work).
No new Critical finding.

## Build and test evidence

```
$ ./shim/build.sh --board bluejay --avb
PASS header v4 (raw): kernel=208896 ramdisk=98; host tests & safety checks passed
PASS header v4 (lz4): kernel=17512 ramdisk=98; host tests & safety checks passed
-rwxrwxr-x 208896  shim/out/Image
-rw-rw-r-- 217088  shim/out/shim-boot.img
-rw-rw-r-- 67108864 shim/out/shim-boot-avb.img
-rw-rw-r-- 28672   shim/out/shim-boot-lz4.img
-rw-rw-r-- 67108864 shim/out/shim-boot-lz4-avb.img

$ python3 tools/test-bluejay-boot.py shim/out/shim-boot.img
PASS header v4 (raw): kernel=208896 ramdisk=98; host tests & safety checks passed

$ python3 tools/test-bluejay-boot.py shim/out/shim-boot-lz4.img
PASS header v4 (lz4): kernel=17512 ramdisk=98; host tests & safety checks passed

$ cd shim && ./out/host_test && ./out/fdt_host_test
(exit 0, no output)
```

`./shim/build.sh --board sweet` still compiles but stops at `mkbootimg`
because `../dtb/boot_unpacked/dtb` is absent and gitignored; that is the known
worktree state, not a branch regression. Bluejay was rebuilt last, so
`shim/out/` is the bluejay artifact set described above.

Note: `./shim/out/host_test` run from the repository root segfaults because the
test writes a relative `out/host.ppm`. It passes when run from `shim/` (as the
build does). That is a pre-existing test-harness invocation detail, not a
branch change.

Disassembly checks after the rebuild (same as round 1 where relevant):
`vectors` at 0x1000; `exc_common` at 0x178c saves x0–x28 and restores them;
`try_read32`'s faulting `ldr w4,[x0]` at 0x1834 resumes at 0x1838; the probe
fault/guard words are at 0x7000/0x7004; `wait_ticks` ORs CNTKCTL with 0xd4
(EVNTEN, EVNTI=13) and re-reads CNTPCT inside the WFE loop; `secondary_entry`
preserves x0 (context); `_end` = 0x33000 and `_stack_top` = `_sec_stacks` =
0x2b000, so the primary and secondary stacks do not overlap.

## Round-1 findings

### C1 — FIXED

`shim/shim.c:371-386` handles `slot == 4` only for ESR EC 0x25 and advances
`ELR_EL1` by 4; `slot == 7` (SError from the current EL) records the fault,
clears the guard, and re-masks `PSTATE.A` in `SPSR_EL1`. `try_read32`
(`:100-110`) and `try_write32` (`:112-120`) now do `dsb sy; msr daifclr, #4;
isb; msr daifset, #4` while the guard is still set, so a pending asynchronous
SError is taken inside the window. The shim runs EL1h, so slots 4/7 are the
correct current-EL entries; `probe_guard`/`probe_fault` are `volatile`
(`:62-64`). The resume address matches the instruction after the faulting
load in the disassembly.

Residual (unavoidable in software): a read that stalls the bus (gated PCLK)
cannot be recovered by any handler, and an SError that becomes pending only
after the `daifset` window still reaches the panic path. If EL3 routes SErrors
to itself, the probe stops; documented in `device/google/bluejay/docs/GATE0.md`.

### C2 — FIXED

`shim/start.S:43-86` writes (rather than ORs) `HCR_EL2 = RW`, copies
MIDR/MPIDR to VPIDR/VMPIDR, writes `SCTLR_EL1 = 0x30d00800`, clears CPTR/HSTR/
VTTBR, sets `CNTHCTL_EL2 = 3`, `CNTVOFF_EL2 = 0`, and ORs `ICC_SRE_EL2` with
SRE|Enable before the `eret`. The secondary path `:100-137` repeats the whole
sequence and keeps x0 (the CPU_ON context) intact; it only uses x1/x2. The
`mov x2, #(1 << 31)` and SCTLR immediate assemble and the image builds.

Residual: `SCR_EL3.EA` (EL3 SError routing) and firmware-set EL3 state remain
outside the shim's control; `device/google/bluejay/docs/GATE0.md` already lists this.

### H1 — FIXED

`shim/console.c:32-52` is the single wait helper: it sets `CNTKCTL_EL1`
EVNTEN and EVNTI=13 (~0.67 ms at 24.576 MHz), then loops on CNTPCT with `wfe`.
`screen_room` (`:59`), the probe delays (`shim/shim.c:244`, `:257`, `:296`,
`:300`), the final 120 s wait (`:305`), and both `reset()` variants (`:326`,
`:332`) use it. All bluejay waits use `counter_freq()` = `TIMER_FREQ`, fixing
L3 at the same time.

Residual: this still relies on the generic-timer event stream actually waking
WFE. If the stream is not implemented or connected on gs101, the core sleeps
forever; that is the Gate 0 "WFE event stream wake" item and must be tested on
hardware. A belt-and-braces fallback (read CNTPCT in a spin loop if the event
does not arrive within, say, 2× the event period) would remove the risk at the
cost of a busy core.

### H2 — FIXED

`shim/console.c:74-106`: both poll paths are bounded by a 1 ms deadline
(`start + counter_freq()/1000`), and the first timeout sets the sticky
`uart_dead` so later characters return immediately. There is no unbounded
UART loop left.

Residual: the very first `UFCON` read at `:85` is itself unguarded. If the UART
block is clock-gated, that APB read can still hang the bus before any poll
starts. No software poll can fix a bus stall; the safe options are to probe the
block once through the C1 guard (requires moving the UART check above
`console.c`'s dependency cycle) or to accept it as the documented Gate 0 risk.
This is a note, not a reason to reopen H2: round 1's suggested fix had the same
property.

### H3 — FIXED

The unified walker records root `#address-cells`/`#size-cells` and the
`/reserved-memory` overrides at `shim/fdt.c:185-195`; `fdt_active_*_cells`
(`:74-86`) selects them by depth, `fdt_reg_stride` (`:88-95`) rejects 0/3+
cells, and `fdt_read_reg_entry` (`:97-107`) reads the right number of cells.
`fdt_memory_contains` and the report both iterate by stride rather than
guessing 12 vs 16 bytes. `fdt_host_test.c:119-153` covers the real gs101
4×12-byte layout and asserts that a range spanning two banks does not match.

Residual: reserved-memory children deeper than depth 3 fall back to the root
cells, and a `#address-cells`/`#size-cells` pair that changes after the node's
`reg` (property order) is not retroactive. Neither occurs in the gs101 tree;
both are fail-safe (no match / no dump).

### M1 — FIXED

`shim/mmu.c:89-90` returns -1 before building any tables when
`fdt_memory_contains` does not confirm the image. `shim_main` already treats
`rc != 0` as "stay MMU-off" (`shim.c:658-663`), so sweet can no longer take a
prefetch abort from PXN Device mappings. The stale fields printed on that path
are a separate cosmetic issue (N4 below).

### M2 — FIXED

`bluejay_probe` disables both watchdogs at `shim/shim.c:223-224`, before the
framebuffer walk, the first `con_clear`, and the DECON rearm. The order asked
for by round 1 is implemented. `disable_bluejay_watchdog` (`:130-142`) reads
WTCON/WTCNT once, unconditionally clears ENABLE|RSTEN (bits 5 and 0), writes,
and re-reads WTCON; `report_bluejay_watchdog` (`:144-158`) prints the original
and re-read values and a FAR on a guarded read fault.

Residual: `:157` prints `disabled=1` unconditionally after a successful write.
It should print `(s->after >> 5) & 1` (or compare against 0) so a write that is
ignored cannot be reported as "disabled". Cosmetic, because the write path is
guarded and the value is re-read anyway.

### M3 — FIXED

The bluejay path no longer emits a 16 KiB PRDT entry. `list_partitions`
(`shim/ufs.c:188-193`) and the bluejay GPT-entry loop (`:324-329`) issue one
4 KiB block per command; `scsi()` programs byte-granular RSP/PRDT units
(`:112-127`) and `tools/boards.py:58` sets `ufs_prdt_byte_gran: 1` for bluejay.
The 4 KiB `BLOCK` is defined at `:35`. `shim/ufs.c:141` reads the SCSI status
from `resp[7]`, which is correct (byte 6 is the UPIU response code, byte 7 the
SCSI status), so round 1's `resp[7]` remark was a false alarm.

Residual (pre-existing, not introduced): the sweet-only `test_ufs` still reads
the MBR and GPT header as `2 * BLOCK` = 8 KiB in one PRDT entry
(`shim/ufs.c:268-269`). That path is unreachable on bluejay (`bluejay_probe` is
`noreturn`) but it violates the same 4 KiB host limit on sweet. Fix it when
sweet is next touched.

### M4 — FIXED

`fb_begin` (`shim/fdt.c:282-293`) clears `cur[d]`/`is_fb[d]` on every
BEGIN_NODE, so a sibling can no longer inherit `reg` or the splash flag.
`/reserved-memory/*splash*` is accepted only at depth 3, and
`/chosen/framebuffer*` only with `compatible` containing `simple-framebuffer`
(`:301-305`). `fb_end` (`:324-333`) aborts the walk on the first node that has
both a recognised type and a base. `fdt_host_test.c:186-222` is exactly the
sibling-leak case from round 1 (sibling 1 has compatible but no reg, sibling 2
has reg but no compatible) and now asserts a clean miss, i.e. board fallback.

Residual: properties are processed in FDT order. If a node puts `reg` before
`compatible`, `is_fb[d]` is still 0 when the `reg` is seen and the base is
dropped; the result is the board fallback, which is the safe outcome. A
`reg` shorter than the active stride is discussed in N7.

### L1–L8

- **L1 FIXED.** `shim/shim.c:221` sets `stage = "bluejay probe"` before any
  `con_*` call, so the panic path never passes NULL.
- **L2 FIXED.** `shim/fdt.c:113-120` rejects a magic/totalsize/struct-offset
  combination outside the blob; every token and property is bounds-checked
  (`:135-136`, `:145-149`, `:175-183`). The walker cannot loop: FDT_NOP and
  FDT_END_NODE advance 4 bytes, FDT_BEGIN_NODE advances at least 5, FDT_PROP
  at least 8. Residual totalsize trust is N8.
- **L3 FIXED.** All bluejay waits use `counter_freq()` (constant
  `TIMER_FREQ`), including `screen_room`, the probe delays, `test_ufs`'s
  timeout, and the final wait. No `CNTFRQ_EL0` use remains on the bluejay path.
- **L4 PARTIAL.** See below.
- **L5 FIXED.** The GICR write test is inside `#if !BOARD_BLUEJAY`
  (`shim/shim.c:425-435`), so bluejay CPU1 only records its ID registers and
  powers off.
- **L6 FIXED.** The slot-9 special case and its wrong comment are gone.
  Anything that is not the IRQ slot (5) or a guarded probe fault now reaches
  the panic screen, which is the correct behaviour at EL1h.
- **L7 FIXED.** `shim/build.sh:57-73` builds raw and LZ4 v4 images and AVB
  footers for both; `tools/test-bluejay-boot.py:88-89` accepts either an ARM64
  Image or the LZ4 magic. Both images pass the test. The raw and LZ4 paths
  produce 217088-byte and 28672-byte boot images and 64 MiB AVB outputs.
- **L8 FIXED.** `decon_rearm_trigger` (`shim/shim.c:166-178`) reads the trigger
  before writing 0x3061 and prints `before`/`after`, and reports a guarded read
  or write fault instead of panicking.

### L4 — PARTIAL (the only incomplete fix)

What round 1 asked for: do not blindly retry after the 250 ms timeout, act on
`ON_PENDING` (-5) / `ALREADY_ON` (-4), and print the first return value.

What the code does now (`shim/shim.c:263-281`):

```c
uint64_t ret = smc(PSCI_CPU_ON, ...);
/* wait 250 ms */
if (cpu_report[cpu][6]) { ... }
else {
	con_hex2("CPU_ON 1st", "cpu=", cpu, "ret=", ret);   /* first ret is printed */
	con_str("CPU path", "retry after PMU prep");
	prepare_bluejay_cpu(cpu);                            /* unconditional */
	ret = smc(PSCI_CPU_ON, ...);
	/* wait another 250 ms, print 2nd ret, "started"/"failed" */
}
```

- The first return value is printed, but only *after* the timeout, so it is
  still not available to the decision that follows.
- There is no test of `ret` at all. A slow core that returns SUCCESS (0) but
  reports after 250 ms gets `prepare_bluejay_cpu()` run against it while it is
  still powering up; the second `CPU_ON` then returns ALREADY_ON (-4) and the
  screen says `CPU retry: failed`, which is wrong.
- `prepare_bluejay_cpu` clears the BID enable and pending bits for that CPU
  (`:210-213`). Poking those while the core is mid-power-up is the exact race
  round 1 warned about.
- The PSCI return is a signed 32-bit status. `ret` is `uint64_t` and is
  printed with `con_hex2`, which truncates to 32 bits, so the text is useful,
  but any future comparison must cast to `int32_t`.

Concrete failure: CPU3 (A76) takes ~300 ms to reach `secondary_entry`.
First `CPU_ON` returns 0. At t=250 ms the code runs the PMU prep, clearing
CPU3's BID/pending state. The second `CPU_ON` returns -4 (ALREADY_ON). At
t=300 ms CPU3 writes `cpu_report[3][6]`. Whether the per-CPU block at `:283`
prints it depends on the exact interleaving (it re-reads the flag at `:283`,
so a report arriving before that line is shown), but `:281` has already
printed `CPU retry: failed` either way, and the PMU race has already happened.
The harm is the misleading status line plus poking a starting core, not a lost
report.

Fix:

```c
int32_t r = (int32_t)smc(...);
if (r == 0) {
	/* success: wait up to 2 s, never PMU-prep */
} else if (r == -4 || r == -5) {
	/* ALREADY_ON / ON_PENDING: just wait longer */
} else {
	prepare_bluejay_cpu(cpu);
	r = (int32_t)smc(...);
}
```

and report "late" separately from "failed". A 2 s timeout for the success case
costs nothing on hardware that reports in milliseconds.

## New findings

Ranked by severity. None of these are regressions of a round-1 verdict; N1 is
a new safety-control defect in the code added by L7, N2 is a missing guard in
the bluejay UFS test, N3 is documentation drift, and N4–N8 are low-severity
correctness/robustness issues in the new code.

### N1 (High) — the UFS write-opcode safety check does not work

`tools/test-bluejay-boot.py:34-50`, `docs/SAFETY.md:30-31`, `shim/ufs.c:97`

`docs/SAFETY.md` says "A host check fails the build if a write opcode
appears." It does not. I exercised `check_ufs_safety()` directly against
modified copies of `ufs.c`:

| Edit | Result |
|------|--------|
| current tree | passes |
| `ucd.cmd[1] = 0x20;` (UPIU write flag) | **passes** |
| `ucd.cmd[1] = len ? 0x20 : 0x00;` | **passes** |
| add `uint8_t w[10] = { 42, ... };` (decimal WRITE(10) opcode) | **passes** |
| add `uint8_t w[10]; w[0] = 0x2a;` | **passes** |
| add `uint8_t w[10] = { 0x2A, ... };` | caught |

Three separate defects:

1. The flag regex at `:44` is
   `ucd\.cmd\[1\]\s*=\s*(0x[0-9a-fA-F]+|\d+)`. The real line is
   `ucd.cmd[1] = len ? 0x40 : 0x00;` (`shim/ufs.c:97`): after `=` comes `len`,
   so the regex matches nothing at all (`flag_matches == []`). The check is
   dead code today.
2. Even when it matches, it tests `val & 0x40`. `UPIU_CMD_FLAGS_WRITE` is
   `0x20` and `UPIU_CMD_FLAGS_READ` is `0x40` (`include/ufs/ufs.h`), so the
   test rejects reads and accepts writes. Writing `0x20` passes.
3. The CDB scan (`:34-42`) only looks at `uint8_t name[N] = { ... }`
   initializers whose first token is a hex literal, and only at the first
   token. A write CDB built by assignment, by a decimal literal, or through a
   pointer/loop is invisible. The `0x16` and query-opcode checks are literal
   substring/regex tests with the same weakness.

Failure scenario: the next author adds a "write the userdata image" helper and
builds the CDB as `cdb[0] = 0x2a;` (or flips the UPIU flag to `0x20` in a
ternary). CI passes, the documented invariant is believed, and the first run
issues a WRITE(10) to the phone. The safety story is the only thing standing
between "probe" and "destructive tool"; a check that cannot fail is worse than
no check because it is trusted.

Fix: (a) test `0x20` and treat *any* `ucd.cmd[1]` assignment as suspect unless
it is provably `0x00`/`0x40` in a `len ? ... : ...` form; (b) scan the whole
`scsi()` call graph, not just brace initializers, or better, make `scsi()`
take an enum/flags argument and refuse any direction other than read at
compile time (`_Static_assert`); (c) add a negative self-test that feeds a
known-bad source to `check_ufs_safety()` and asserts it raises. The self-test
is the only way to keep the regex from silently rotting again.

Note: the code as committed is read-only. All CDBs are TUR (0x00), READ
CAPACITY(10) (0x25), and READ(10) (0x28), and `scsi()` sets the read flag for
every data command; no current path can write to the device. The finding is
about the enforcement, not about a present write.

### N2 (Medium) — `test_ufs_bluejay` lacks the two takeover guards that `test_ufs` has

`shim/ufs.c:288-307` vs `:229-249`

`test_ufs` checks, before touching the list:

```c
if (ufs_rd(REG_UTRLDBR)) { "requests outstanding: stop"; return; }
...
if (ufs_rd(REG_UTRLBA) != list || ufs_rd(REG_UTRLBAU) != ... ||
    !(ufs_rd(REG_UTRLRSR) & 1)) { "list base not taken: stop"; return; }
```

`test_ufs_bluejay` checks HCE/HCS, then immediately writes `UTRLRSR=0` and
`UTRLBA`, and its acceptance test (`:304`) checks only the base, not that
`UTRLRSR` actually reads 1. If ABL left a doorbell bit set in `UTRLDBR`, then
as soon as `UTRLRSR` is set to 1 at `:303` the host may fetch our (zeroed)
slot 0 before `scsi()` ever fills it. `utrl` is static and zero, so the UTRD
has `dw0 = 0` (direction "to device", PRDTL 0) and `ucdba = 0`; the host
fetches a UCD from physical address 0 and completes with an error/OCS, or
worse the doorbell never clears and the first `scsi()` waits out its 1 s
timeout and clears the bit. The device sees a bogus zero-length command. That
is not a write, but it is exactly the class of stale-ABL-state problem the
guards in `test_ufs` exist to prevent.

Failure scenario: ABL hands over with `UTRLDBR` bit 0 set (it had a request in
flight or left the register dirty). `:299` stops the list, `:303` restarts it
with the doorbell still set, and the probe's first visible result is a UFS
error or a 1 s stall, not a clean "read-only probe".

Fix: copy the `UTRLDBR` check and the `UTRLRSR & 1` check from `test_ufs`
into `test_ufs_bluejay`, before `UTRLRSR=0` and in the acceptance test
respectively. Also clear `UTRLDBR` explicitly (write 1 to the bit) only after
the base is accepted.

### N3 (Medium) — `NOTES.md` handoff is stale and now wrong

`NOTES.md:5`, `NOTES.md:8`

The active handoff still says:

- "Watchdogs are sampled twice one second apart and modified only if WTCON is
  enabled and WTCNT changes." The code now reads each watchdog once, disables
  it unconditionally, and re-reads (`shim/shim.c:130-142`). The note describes
  behaviour that no longer exists and would make a future agent believe a
  disabled-at-entry watchdog is left alone.
- "Rebased onto current local `main` at `fcca658`. Commits: `741c6b5` ...
  `389efc1`." Actual `main` is `f5f40d6`; the branch is 13 commits
  `cf7566e..a3a9da0`. The listed commits do not exist on this branch.
- Nothing about the round-2 fixes, the LZ4/AVB images, or the UFS safety check.

`AGENTS.md` requires the active handoff to be updated after each milestone and
to separate observations from guesses. As written, this is a navigation hazard
for the next agent (it points at the wrong base and wrong commit IDs).

Fix: rewrite the "Active handoff" block for `a3a9da0`, state that watchdogs are
disabled unconditionally with one read, list `cf7566e..a3a9da0`, and note the
review-2 verdicts. Do not claim a hardware result; there is none.

### N4 (Low) — `mmu_info` fields are printed uninitialised when `mmu_enable` bails

`shim/shim.c:633`, `:670-674`; `shim/mmu.c:89-90`

`struct mmu_info mi;` is uninitialised. `mmu_enable` sets `img_lo`, `img_hi`,
`ttbr`, and `img_fdt_confirmed`, then returns -1 at `mmu.c:89-90` before
setting `n_l3`, `sctlr_before`, or `sctlr_after` (those are at `:115`,
`:127`, `:132`). `shim_main` prints `mi.n_l3`, `mi.sctlr_before`, and
`mi.sctlr_after` unconditionally at `shim.c:670-674`, so the sweet failure
screen shows stack garbage on the path that is supposed to explain why the MMU
was not enabled. Cosmetic, but it is the diagnostic path.

Fix: `struct mmu_info mi = { 0 };` or set the three fields before the early
returns in `mmu_enable`.

### N5 (Low) — the panic dump prints the wrong frame slot as X30

`shim/shim.c:405`, `shim/start.S:157-221`

`vent` does `stp x29, x30, [sp, #-16]!` at the vector (sp1 = sp0−16, x29 at
sp1, x30 at sp1+8), then `exc_common` allocates a 240-byte frame
(sp2 = sp1−240) and saves x0–x28. `regs` points at sp2, so the layout is:

| C index | address | contents |
|---------|---------|----------|
| `regs[28]` | sp2+224 | x28 |
| `regs[29]` | sp2+232 = sp1−8 | **never written** (stale stack) |
| `regs[30]` | sp2+240 = sp1 | saved x29 |
| `regs[31]` | sp2+248 = sp1+8 | saved x30 |

Therefore `con_hex("X30", regs[29])` prints stale stack, while the saved x30
is at `regs[31]`. The comment "stashed lr is just above the frame" points at
the x29 slot, not x30. This is diagnostic-only (the panic path is already
terminal), but on a first boot the register dump is the only evidence, and a
garbage X30 is worse than no line.

Fix (minimal): `con_hex("X30", regs[31])` and correct the comment. If the
layout is to be made explicit instead, enlarge the frame to 32 slots
(`sub sp, sp, #(32 * 8)`), copy the stash into it
(`ldr x2,[sp,#256]; ldr x3,[sp,#264]; stp x2,x3,[sp,#232]`), keep the epilogue
at `#(32 * 8)` so the original `ldp x29,x30,[sp],#16` still reads the stash,
and print `regs[30]`. Do not copy into `[sp,#232]` without enlarging the
frame: that would overwrite the saved x29 that the epilogue reloads.

### N6 (Low) — "memory" substring match also accepts `reserved-memory` and `memory-controller`

`shim/fdt.c:248`, `:386`

`has_text(st->names[1], "memory")` matches any node name containing "memory",
including `/reserved-memory` itself at depth 2 and a `memory-controller`
node. If such a node has a `reg` property, its contents are reported as DRAM
(`report_prop` `:386-400`) or used by `fdt_memory_contains` (`:248-260`). On
the gs101 tree `/reserved-memory` has no `reg` of its own, so this is latent,
but a bootloader-injected `reg` would produce a bogus memory bank in the dump
and could make `fdt_memory_contains` confirm an image page that is not RAM.

Fix: match the node's unit name exactly: `streq(name, "memory")` or
`strncmp(name, "memory", 6) == 0 && (name[6] == 0 || name[6] == '@')`.

### N7 (Low) — framebuffer `reg` length check and unbounded `compatible` scan

`shim/fdt.c:303`, `:307`

`fb_prop` accepts `reg` when `len >= 8` and then calls `fdt_read_reg_entry`,
which reads `(ac + sc) * 4` bytes. With the gs101 root cells (2+1 = 12 bytes)
a malformed 8-byte `reg` makes the size read 4 bytes past the property, i.e.
the next property's length/nameoff header, producing a garbage size (the base
is still correct). Separately, `has_text((const char *)val,
"simple-framebuffer")` scans until a NUL with no `len` bound, so a
non-NUL-terminated `compatible` can run past the property and, in the worst
case, past the blob into adjacent memory.

Failure scenario: an FDT with a short `reg` on `/chosen/framebuffer@0` and a
following property whose first bytes look like a size; the shim maps a screen
with a wrong size, or `con_clear` writes outside the reserved splash region.
The current gs101 tree has no `reg` on that node at all, so this is defensive.

Fix: require `len >= (ac + sc) * 4` (use `fdt_reg_stride`) before decoding, and
write a bounded `has_text_n(val, len, needle)` or check `memchr(val, 0, len)`
before calling `has_text`.

### N8 (Low) — corrupt `totalsize` is still trusted, and a truncated walk looks like a complete one

`shim/fdt.c:113-120`, `:423-426`

L2 bounds the walk by `fdt + totalsize`, but the header's `totalsize` itself is
unvalidated. A bogus large value (up to 4 GiB) extends `fdt_end` beyond DRAM,
and the walker can then follow a struct offset into MMIO; C1's guard does not
cover `fdt_walk`, so this is a bus hang.

Second, `FDT_END` (9) is not a `case` in the switch (`:139-208`), so a
well-formed tree falls into `default:` and returns 0 exactly like a bounds
failure. `fdt_probe_report` (`:423-426`) prints "invalid" only when the magic
is wrong; a walk that aborts halfway leaves a partial dump on screen with no
error line. The reader cannot tell "the FDT ended" from "the FDT was
truncated". Note that simply returning 1 for FDT_END would break
`fdt_chosen_u64`, which treats `fdt_walk(...) == 1` as "property found"; the
walker needs three outcomes (error / op-match / complete).

Fix: cap `totalsize` to a sane maximum (e.g. 2 MiB, or the region the live
FDT is known to occupy) and require `totalsize >= 40` plus
`off_dt_struct`/`off_dt_strings` inside it (already done). Then have
`fdt_walk` return 0 for error, 1 for callback match, and 2 for FDT_END; make
`fdt_chosen_u64` test `== 1`, `fdt_framebuffer` ignore the return, and
`fdt_probe_report` print "truncated" on 0. Add a host test with a clipped
`totalsize`.

## Pre-existing issues confirmed (not introduced by this branch)

These were present on `main` and are not round-1 regressions. They matter for
the next phase, so they are recorded rather than scored.

- **`resp[6]` is never checked.** `scsi()` reads `resp[7]` (correct: SCSI
  status) but ignores `resp[6]` (UPIU response code) and the residual/underflow
  flags (`shim/ufs.c:141-146`). A TARGET FAILURE response with SCSI status 0
  returns rc=0. Fix: require `resp[6] == 0` (TARGET SUCCESS) and check the
  residual fields before trusting data.
- **Sweet `test_ufs` 8 KiB PRDT.** `shim/ufs.c:268-269` reads 2 blocks in one
  command, exceeding the 4 KiB data unit on the same host class as M3. The
  bluejay path is fixed; this is sweet-only and unreachable on bluejay.
- **`host_test` relative path.** `./shim/out/host_test` from the repo root
  segfaults because it writes `out/host.ppm`; run it from `shim/` or fix the
  test to create the directory.
- **Unguardable MMIO reads.** `bluejay_probe` reads GICD/GICR and UFS CAP/HCS
  unguarded (`shim/shim.c:248-251`, `shim/ufs.c:292-294`), and `console.c:85`
  reads UFCON unguarded. A clock-gated block can stall the bus and no exception
  handler can recover it. This is the documented Gate 0 risk; if the first
  hardware run stops there, the next step is to move those probes behind the
  C1 guard, not to add more polling.

## Hunt checklist (explicitly requested)

| Area | Result |
|------|--------|
| Exception-vector register clobbering | No clobbering bug: `vent` stashes x29/x30, `exc_common` saves and restores x0–x28 before `eret`, and PSTATE comes from SPSR. The dump does read the wrong slot for X30 (N5), which is diagnostic-only. |
| C/asm ABI | No bug. `bl exc_handler` runs on a 16-byte-aligned SP; `-mgeneral-regs-only` means no FP/SIMD state to preserve; `secondary_entry` keeps x0 (context) through EL2 init; `secondary_main` uses `idx` under the `NR_CPUS` guard (the `sec_mpidr[idx]` write before the guard is pre-existing and only receives our own 1..7). |
| WFE not waking | Hardware risk, not a code bug: CNTKCTL EVNTEN/EVNTI=13 is programmed and CNTPCT is re-read each iteration, but the event stream must exist. Gate 0 item. |
| Tick-math overflow | No bug for the inputs used. `start + ticks` with at most 120 s × 24.576 MHz is ~2.95e9; 64-bit wrap would need ~23,800 years. `deadline = now() + freq/4` is fine. |
| FDT walker infinite loop | No. Every token advances `p` by ≥4 bytes, depth is capped at 15 open nodes, and every read is bounds-checked against `fdt_end` (subject to N8's totalsize trust). |
| Anything that could write to UFS | No current write path: CDBs are 0x00/0x25/0x28 and the UPIU flag is 0x40 for data commands; only HCI registers are written. The *check* that is supposed to keep it that way is broken (N1). |
| Sweet behaviour changes | M1 now fails safe to MMU-off on an unconfirmed FDT (intended), the FDT rewrite is compatible with the loader and chosen-property callers, and the only new sweet wart is the uninitialised `mmu_info` print (N4). The sweet `test_ufs` 8 KiB read is unchanged. |

## Hardware-unverified (Gate 0)

Still no device result: no `adb`, no `fastboot`, no boot log. The following are
code-reviewed but cannot be confirmed without `fastboot boot`:

- WFE wake from the generic-timer event stream (H1 residual).
- Whether bluejay ABL accepts the uncompressed `Image`; the LZ4 variant is
  ready (L7).
- Whether the bootloader's live FDT has the `/chosen/framebuffer*` node and
  the `soc@0/memory@...` layout assumed by M4/H3.
- Whether EL3 routes SErrors to EL3 (`SCR_EL3.EA`), which would bypass C1's
  recovery for asynchronous errors.
- Whether the AON watchdog is really at 0x10060000/0x10070000 and accepts the
  WTCON clear (M2).
- The PMU SMC and intr-gen offsets for CPU_ON (L4).

## Recommendation

The branch is close. Merge after:

1. **N1** — fix and self-test the write-opcode check before anyone trusts the
   documented safety invariant.
2. **N3** — update `NOTES.md`; the next agent will otherwise start from the
   wrong base and the wrong watchdog description.

Then, in the same pass if cheap: **N2** (two guards copied from `test_ufs`),
**N4** (`= { 0 }`), and the L4 retry logic (act on the PSCI status instead of
retrying unconditionally). N5–N8 can wait for the next cleanup; none of them
can write to storage or hang the current probe on the known tree.

Do not claim a hardware result from this review. The build, the host tests,
and `tools/test-bluejay-boot.py` pass, but the first real evidence is still a
`fastboot boot` of `shim/out/shim-boot.img` on a bluejay that reports the
right product string.
