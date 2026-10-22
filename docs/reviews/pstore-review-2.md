# pstore review 2 (worktree pstore, 7f6244a..fd45dcf)

Reviewer: Sonnet sub-agent, read-only, no adb/fastboot, no sudo, no subagents. Reviewed
b6bf6cd (D1), 4016281 (D2), a8bc231 (D3), 6f42a05 (D8), fd45dcf (docs). Kernel behaviour is
from memory of fs/pstore/ram*.c (no kernel tree offline): marked UNSURE where it matters.

## Summary

| Defect | Verdict |
|---|---|
| D1 panic-save recursion guard | FIXED (one Low residual: single global slot is clobberable by another CPU) |
| D2 reserved-table truncation | FIXED for pstore (fails closed); PARTIAL repo-wide (ufs.c consumer ignores the counter) and it likely disables pstore on the real phone (Med, availability) |
| D3 fallback restricted to GS101 window | FIXED |
| D8 marker survives tail-keep | FIXED |

No new High. No new write-outside-zone path found.

## D1 - panic-save recursion guard: FIXED

Evidence: shim.c:453 `pstore_panic_save()` replaces `pstore_save()` in `exc_handler`;
pstore.c:167-179 guard; pstore.h:27-31.

- Re-entry on the same CPU: `panic_save_cpu == self` -> `status("panic save re-entered; skipped")`
  and return (pstore.c:169-172). The guard is only cleared on normal return (:177), so a fault
  inside `pstore_save` leaves it set and the next nested entry skips. `exc_handler` then falls
  through to `reset(40)` (shim.c:454). Chain verified: nested -> con_clear/con_str prints (same
  calls the handler already made before the save) -> `status()` = `con_str` -> skip -> `reset(40)`.
  The skip path adds no fault surface beyond what `exc_handler` already executed above it.
- Checkpoint (non-panic) `pstore_save()` callers: a fault inside one gets exactly one full
  re-save through `exc_handler` (guard 0 -> set), a second fault is skipped. Bounded.
- Guard-clear path: `panic_save_cpu = 0` is a plain store to .bss; cannot fault. If an exception
  lands between `pstore_save` returning and the clear, the guard is still set -> nested skip. Fine.
- mpidr aliasing: `my_mpidr()` returns the whole `MPIDR_EL1` (pstore.c:106-114), unique per core,
  and bit 31 is RES1 so a real value is never 0 (0 = "no owner"/"cleared"). Host stub 0x1000.
  PASS.
- Other CPU: the slot is ONE global, not per-CPU, despite the NOTES wording "per-CPU guard".
  CPU B panicking while CPU A is inside its own panic save writes `panic_save_cpu = B`, B's
  `pstore_save` finds A owning `save_owner` and skips after 64 ldaxr tries, then B clears the slot
  (== self) -> A's guard is gone. If A then faults inside the save, A runs one more full save
  (guard re-set to A) and a second fault is skipped. Cost: one extra bounded re-entry per
  cross-CPU clobber (at most CPUs-1, each CPU dies in `reset`). Low, see N1.
- Not covered (pre-existing, not a regression): the guard protects only the save. A fault in
  `reset()`/`pager_replay()` re-enters `exc_handler` after the guard was cleared by a completed
  save, re-runs a full save and recurses again. The ARM exception path re-entering the pager is
  unbounded on one stack regardless of pstore. Low, see N2.
- Test: `test_panic_save_recursion_guard` re-enters from `status("saved bytes=")`, i.e. after
  the unlock, not mid-store; it proves skip + re-arm but not a true mid-write fault. Acceptable
  for a host test.

## D2 - reserved table truncation fails closed: FIXED (with caveats)

Evidence: fdt.c:365-377 (only site that appends to `reserved[]`), fdt.c:396 reset of the counter,
fdt.h:21, pstore.c:382-392.

- Only one append site exists (`collect_prop`, grep for `n_reserved` confirms); the else-branch
  counts every dropped reg tuple (per tuple, not per node). Banks overflow is still silent but
  pstore does not use banks.
- `pstore_init` fails closed on any drop (`return 0`, `ready` stays 0 from `pstore_reset()` at
  the top). Walked every `return 0` in `pstore_init` (:352-465): each prints and returns before
  `ready = 1` (:471). No branch arms writes on a gate failure.
- Overflow of `n_reserved_dropped` (unsigned): bounded by FDT size (<= 2 MiB, FDT_MAX_SIZE) and
  8 bytes/tuple minimum -> <= 262144 increments. No overflow. `put_dec(uint32_t)` fine.
- /memreserve block: still not consulted (out of scope, D6 left open by decision). Reserved
  nodes whose `reg` has an unsupported #cells (stride 0, fdt.c:354) are also silently skipped,
  not counted, as is a trailing partial tuple. Same class of "invisible reservation" as D2 but
  unchanged and Low.
- `fdt_read_reg_entry`'s failure `continue` (fdt.c:359) is dead code after the stride check.
- Side effects:
  - N3 (Med, availability, not safety): FDT_MAX_RESERVED is still 16. `docs/research/01-boot-path.md:51`
    lists ~9 GS101 no-map regions (gsa, tpu_fw, aoc, abl, logs, 4 modem...) plus splash, ramoops,
    and more; a real Pixel FDT plausibly exceeds 16, in which case pstore is now never armed
    and `PSTORE reserved overflow +N; skipped` appears. Safe, but the feature may be dead on the
    phone. The NOTES say the table was deliberately not enlarged. Decide before the hardware
    run: raising the cap to 64 costs ~2.5 KiB of stack in `pstore_init`.
  - N4 (Low): `ufs.c:765-769`/`:660` consumes the same `fdt_memmap` and ignores
    `n_reserved_dropped`; `reserved_hit()` can say "not in reserved range" for a dropped range.
    It only affects the placement diagnostic for the shim's own UFS DMA buffers (no UFS write
    opcodes), but it is the same silent-truncation pattern.

## D3 - fallback restricted to exact GS101 window: FIXED

Evidence: pstore.c:412-426, constants :83-84; reached only when `used_fallback` (set at :338-345).

- Check is `reserved[containing].base != FALLBACK_BASE || .size < FALLBACK_SIZE` -> skip. Exact
  base, >= 4 MiB, otherwise fail closed. The zone for the fallback geometry (record 0, no
  pmsg/ftrace, console 2 MiB) is `base+0..+2 MiB`, inside the matched window, and the other-range
  overlap, image, FDT and initrd gates still run after it.
- Live node with usable reg: unchanged. `used_fallback` is 0 when `fdt_ramoops` returned found
  (reg parsed), so the new block is skipped. I found no way for a node with a usable reg to be
  rejected by the new check.
- Node exists but malformed: `qualified_no_reg` (3-cell/absent/short reg) -> fallback -> only
  proceeds inside the exact window. A walk-level malformed FDT -> `fdt_ramoops` 0 -> fallback ->
  `fdt_collect_memmap` fails (:378) -> skip. Fail closed.
- `containing` is the FIRST covering reserved entry (:395-399). If a smaller overlapping entry
  precedes the window entry, the geometry check sees the wrong one and rejects (false negative,
  safe). Residual Low.
- A real window of different size (e.g. 0x200000) would be rejected: fail-closed by design.
- HANDOFF text no longer claims "console at base+2 MiB" for the fallback. Code puts console at
  base+0 (record_size 0 -> advance 0). Docs match code now.

## D8 - marker survives tail-keep: FIXED

Evidence: pstore.c:473-534 (`stream`, `pstore_save`), PSTORE_HDR :78, cap :237.

- Layout: marker, '\n', then lines. Single cursor `dst`; `written = dst` (:532). Every payload
  store is `zone_put8(PSTORE_HDR + *dst, ...)` (:483) and `zone_put8` compares against
  `zone_size` (the raw console_size; see D4 below, unchanged).
- Budget math: `total = marker_len + 1 + sum(len_i + 1)`; `skip = max(0, total - cap)`;
  marker+'\n' is never dropped; per line `d = min(drop, len)`, newline `d = min(drop, 1)`.
  Retained payload = payload - skip = cap - (marker_len+1), so `written == cap` exactly when
  `total > cap`, `== total` otherwise. No off-by-one at capacity (`size == buffer_size` is accepted
  by `persistent_ram_post_init`: it rejects only size > buffer_size or start > size - UNSURE,
  from memory). A line fully dropped also drops its newline only when `drop` is left over, so
  no stray blank lines.
- Edge cases: zero lines -> `written = marker_len+1`; empty line (len 0) -> `d=0`, newline kept
  or dropped by remaining `drop`; marker longer than the zone is impossible in practice
  (cap >= 4084 vs a ~40-byte marker; `PSTORE_MIN_CONSOLE` gate :364) and even then each store is
  still `zone_put8`-gated.
- Ring semantics (UNSURE, from memory of ram_core.c `persistent_ram_save_old`): old_log =
  `data[start..size)` then `data[0..start)`; with start=0, size=cap that is data[0..cap) in
  write order, i.e. marker first then the newest tail. Correct order.
- Torn write: sig zapped first (:497), data, dsb, size, start, sig last, dsb. Unchanged.
- Not a regression but still open: pass 1 / pass 2 divergence (D5) can make `written` exceed
  `cap`; kernel then drops the buffer; stores remain confined by `zone_put8` to `zone_size`.
- pstore-pull.py: `find(MARKER)` hits offset 0 in the truncated zone; 3 new python tests pass.
  The retained tail begins mid-line with no truncation indicator (cosmetic, Low).

## Bounds audit (objdump of out/pstore.o, `pstore_save`, aarch64-none-elf-objdump 15.2)

Disassembled all 265 lines of `<pstore_save>` and enumerated every byte/word store:

- Zone stores, each immediately preceded by `cmp wN, wM` + `b.ls/b.cs/b.cc` skip against
  `[x26,#8]` (= `zone_size`):
  - c24 `strb wzr,[x2,x0]` (sig zap): `cmp w1,w0; b.ls c28`
  - cd0 `strb w1,[x3,x0]` (marker): `cmp w2,w0+12; b.ls cd4`
  - f44 `strb w0(0x0a),[x5,w27,uxtw]` (marker newline): `cmp w27,w2; b.cc f40` (w27 = marker_len+12)
  - d44 `strb w6,[x7,w4,uxtw]` (line bytes): `cmp w4,w5; b.cs d48`
  - d7c `strb w24,[x1,w0,uxtw]` (line newline): `cmp w1,w0; b.ls d80`
  - dbc size bytes: `cmp w1,w3; b.ls dc0`; ddc start bytes: `cmp w1,w3; b.ls de0`
  - e08 sig bytes: `cmp w1,w0; b.ls e0c`
- All other stores are on the stack: e64, eb8, f04, f1c, f7c (`line[]` formatting through
  `x9 = sp+0xc0`), and `str xzr,[x26,#32]` (e2c, `save_owner = 0`, in `save_unlock`).
- Result: every zone store is dominated by a bounds compare. PASS. (The bound is `zone_size`
  = raw console_size; D4 is unchanged, see below.)

## Real output

- `cd shim && ./build.sh --board bluejay --avb`: tail shows `PASS ufs safety self-test: 8/8
  known-bad sources rejected`, `PASS header v4 (lz4): kernel=31243 ramdisk=98; host tests &
  safety checks passed`, out/ listing (Image 290816, shim-boot-avb.img 67108864). (the
  `EXIT=0` printed there is `tail`'s status; the build itself ran under `set -e` and printed its
  PASS lines.)
- `./build.sh --board bluejay --avb --pstore-fast` (checked with PIPESTATUS): EXIT=0, `PASS header
  v4 (lz4): kernel=31194 ramdisk=97`, same listing. This second build overwrote shim/out, so the
  next test ran on the pstore-fast image.
- `python3 tools/test-bluejay-boot.py shim/out/shim-boot-avb.img` -> `PASS header v4 (raw):
  kernel=290816 ramdisk=97; host tests & safety checks passed`.
- `./out/host_test` EXIT 0; `./out/ramlog_host_test` -> `PASS ramlog host test: page count, copy,
  wraparound, overflow, whole-line eviction`; `./out/fdt_host_test` EXIT 0 (silent);
  `./out/pstore_host_test` -> `PASS pstore host test: fdt ramoops parse, zone math, encoding,
  tail-keep, safety gates`.
- `python3 -m unittest discover -s tools -p "test_*.py"` -> `Ran 15 tests ... OK`.
- `python3 tools/validate-repo.py` -> `Validated 81 patches ... 19 Python tools ... 8 Shell
  scripts ... All repository integrity checks PASSED successfully!`.
- `git status` in the worktree after: clean (build outputs gitignored).

## Unchanged by decision (not re-reviewed beyond impact)

D4 (raw vs floored console size in `zone_size`, pstore.c:~439/:469), D5, D6, D7, D9-D11.
D4+D5 together are the only route to a store past the kernel's zone for a non-pow2
console-size; the store is still inside the gated reserved range.

## New defects, ranked

### High
- none.

### Medium
- N3. FDT_MAX_RESERVED=16 plus the new fail-closed D2 gate probably means pstore never arms on a
  real Pixel 6a FDT (GS101 rmem has well over 10 carve-outs plus splash/ramoops/etc.). Safe, but
  the hand-back feature may silently not work. Consider raising the cap (e.g. 64) before the
  hardware run, and/or read the `UFS MAP rsv=` count from a probe to confirm.

### Low
- N1. D1 guard is one global word, not per-CPU: another CPU's panic clobbers/clears it, giving
  at most one extra bounded re-entry (pstore.c:165-179). Per-CPU slot or CAS if wanted.
- N2. Recursion in `reset()`/`pager_replay()` is not guarded (pre-existing); the guard re-arms
  after a completed save, so such a loop re-runs the save each time.
- N4. ufs.c ignores `n_reserved_dropped` (diagnostic only).
- N5. Reserved nodes with unsupported #cells / partial trailing tuples are skipped uncounted,
  and /memreserve is still ignored.
- N6. "containing" is the first covering reserved entry; the D3 geometry check can then reject a
  valid window (fail-closed).
- N7. Tail-keep leaves no "truncated" indicator in the retained text (cosmetic).

## Verdict

D1, D2, D3 and D8 are all fixed with no new unsafe path (every zone store still dominated by a
bounds compare, all gates fail closed); the one real concern is that the D2 fail-closed gate may
leave pstore disabled on the phone because the table is still 16 entries.
