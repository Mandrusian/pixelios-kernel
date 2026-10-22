# pstore review 1 (branch t3/pstore-log, fda0328..7f6244a)

Reviewer: Sonnet sub-agent, read-only, no adb/fastboot, no sudo. Kernel
behaviour below is from the shim's own header citations and my memory of
fs/pstore/ram*.c; I had no kernel tree offline, so those points are UNSURE
unless stated.

## 1. Stores in pstore.c: bounds dominate every store  -- PASS (one caveat)

- Every zone store goes through `zone_put8()` (shim/pstore.c:158-162), gated by `off < zone_size`.
  `zone_put32` (:164) is four `zone_put8`. `stream()` (:431) passes `PSTORE_HDR + (at - skip)`
  as u32; `at >= skip` there so no underflow; `off` cannot wrap (<= 12 + 32 KiB).
- objdump of out/pstore.o `<pstore_save>` (aarch64-none-elf-objdump): zone strbs at a64, b24, b50,
  ba8, be4, c24, c70 each preceded by `cmp wN,wM` + `b.ls/b.cs` skip. The `strb wzr,[x0]` at c44
  is the stack `line[p]=0`. No other byte stores.
- Zone math (:177-214): `used` is u64 from three u32 so cannot overflow; `used > region_size`
  returns -1 before the subtraction (no underflow); `dump/record_size`, `dump/cnt` guarded
  (`record_size!=0`, `cnt==0` -> -1); `zone &= ~1`. Zero console -> -1. Non-pow2 inputs are
  floored. reg cells 1 or 2 only (`fdt_reg_stride` fdt.c:~117 rejects 0/3+).
- Caveat (Low, see D4): the bound is `zone_size = ro.console_size` (:415) but the math uses
  `pow2_floor(console_size)`. For non-pow2 console-size the runtime bound is larger than the
  kernel's zone, and `zone_cap` (:416) is the floored one. Only reachable if the pass-1/pass-2 totals
  diverge (D5) or for the signature/header words; still inside the gated reserved range.

## 2. Gate logic  -- PASS with gaps

- Containment: `range_covers` (:225) with `zb+zs` wrap check (:343) -> fails closed when the zone
  is not wholly inside one reserved entry (:364). Disjoint from the other 15 entries (:376),
  image+both stacks (:392-396; shim.ld puts `_stack_top` and `_sec_stacks` below `_end`), FDT
  blob (:399-403) and initrd (:405-411). Every failure prints and returns 0 with `ready==0`,
  and `pstore_save` is a no-op when `!ready` (:438).
- Malformed FDT: `fdt_walk` returns 0 (bad magic, totalsize > 2 MiB, truncated tokens), so
  `fdt_ramoops` returns 0 -> fallback struct, then `fdt_collect_memmap` fails (:353) -> skip. Missing
  FDT (dtb==0) same. Fails closed.
- Gaps (see D2, D3, D6): 16-entry reserved table truncates silently; /memreserve block not
  consulted; unusable-reg ramoops falls back to a hardcoded guess address (still gated).
- `range_overlaps` (:218) `b2+s2` can wrap for a garbage reserved entry and give a false
  "no overlap" (Low).

## 3. Ordering, reentrancy, multi-CPU, bounded loops  -- PASS with defects

- Order: sig zap -> dsb (:447-448); data; dsb (:475); size, start, sig last; dsb (:476-479).
  Byte-wise LE stores, sig bytes cannot form a valid sig partially (old sig was zeroed first).
  MMU is off (Device memory) so no cache maintenance needed. Sound.
- Lock (:122-146): same-CPU re-entry allowed; other CPUs get 64 `ldaxr` tries then skip. Bounded.
  Unlock clears only if owner==self. Exception mid-save on CPU A: handler on A re-enters, rewrites
  from scratch (sig zapped again first), unlocks. Exception on CPU B while A saves: B's panic save
  is skipped (Low, accepted).
- Loops bounded: 2 passes over `ramlog_lines()` (<= 32768/1), per-line copy capped at 96.
- No recursion guard: if a zone store (or ramlog) faults inside `exc_handler`->`pstore_save`,
  the same-CPU re-entry path lets the fault recurse indefinitely on the same stack (D4/D7).
  The 0x8000 gap below `_stack_top` is the only margin before .bss/ramlog is overwritten.
- Performance (D7): `ramlog_copy_line` (ramlog.c:66-86) is O(used) per call and `pstore_save` calls
  it for every line in both passes: ~lines x 32 KiB x 2 uncached Device reads per save (order of
  tens of millions of loads at the full 32 KiB log), done ~14 times per run and also in the
  panic path before `reset(40)`. Not a correctness bug, but long saves delay checkpoints and the
  "pstore copy survives" intent.

## 4. Layout vs kernel  -- PASS / UNSURE

- Sig 0x43474244 written as LE bytes 44 42 47 43 ("DBGC"), start @+4, size @+8, data @+12, LE u32:
  matches `struct persistent_ram_buffer` {u32 sig; atomic_t start; atomic_t size; u8 data[]}
  (PASS; matches ram_core.c as cited).
- start=0, size=len (<= cap = console_size-12): kernel `post_init` accepts (size<=buffer_size,
  start<=size); old log = data[0..size). Tail-keep when it overflows: writes the last `cap`
  bytes with start=0,size=cap -> kernel reads them in order (PASS). `skip` drops whole head bytes,
  so the first retained line can be cut mid-line (cosmetic; the marker line is then lost: the
  marker is the first thing written, so after a >32 KiB log `pstore-pull.py` finds no marker,
  Low/Med, D8).
- Console offset: record 0 -> 0 advance (PASS vs kernel as I remember it for >= 5.x); dump>0 &
  record>0 -> cnt*zone_sz (matches ramoops_init_przs); pmsg/ftrace after console so they don't
  shift it (matches ramoops_probe order dmesg, console, pmsg, ftrace; UNSURE for older Android
  5.10 order where ftrace/pmsg ordering differs, doesn't move console either way).
- UNSURE: `cnt==0` (record_size > dump): shim skips; some kernel versions instead return 0 without
  advancing paddr and put console at base. Skipping is conservative (never writes the wrong place).
- ecc-size != 0 -> skip (PASS, correct reasoning in header; kernel would RS-"correct" raw bytes).
- Kernel zone size uses the floored console size; shim `zone_size`/gates use the raw size (D4).
- mem-type ignored (fine: MMU off).

## 5. tools/pstore-pull.py  -- PASS (Low items)

- Only mutating device commands: `fastboot boot <image>` (:169), `adb reboot bootloader` (:250).
  No flash/erase/format/set_active/oem/flashing/erase anywhere (grep). `adb shell cat <fixed paths>`
  and `adb bugreport <tmpfile>` read-only. 
- Timeouts on every external command via `run_cmd` (60/180/120/600/120 s); polling loops are
  deadline-bound. Product check `fastboot getvar product == bluejay` before boot (:156-166).
- Low: no `-s <serial>` pinning; `fastboot getvar`, `fastboot boot`, `adb devices/shell` can each
  address a different device if two are attached (the lead's phone is meant to be the only one).
  `fastboot boot` success is detected by substring "OKAY" in output (fine).
- Low: `adb_state` ignores serials; accepts any adb device in `device` state.
- Extraction handles both plain text and raw buffer header; ring order matches the kernel.

## 6. Real output

- `cd shim && ./build.sh --board bluejay --avb` : EXIT=0, ends with "PASS header v4 (raw)... PASS header
  v4 (lz4)..." and out/ listing (Image 290816, shim-boot-avb.img 67108864).
- `./build.sh --board bluejay --avb --pstore-fast` : EXIT=0, same ending (lz4 kernel=30801 vs 30831).
- `python3 tools/test-bluejay-boot.py shim/out/shim-boot-avb.img` -> "PASS header v4 (raw): kernel=290816
  ramdisk=98; host tests & safety checks passed", EXIT=0.
- host_test EXIT=0; ramlog_host_test "PASS ramlog host test: page count, copy, wraparound, overflow,
  whole-line eviction"; fdt_host_test EXIT=0 (silent); pstore_host_test "PASS pstore host test: fdt ramoops
  parse, zone math, encoding, tail-keep, safety gates".
- `python3 tools/validate-repo.py` : "Validated 81 patches ... 19 Python tools ... 8 Shell scripts ... PASSED", EXIT=0.
- `python3 -m unittest discover -s tools -p "test_pstore*.py"`: Ran 12 tests, OK.
- git status after: clean (outputs only in gitignored shim/out/).

## Defects (ranked)

### High
- none.

### Medium
- D1. No recursion guard on the panic-path save (shim.c exc_handler -> pstore_save; pstore.c:122-127).
  A fault inside pstore_save/ramlog re-enters on the same CPU and recurses on the same stack until it
  tramples .bss (ramlog) / data. Fix: a "in exc_handler" flag that skips pstore_save on a nested panic.
- D2. Silent truncation of the reserved table at FDT_MAX_RESERVED=16 (fdt.c collect_prop; fdt.h:5).
  Reserved ranges beyond #16 are not in the overlap gate; a real Pixel FDT can have more than 16. If the
  ramoops range itself is beyond #16 the gate fails closed (no log); if an overlapping neighbour is beyond
  #16 it is missed. Fix: report overflow from `fdt_collect_memmap` and make `pstore_init` skip (or raise
  the cap).
- D3. Fallback address path: when the ramoops node has no usable reg (3/4-cell, absent), `pstore_init`
  guesses base 0xfd3ff000 (:312-323) and writes there if it happens to sit in ANY reserved range with no
  other overlap. That range need not be ramoops memory (could be another driver's carve-out). Safer: no
  fallback for a missing/invalid reg, or require the containing reserved entry's name to look like ramoops.

### Low
- D4. `zone_size` = raw `console_size` (:348, :415) while the math and `zone_cap` use the pow2-floored
  size; the runtime bound (and the containment gate) is looser than the kernel's zone for non-pow2
  console-size. Use the floored value for both.
- D5. Pass 1 / pass 2 can see different ramlog contents if another CPU logs in between (ramlog has no
  lock); `written` can exceed `zone_cap`, so the stored size could exceed the kernel's buffer_size and the
  kernel would drop the log. Clamp `written` to `zone_cap` (and bound `at - skip` by `zone_cap`).
- D6. /memreserve block of the FDT and the u64 wrap in `range_overlaps` (:222) are not handled.
- D7. `pstore_save` is quadratic-ish via `ramlog_copy_line` (see section 3); also runs in the panic path.
- D8. Tail-keep drops the marker line when the log exceeds the zone capacity; `pstore-pull.py` then says
  "marker not found". Re-emit the marker at the start of the retained data, or search for "PIXELIOS" lines.
- D9. `fdt_ramoops` writes fields from non-qualified `/reserved-memory` siblings into `out` as they stream
  by (fdt.c ramoops_prop; only `reg` is overwritten by the real node), so stale values can leak into a later
  node that lacks, e.g., `pmsg-size`. Practically harmless, wrong in principle.
- D10. pstore-pull.py has no `-s` serial pinning (see section 5).
- D11. "saved bytes=" line is logged after the save, so it only reaches pstore on the next checkpoint
  (the final save's own result is never in the pulled log). Cosmetic.

## Verdict

Safe to try on the phone (stores are bounded by runtime checks and gated by reserved-range containment, fail-closed on bad/missing FDT), but fix D1-D3 and D8 before relying on it for diagnosing panics.
