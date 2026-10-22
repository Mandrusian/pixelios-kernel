# Review 1: stage-1 read-only DWC3 USB probe (`t3/usb-probe`)

Scope: `git diff cca4b25..7a5d7ca`, which is 3 commits: `5265e05` probe + host test + wiring, `fd2a531` checker,
`7a5d7ca` NOTES. This was a read-only review: no adb, fastboot or sudo, and no source edits.
Reviewer: Claude Opus 5.5, 2026-10-20.

## Bottom line

**It is safe to fastboot-boot this image on the phone with respect to USB state.** In `usbprobe.o`, every
store goes to the stack. Every MMIO access is a 32-bit load through the out-of-line, fault-guarded `try_read32`,
and the address is either a constant or a constant plus a loop counter. No value read from the device is ever
dereferenced. The registers it reads have no read side effects in the DWC3 core (details below).

One residual risk can't be removed in software: on Exynos, a read from a clock- or power-gated block may stall
the bus instead of faulting (UNSURE). Because the watchdogs are disabled earlier in `bluejay_probe`, a stall
would hang the phone until a forced reboot. Nothing is written, so nothing persists. **Before booting, apply
M1** (one added `pstore_save()` line). Without it, a stall in the USB probe also loses the UFS section's log.

## Verdicts

| # | Item | Verdict |
|---|------|---------|
| 1 | No MMIO write or side-effecting read on the probe path | **PASS** (source and objdump) |
| 2 | Fault guard: every read guarded, handler resumes, a gated block gives a clean report | **PASS** if the block faults. Residual stall risk (M2, UNSURE) |
| 3 | Device-derived addresses never dereferenced | **PASS** |
| 4 | Output format and size vs ramlog/pstore/screen | **PASS** for ramlog/pstore/UART. Screen labels collide (L1) |
| 5 | Safety checker regexes | **WEAK**: 21 of 22 tried bypasses get through (H1). The objdump audit is the real guarantee |
| 6 | Integration ordering in `shim.c` | **PASS with defect M1** (no checkpoint between UFS and USB) |
| 7 | Host test adequacy | **ADEQUATE for the logic**. Gaps listed (L3) |
| - | `tools/check-all.sh` | **`GATE PASS 7a5d7ca`** (run by this reviewer, tree clean) |

## Defects

### High

**H1. The USB safety checker is easy to bypass and its self-test overstates coverage**
(`tools/test-bluejay-boot.py:249-340`). Evidence: I imported the module and ran `check_usb_safety_text(real
usbprobe.c + snippet)`. Each line below returned **without raising**:

| Bypass | Snippet |
|---|---|
| non-volatile cast store | `*(uint32_t *)(USBPROBE_DWC3_BASE + 0xC704) = 0;` |
| cast-then-index (no leading `*`) | `((volatile uint32_t *)USBPROBE_DWC3_BASE)[0x31c1] = 0;` |
| memcpy / builtin memset | `memcpy((void *)(BASE+0xC704), &v, 4);` `__builtin_memset((void *)BASE, 0, 4);` |
| shim's own helper name | `wr32(BASE + 0xC704, 0);` (`_MMIO_WRITE_ID` only knows `*write32*`/`writel`) |
| other kernel widths | `writeb`/`writew`/`writeq`/`__raw_writeb` (regex matches only `writel`) |
| typedef'd volatile | `typedef volatile uint32_t reg_t; reg_t *r = ...; r[1] = 0;` |
| `volatile T *const r` | `_volatile_pointer_vars` captures the name `const`, not `r` |
| volatile pointer parameter | `static void f(volatile uint32_t *r) { r[1] = 0; }` (`)` is not in the terminator set) |
| compound assign via a pointer | `*r \|= 1; r[1] += 1;` (patterns need `=` right after the lvalue) |
| increment | `(*(volatile uint32_t *)BASE)++;` |
| macro-hidden lvalue | `#define REG(a) (*(volatile uint32_t *)(a))` then any `;` line, then `REG(BASE) = 0;` |
| token paste | `CAT(try_wr,ite32)(BASE, 0)` |
| atomics | `__atomic_store_n((uint32_t *)BASE, 0, 0);` |
| asm mnemonics not listed | `stnp`, `st1`, `stadd`/`stset`/`swp`/`cas*` (LSE atomics write), `dc zva` |
| asm encodings and spellings | `.inst 0xb9000020`, `"s" "tr ..."` string split, `__asm volatile(...)` |
| SMC (PMU writes go through EL3 on gs101, UNSURE) | `__asm__ volatile("smc #0");` |
| anything outside the file | store helpers or macros in `usbprobe.h` or another `.c`/`.S`. Only `usbprobe.c` is scanned |

Also, the self-test case "member store through a volatile struct pointer" (line 365) is caught for the wrong
reason. The message is `('*d =')`, so it matched the **declaration initializer** `volatile struct dwc3 *d =
...`, not `d->dctl = 1`. The same false positive rejects a purely read-only `volatile uint32_t *r = (void
*)0x11110000; x = *r;` (confirmed). `d->f =` detection does work when tested on its own.

Why High and not a blocker: today's source contains none of these spellings, and the binary audit below shows
zero non-stack stores. But the checker is the gate meant to keep stage 2 and later edits honest, and in its
current form it would pass most real write spellings. Suggested fix, in increasing order of strength:
(a) ban `asm`/`__asm`, `mem*`/`__builtin_mem*`/`__atomic`/`__sync`, and any `)` immediately followed by `[`;
(b) **replace the source scan with an object check on `out/usbprobe.o`**:
`nm -u` ⊆ {try_read32, con_str, fdt_collect_memmap, _head, _end}, `.data`/`.bss` empty, no `smc`/`hvc`/`dc`/
LSE or `st*` instruction whose base register is not provably sp-derived. A simple version that is still useful:
the only non-sp store bases allowed are registers whose last definition is `add xN, sp, #imm` or a
`put_*` helper's x0. That is what I checked by hand below.

### Medium

**M1. No pstore checkpoint between the UFS section and the USB probe** (`shim/shim.c:335-343`). Order now:
FDT → `pstore_save` (331) → UFS (`test_ufs_bluejay`, 335) → *(no save)* → 3 s delay → USB probe → `pstore_save`
(343). The watchdogs are disabled (`shim.c:234-235`), so if any USB, PHY or PMU read stalls the interconnect
rather than faulting, the phone hangs until the user force-reboots. The UFS section's output then never
reaches pstore. Fix: add `pstore_save(); /* checkpoint: UFS section */` right after
`test_ufs_bluejay(dtb);`. It is one line, has no other behaviour change, and makes the USB step's worst case
cost only its own lines.

**M2. A gated block may hang instead of faulting (hardware, UNSURE).** `try_read32` catches a synchronous data
abort (ESR EC 0x25, ELR+4) and an SError taken during the `daifclr #4` window (`shim.c:110-120, 433-449`). In
`shim.elf` I confirmed `try_read32` is a single `ldr w3, [x0]` at 0x1994 immediately followed by `dsb sy`, so
ELR+4 resumes correctly. But from memory, Exynos parts often **stall** on an access to an IP whose clock is
gated, unless an interconnect timeout turns it into an SError (UNSURE). There is no evidence in NOTES that the
guard has ever caught a real fault on gs101 (UNSURE: none found). Mitigating facts:
- DWC3 was serving fastboot moments before the jump.
- GSNPSID is read first and stops the whole probe if it faults.

The PHY bank (0x11100000) and the PMU words are read even when GSNPSID is readable. They are separate
power/clock domains (PMU always-on, UNSURE). Nothing to change in code beyond M1. Note it as the expected
failure mode when interpreting a hang.

### Low

**L1. On-screen labels collide** (`console.c:219-223`, labels in `usbprobe.c`). `con_str` draws the label at
column 0 and the value at `LABEL_COLS`=8, overwriting label characters from column 8 on. On screen,
`USB GEVNTADR`/`GEVNTSIZ`/`GEVNTCOUNT` all show as `USB GEVN…`, `USB GUSB2PHYCFG0`/`GUSB3PIPECTL0` both show
as `USB GUSB…`, and `USB PHY+00..30` all show as `USB PHY+…`. Also, about 44 columns fit (1080 px wide,
UNSURE), so long values like the GSNPSID family text, the EVBUF addr/end line and an all-unreadable PHY row are
clipped. ramlog/pstore/UART get the full `label + " " + value` (at most 72 characters, under the 95-character
cap), so this only matters if the pager photos are the only output. Cheap fix: put the distinguishing text in
the value, or use labels of 8 characters or fewer.

**L2. The EVBUF overlap check covers only the shim image** (`usbprobe.c:394-399`). If ABL left RUN_STOP=1, the
controller can keep DMA-writing events while the shim runs. The pstore/ramoops zone is outside `[_head,_end)`
and is not checked. It's worth a second flag ("OVERLAPS ramoops zone") because a live event buffer there
would corrupt exactly the log being pulled. (The ramlog is in .bss, so it is already covered.)

**L3. Host test gaps** (`usbprobe_host_test.c`). The test is good: the fake window counts stray reads, the
skip path proves exactly 2 lines and 0 extra reads, and the fault-suppressed decode is checked. Not covered:
- the `addr 0`, `range wraps` and `reserved table truncated` branches, and a buffer that *partly* overlaps a
  reserved range (covers vs overlaps);
- a per-address read count, which would catch a double read of a sensitive register;
- the fake `con_str` doesn't apply the 96-byte cap, so a line overflow wouldn't be seen (none exists today);
- by design, the host test cannot exercise the real fault guard.

**L4. NOTES handoff is stale after the rebase** (`NOTES.md:5-7, 9, 34`). It cites `ea4b591`/`144d14e` and says
the branch is based on `fd45dcf` without `cca4b25`. The actual commits are `5265e05`/`fd2a531`, on top of
`cca4b25`.

**L5. `try_read32` is no longer inside `#if BOARD_BLUEJAY` but is only guarded on bluejay** (`shim.c:101-120`).
On sweet it would be a raw read. The code comment says so, and nothing calls it on sweet (`usbprobe_report` is
only called from `bluejay_probe`). Informational only: sweet now links dead USB code.

## Evidence per item

**1. Writes and side effects.** Source: the only memory writes are to `line[96]`, `label[16]`, `tmp[10]`, the
`struct usb_reg` locals and `struct fdt_memmap mm` (stack, about 3 KiB; the stack is 0x8000 per `shim.ld`).
Objdump (`aarch64-none-elf-objdump -dr shim/out/usbprobe.o`, gate build):
- `.data`=0, `.bss`=0.
- Undefined symbols are exactly {con_str, fdt_collect_memmap, try_read32, _head, _end}.
- 120 store instructions. Bases: `sp` ×54; `x27` ×29, defined once as `add x27, sp, #0x120` (=`line`);
  `x19` ×3 (0x9c4/0xc04/0xc18, the PHY label loop), reached only after `add x19, sp, #0x180` at 0x820. The
  rodata definitions of x19 at 0x3e0/0xd10 are earlier, and the ep0 loop sits between them and the stores.
- `x4/x6/x7/x2` = `x27 + idx`.
- `x5@0x125c` loads `[sp,#104]`, which holds the spilled x27 (`str x27,[sp,#104]` at 0x2f0, incremented at
  0x1270).
- `x3@0x144c` = `mov x3, x27`; `x4,x23@0x1490` = `x27+w0`, index from `cset` 0.
- `put_dec`/`put_bit`/`put_reg` store only through their x0, and all 46 call sites pass `mov x0, x27`.
- No `smc`, `hvc`, `dc` or LSE atomics.

**Zero stores to non-stack addresses.**

Read side effects: GSNPSID, GCTL, DCFG, DCTL, DSTS, DALEPENA, GEVNTADR/SIZ/COUNT, GUSB2PHYCFG/GUSB3PIPECTL and
DEPCMD/PAR are plain registers. GEVNTCOUNT is decremented by a *write*, not by a read (Linux
`dwc3_process_event_buf` writes the count back), and DEPCMD only acts when written. This is from memory of
`drivers/usb/dwc3/core.h` and `gadget.c`: UNSURE but high confidence. The USB PHY bank's first 0x40 bytes
(LINKCTRL/CLKRST/UTMI-style control registers) have no known read-to-clear behaviour: UNSURE.

**2. Fault guard.** All 20 `bl try_read32` sites take x0 from a `mov/movk` constant (0x1111xxxx, 0x1746xxxx), or
from `x20`/`x23` loop counters seeded with `0x11110000+0xC800` and `0x11100000` and stepped by +0x10/+4. On a
fault, `usb_read` zeroes the value and each decoder is skipped (`if (x.ok)`). An unreadable GSNPSID returns
after 2 lines (`usbprobe.c:176-181`). The page tables map all of 0-64 GiB as Device-nGnRnE except the image and
framebuffer (`mmu.c:94-96`), so these addresses are Device: no speculation, no translation fault.

**3. Bounds.** `evaddr`/`evbytes` are used only in comparisons (`range_covers`/`range_overlaps`) and `put_hex`.
DEPCMDPAR0 (the TRB pointer) is only printed. The objdump shows that no load uses a base register derived from
a `try_read32` result. The only loads outside the stack are from rodata (hex table, strings) and from `mm`.
Wrap is checked (`usbprobe.c:351`).

**4. Output.** The longest real line is 72 characters (`USB GSNPSID value=… UNKNOWN family (expected dwc3
0x5533)`). `emit` caps at 95, and the `put_*` helpers stop at `pos+1 < cap` (objdump `cmp w?, #0x5f`). The PHY
label buffer is 16 bytes and the longest label is 10. The USB section adds about 27 lines (≤ about 2.6 KB) to a
32 KiB ring. pstore keeps the tail, so the USB lines, being newest, survive.

**5. Checker.** See H1. The bypass script was run against the real `usbprobe.c` with each snippet appended.

**6. Integration.** The USB block is inserted after `test_ufs_bluejay` and before both the PSTORE_FAST and the
pager endings (`shim.c:336-343`). The rest of the flow is unchanged: diff `cca4b25:shim/shim.c` 325-333 against
HEAD shows only the inserted block. There is no build-time switch to skip the probe. "Skipped" happens only at
runtime, on an unreadable GSNPSID, and then adds 2 lines and one `pstore_save`. Defect M1 above.

**7. Host test.** It runs in `build.sh` and the gate (`PASS usbprobe host test: …` in `shim/out/gate.log:4,55`).
Gaps are in L3.

## Commands run

- `tools/check-all.sh` → `GATE PASS 7a5d7ca`
- `aarch64-none-elf-objdump -dr shim/out/usbprobe.o`, plus `nm -u` and `size -A`
- `aarch64-none-elf-objdump -d shim/out/shim.elf` (try_read32 window)
- Python import of `tools/test-bluejay-boot.py`, then `check_usb_safety_text` on 22 bypass snippets and 1 false-positive probe
