# Review 2: usbprobe review-1 fixes (`t3/usb-probe`)

Scope: `git diff f7a96d4..6a7fbc0`, 4 commits: `741cd4b` object audit (H1), `0c7bd13` labels (L1), `4f5d973`
ramoops overlap flag + `pstore_zone_size()` (L2), `6a7fbc0` host tests (L3). Read-only review: no adb, fastboot,
sudo or source edits. Reviewer: Claude Opus 5.5, 2026-10-20. Scratch files: `/tmp/usbrev2/` (not in git).

## Bottom line

**Safe to fastboot-boot `6a7fbc0` with respect to USB state.** A fresh objdump of `shim/out/usbprobe.o` shows
zero non-stack stores, no indirect branches, and no foreign out-pointers passed to callees (see §5). The new
audit's holes (below) do not show up in this object. The residual risk is unchanged from review 1 (M2,
UNSURE): a read from a gated block could stall instead of faulting, and the watchdogs are off.

**The object audit is not a sound gate yet.** It caught 10 of the 27 hostile objects I built. **17 got through**,
and 12 of those compile from plain C that also passes the source scan. The worst is an ordinary `switch`: GCC
15 emits a relative jump table with no ABS64 relocation, so the build's ABS64 check doesn't stop it either. Fix
this before stage 2 or any further edits to `usbprobe.c`. It does not block this boot.

`tools/check-all.sh` → **`GATE PASS 6a7fbc0`** (run by me; the tree was clean apart from the untracked worker
report).

## Verdicts

| Item | Verdict |
|---|---|
| H1 checker bypassable | **PARTIAL.** The object audit is a large improvement and fails closed on unknown mnemonics, `.inst`/data, LSE, smc/msr/dc, non-stack bases, and stores in unreachable blocks. But its dataflow trusts things it can't prove (H2-H5 below). The source scan still passes 10 of the 11 old review-1 spellings I re-tried. It is now only a pre-filter, which is acceptable if the object audit is fixed. |
| L1 labels | **FIXED.** Every label is `"USB"`, and the register name now leads the value. ramlog/pstore/UART text is byte-identical: the existing exact-line host asserts are unchanged and pass. The PHY row is rebuilt as `"PHY+xx "` + values, which gives the same `USB PHY+00 …` line. |
| L2 ramoops overlap | **FIXED.** The getter is read-only and returns 0 unless the zone is armed (details in §3). Not-ready/off prints `EVBUF ramoops zone off; no flag`. No pstore behaviour change. |
| L3 host tests | **MOSTLY FIXED.** Added: addr 0, wrap, truncated table, partial overlap, and a per-address single-read counter. Still missing: the fake `con_str` (`usbprobe_host_test.c:23-30`) still doesn't apply the 96-byte cap. The worker report says L3 is fully fixed, which is not quite true. |

## 1. Bypass table (object audit, `check_usb_object()` on real `.o` files)

These were compiled with the shim's exact CFLAGS (`-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only
-mstrict-align -fno-stack-protector`) via `aarch64-none-elf-gcc` 15.2. The "src scan" column is
`check_usb_safety_text()` on the same C. Each PASS was confirmed by reading its disassembly.

| # | Hostile object | Audit | src scan | Note |
|---|---|---|---|---|
| 13b | `switch` with 12 cases, one case sets `d = (char *)fdt`, then `*d = 1` | **PASS** | PASS | jump table `ldrb`+`adr`+`br x0`, 0 ABS64. Case blocks are not CFG successors → treated unreachable → excluded from the join meet |
| 08 | `try_read32(B+0xC70C, (uint32_t *)(B+0xC704))` (out-pointer = MMIO) | **PASS** | PASS | the callee does the store; call arguments are never checked |
| 09 | `fdt_collect_memmap(fdt, (struct fdt_memmap *)B)` | **PASS** | PASS | same: a sanctioned callee writes through a foreign pointer |
| 03 | static `put_txt((char *)(B+0xC704), …)` | **PASS** | PASS | the `put_*` x0 rule assumes callers pass stack; call sites are unchecked |
| 07 | `line[v] = 1` with `v` from `try_read32` | **PASS** | PASS | `strb w1, [x2, x0]`: the index register is never checked |
| 21 | `char *q = line + (v << 20); *q = 1` | **PASS** | PASS | `add`/`[base, reg]` with a stack base and any register stays "stack" (line 744-759) |
| 01c | stack array of pointers: `ptrs[k] = fdt` (k<4), then `ptrs[0][0] = 1` | **PASS** | PASS | `str x20, [x0, w1, uxtw #3]` overwrites slot 64 via a non-sp base; the slot map still says "sp" |
| 14 | `union { char *p; uint8_t b[8]; }` with device bytes into `b[3]`,`b[4]`, then `*u.p = 1` | **PASS** | PASS | `strb` to sp+43/44 doesn't invalidate the 8-byte slot at sp+40 |
| 15 | `char *volatile p = line; try_read32(B, (uint32_t *)&p); *p = 1` | **PASS** | PASS | `bl` kills registers but not stack slots whose address escaped |
| 11 | call through a function pointer `(fn)fdt` | **PASS** | PASS | `blr` is treated as a plain call (`_USB_NO_WRITE`, line 508) |
| 12b | `static const uint32_t code[] = {str w0,[x1]; ret}` called via `blr` | **PASS** | PASS | executes unaudited bytes from `.rodata` |
| 16 | `static void put_evil(uint32_t *d) { *d = 0; }` called with MMIO | **PASS** | PASS | any static `put_*` name is trusted |
| 10 | `extern void ufs_write32(...) __attribute__((weak))` | **PASS** | caught by name only | `nm -u` prints `w`; the parser keeps only `U` (line 860). Renamed (e.g. `ufs_wr32`), it passes both |
| 24b | asm: `b` to `.text.unlikely` (reloc), which redefines x19 and `b`s back mid-function | **PASS** | n/a (.S) | the audit ignores relocations: `b 0 <usbprobe_report>` is read as a tail call |
| 25 | asm: local symbol `put_x` mid-function, x0 = MMIO before it | **PASS** | n/a | a symbol splits the function; the second half starts with "x0 = stack" |
| 26 | asm: spill a stack pointer to [sp,#8], `add sp, sp, #16`, reload [sp,#8] | **PASS** | n/a | sp moves don't rebase or clear the slot map |
| 27b | asm: `st1 {v0.4s}, [x1], x2` then `str w0, [x1]` | **PASS** | n/a | register post-index writeback doesn't kill the base |
| 02 | `str` via x0 in a non-`put_` static | caught | PASS | |
| 04 | store through a returned pointer | caught | PASS | |
| 05 | `stp` through `fdt` | caught | PASS | |
| 12 | data in `.text.z` via `blr` | caught | PASS | only because `.text.z` is exec, disassembled, and fails closed. 12b (`.rodata`) passes |
| 13 | 8-case switch (compare tree) | caught | PASS | |
| 17 | `cold` noinline volatile store | caught | caught | |
| 18 | `__atomic_fetch_or` (outline helper) | caught (undef) | caught | |
| 19 | LSE `ldset` (`target("arch=armv8.1-a")`) | caught | caught | |
| 20 | `target("+simd")` store loop | caught | PASS | compiled to scalar `str`, not `st1`. 27b covers `st1` |
| 24 | asm cross-section with no other path to the store | caught | n/a | by luck: "unreachable block" |

Not reproduced: GCC hot/cold block partitioning (`.cold` parts jumping back mid-function). At -O2 without
profile data it did not split (test 23). If it ever does, it is the same defect as 24b. UNSURE.

## 2. Does the audit gate the real build?

Yes. `main()` runs `check_usb_object(repo/shim/out/usbprobe.o)` (`tools/test-bluejay-boot.py:1050`).
- `build.sh:93-94` (`set -euo pipefail`) runs it for both images on bluejay.
- `check-all.sh` reaches it three ways: the `build` step, the `build-fast` step, and the `boot-safety` step.
  `gate.log` shows `PASS usb object audit: 119 stores` at lines 52, 101 and 162.
- A violation raises an uncaught `AssertionError`, so python exits 1, the `step` fails, and the gate prints
  `GATE FAIL`. I confirmed that `check_usb_object` raises on objects 02/04/05.
- I did not inject a bad object into `shim/out` (no edits).

Weak spots (Low):
- No `n_stores > 0` floor in `main()`.
- The objdump exit-status check and the file-exists check are plain `assert`s (lines 849, 854). Under
  `PYTHONOPTIMIZE`, a failed objdump yields empty text and the audit passes with 0 stores.
- The worker report claims 113 stores; the gate shows 119.

## 3. `pstore_zone_size()` and the overlap flag

- `pstore.c:565-568` returns the static `zone_size`. It writes nothing.
- `zone_size` is set only at `pstore.c:467`, together with `zone_base`/`ready=1` after all validation
  (465-469). It is zeroed only in `pstore_reset()` (host tests). So it is 0 whenever pstore is not armed: not
  ready, rejected zone, or PSTORE off.
- `usbprobe.c:327-335` reads base and size. `!rz_size` → "zone off; no flag". Otherwise it calls
  `range_overlaps` (0 sizes → 0; inputs are already wrap-checked at 266).
- `pstore.c` diff: +5 lines, getter only. No change to pstore behaviour.
- The flag covers only the console zone, not other ramoops records. That's acceptable, because the console
  zone is what gets pulled. The flag reports a live-DMA hazard but does not mitigate it (by design).

## 4. Label change

Every `emit`/`con_str` now uses the label `"USB"`, and the old label text leads the value, so `label + " " +
value` is unchanged. This is pinned by the unchanged `assert_line(...)` strings in the host test, which passes
(`gate.log:4`). The longest value is still under the 95-byte cap. Side effect (Low, informational): values are
now longer on screen, so more of each line is clipped at about 44 columns. For example, `softrst` on the
GUSB2PHYCFG0 line is cut off. UART/ramlog are unaffected.

## 5. Fresh objdump of `shim/out/usbprobe.o` (gate build of 6a7fbc0)

- Sections: `.data` = 0, `.bss` = 0.
- `nm -u` = {con_str, fdt_collect_memmap, try_read32, pstore_zone_addr, pstore_zone_size, _head, _end}. There
  are no weak undefined symbols.
- No `br`, `blr`, `ret xN`, smc/hvc/msr/dc/ic/at/tlbi or LSE. So no jump tables or indirect calls (the 13b/11
  class is absent).
- Stores by base: sp ×45, x24 ×33, x0-x8 ×41 (helpers, plus inlined `put_txt`/`put_hex` off x24).
  - x24 = `add x24, sp, #0x130` (`line`, 96 bytes up to sp+0x190). It is defined once at 0x248 and otherwise
    only restored by the epilogue `ldp`.
- All 43 `bl/b put_*` calls in `usbprobe_report` take `x0` from `mov x0, x24`. This is a linear check, with no
  intervening x0 definition.
- Call-site out-pointers:
  - All 20 `try_read32` calls pass `x1 = sp+0x194`.
  - `fdt_collect_memmap` gets `x1 = sp+0x190` (the `mm` slot reused after the last read).
  - `con_str` gets `x1` = rodata or x24.
- Register-offset stores (`[x24, wN]` etc.) are bounded in the source: the `pos + 1 < cap` loops, `emit`
  clamps to 95 (87 `#0x5f/#0x5e` compares), and `put_dec` `tmp[10]` has `n < 10`.
- No pointer arrays or unions in the frame. Spill slots 104-160 are below `line` (0x130).

**Result: zero stores to non-stack addresses. Device values are never used as a base or an unbounded index.**

## New defects

### High
**H2. A jump-table `switch` bypasses the object audit** (`tools/test-bluejay-boot.py:633-634, 650-651,
798-826`).
- `br` ends a block with no successors. The jump-table targets become "unreachable" blocks. Unless they
  contain a store, they're silently dropped from the meet, so a pointer they define reaches the join store
  unchecked (test 13b).
- GCC 15 emits relative byte tables (`ldrb`/`adr`/`add`/`br`) with **no** ABS64 relocation. So the comment at
  `usbprobe.c:~142` ("jump table … ABS64 relocations the build rejects") is false for this toolchain, and
  `build.sh:46-47` doesn't catch it.
- Fix: fail closed on `br`, `blr` and `ret xN` (with x30 the only exception). The real object has none. Also
  fix the comment, or add `-fno-jump-tables` to CFLAGS.

### Medium
**M3. Callee out-pointers are unchecked** (`:782-784`). Covers tests 08, 09 and 03.
- `try_read32`'s `x1` and `fdt_collect_memmap`'s `x1` can be any address; the sanctioned callee then does the
  write.
- The x0 dst of a static `put_*` helper is trusted, but call sites are never checked.
- `try_read32(reg, (uint32_t *)reg2)` is a one-typo bug in exactly the idiom this file uses 20 times.
- Fix: at each `bl`, require `cls(x1) == "sp"` for try_read32/fdt_collect_memmap and `cls(x0) == "sp"` for
  `put_*` targets.

**M4. Unbounded register index/offset off a stack base is accepted** (`:708-716, 744-759`). Covers tests 07
and 21. `[xS, xI]` and `add xN, xS, xI` keep the "stack" class whatever `xI` is, so `line[device_value]` can
write anywhere. The audit proves the base's provenance, not the bounds. At minimum, say so in the docstring and
NOTES (today it says "provably the stack"). Optionally, fail on register-offset stores where `xI` derives
from a load result (taint).

**M5. Slot map is unsound** (`:717-743`). Covers tests 01c, 14, 15 and 26.
- Slots aren't invalidated by stores into the stack via a non-sp base, by partial or misaligned overlaps
  (`strb` into bytes 1-7 of an 8-byte slot), by `bl` when the slot's address escaped, or by sp adjustments.
- Simplest sound fix: clear the slot map on any store not of the form `[sp, #imm]`, on every `bl`, and on any
  sp write. Also invalidate overlapping slots by byte range. The real object only needs spills reloaded
  within straight-line code. UNSURE whether that holds across its `bl`s; if not, keep callee-saved registers
  instead.

**M6. Indirect calls are allowed** (`:508, 782`). Covers tests 11 and 12b: `blr` to anything, including bytes
in `.rodata`. Fixed by H2's ban.

### Low
- **L6.** `nm -u` parsing drops weak undefined symbols (`w`) (`:858-861`), test 10. Accept every
  `nm -u` line.
- **L7.** Relocations are ignored for `b`/`bl` targets (`:562, 588-594`), test 24b (asm only). A
  `R_AARCH64_JUMP26` against a section symbol is read as a tail call to entry 0. Fail closed on any branch
  carrying a relocation whose symbol isn't a function in this object.
- **L8.** Any symbol starts a new "function" with a fresh init state (`:556-560`), test 25 (asm only).
  Combined with the `put_` name rule, `put_*` x0 trust is name-based (test 16). Require put_ helpers to be
  real function entries (`nm` type `t` *and* reached only by `bl`), or drop the rule after M3's call-site
  checks.
- **L9.** Register post-index writeback doesn't kill the base (`:708-725`), test 27b (asm only).
- **L10.** `.data`/`.bss` check matches exact names only (`:543-549`), test 22. It ignores `.data.*`,
  `.bss.*`, `.sdata`, COMMON. Stores are still audited, so this is about hygiene only.
- **L11.** The audit can pass with 0 stores. No floor, and its `assert`s vanish under `PYTHONOPTIMIZE` (§2).
- **L12.** The fake `con_str` still has no 96-byte cap (L3 leftover). The on-screen clipping grew (§4).

## UNSURE
- M2 (from review 1): whether gs101 stalls rather than faults on gated USB/PHY reads.
- Whether GCC's hot/cold partitioning can be triggered with these flags (not observed).
- The M5 simple fix may reject the current object if it relies on spills across `bl`. Check before
  tightening.

## Commands run
- `tools/check-all.sh` → `GATE PASS 6a7fbc0`
- `aarch64-none-elf-objdump -dr / -h`, `nm`, `size -A` on `shim/out/usbprobe.o`
- 27 hostile objects (22 C, 5 asm) in `/tmp/usbrev2/c/`, fed to `check_usb_object()` via `/tmp/usbrev2/run.py`
- `check_usb_safety_text()` on those sources and on 11 review-1 spellings
- Python call-site scans for `put_*` x0 and `try_read32`/`fdt_collect_memmap`/`con_str` x1
