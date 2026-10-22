# usbgadget review 1 (Opus 5.5, independent, read-only)

Target: worktree `<worktree>`, branch `t3/usb-gadget`.
The brief named HEAD `0a1c496`. The branch has since moved to `4343995` ("NOTES: step 3 wired,
Opus review pending"), which changes only `NOTES.md` (`git diff 0a1c496..HEAD --stat`: NOTES.md
+19). The code reviewed is therefore identical to 0a1c496. Diff reviewed: `128f781..HEAD -- shim tools`.

References:
- Linux `drivers/usb/dwc3/{core.c,core.h,gadget.c,gadget.h,ep0.c}` at torvalds/linux master
  `72a16bcff25e9f4a1d6d42a243301b1b76db2f32` (2026-10-21), fetched with `gh api`. Copies are in
  `/tmp/ugrev/linux/`. Linux line numbers below refer to that commit.
- `xnu/patches/0073-sweet-usb-device-ep0.patch` (the version that enumerated on sweet).

Rules kept: no adb or fastboot, no sudo, no repo file edited. Side effect: the requested
`./build.sh --board bluejay --avb --usb-gadget` rewrote the gitignored `shim/out/*` images, so
`shim/out/shim-boot-avb.img` is now the **--usb-gadget** build of 4343995. Scratch files are in
`/tmp/ugrev/`.

## Verdict table

| # | Requirement | Verdict | One line |
|---|---|---|---|
| 1 | Write set | **PASS** (guard too wide, H2) | 17 `bl w32` sites, all with constant offsets in {C400,C404,C408,C40C,C700,C704,C708,C720,C800..C83C}. No path writes GCTL, PHY config, OTG or PMU registers. CSFTRST matches what Linux does on every soft-connect. |
| 2 | DMA safety | **PASS** | Every DMA address is `pool + const`. Every length is bounded: ep0 ≤ 512 (only 7 is reachable), bulk ≤ 4096, events wrap at 0x1000. The worst-case MPS overrun still stays inside [pool, pool+0x2500). |
| 3 | Hangs | **PASS** | Every loop is counter-bounded (10 ms, 500 ms, 600 s). Tick math cannot overflow and the frequency is checked for zero. wait_ticks/wfe is proven on hardware by the pager. A sticky fault does not hang, but it does burn the 600 s window (L6). |
| 4 | Protocol / register correctness | **FAIL** | H1 (re-arming setup on bus reset), H4 (bulk TRB type), H5 (DEPSTARTCFG endpoint), M1 (SET_ADDRESS timing), M2 (no ENDTRANSFER), M3 (OUT TRB not MPS-aligned). The descriptors and request constants are correct. |
| 5 | Coherency / ordering | **PASS** | MMU off, so all accesses are Device-nGnRnE. The TRB stores complete before DEPCMD because of the `dsb sy` that follows the PAR0 write. This is incidental (L12). |
| 6 | Streaming | **PASS** | Cursor math is wrap-safe and eviction-safe. Buffers are bounded. Every ready 0→1 transition replays the log. Performance note L7. |
| 7 | The audit | **FAIL** | 5 of 6 bypass attempts pass the object audit. The w32 runtime check stops only writes outside 0xC100..0xCFFF, not GCTL or PHY writes inside it. |
| 8 | Gate + build output | **PASS** | `GATE PASS 4343995`. The build returned rc=0 and all audits passed (output below). |

**SAFE TO RUN ON THE PHONE: yes, for this exact code (4343995 = 0a1c496 + NOTES).** I found no
path that can damage the phone, write outside the DWC3 device, global-event or DEPCMD registers,
DMA outside the pool, or hang without a counter-bounded exit. **However, it will very probably
not enumerate**, because of H1, H4 and H5. Fix those (plus M1/M3) before spending a boot on
it. Before any *further* edit of usbgadget.c goes to the phone, H2 (an exact register allowlist
in w32) should land first, since today it is the only runtime barrier and it admits GCTL and the
PHY config registers.

---

## 1. Write set: PASS

Every MMIO store reachable from `usbgadget_init`, `usbgadget_poll` and `usbgadget_write` goes
through `w32()` (`shim/usbgadget.c:235-242`). That helper calls `try_write32(0x11110000 + off, v)`
only when `off` is in 0xC100..0xCFFF. The object agrees with the source: `objdump -d out/usbgadget.o`
shows exactly one `bl try_write32` (in w32, +0x20) and 17 `bl w32` call sites, each with a constant
offset in w0 (see `/tmp/ugrev/ug.dis`). `shim.c usb_gadget_phase` (`shim/shim.c:326-379`) does
**no** writes; it only calls `try_read32` on GEVNTADRLO/HI.

| Site | Offset | Value |
|---|---|---|
| init `usbgadget.c:613` | DCTL 0xC704 | `0x40000000` (CSFTRST only; RUN_STOP and everything else 0) |
| init `:630-632` | DCFG 0xC700 | `DCFG & ~0x3FF` (DEVSPD=0 HS, DEVADDR=0) |
| init `:636-638` | GEVNTADRLO/HI 0xC400/0xC404, GEVNTSIZ 0xC408 | `pool`, `pool>>32`, `0x80001000` |
| init `:640` | GEVNTCOUNT 0xC40C | `GEVNTCOUNT & 0xFFFF` (ack stale) |
| init `:644` | DEVTEN 0xC708 | `0x7` |
| epcmd `:269-271` (via init/arm_setup/stall/enable_ep1/conndone) | DEPCMDPAR0/1/2 + DEPCMD of phys ep 0, 1, 3 → 0xC800..0xC80C, 0xC810..0xC81C, 0xC830..0xC83C | params / `cmd \| CMDACT` |
| init `:663` | DALEPENA 0xC720 | `0x3` |
| init `:665` | DCTL 0xC704 | `DCTL \| RUN_STOP` |
| enable_ep1 `:370` | DALEPENA 0xC720 | `0xB` |
| drop_enumeration `:458` | DCFG | `DCFG & ~DEVADDR` |
| EP0_STATUS `:543-546` | DCFG | `DCFG & ~DEVADDR \| addr<<3` |
| poll `:705` | GEVNTCOUNT | `consumed*4` (≤ 64) |

- No site writes GCTL (0xC110), GUSB2PHYCFG (0xC200), GUSB3PIPECTL (0xC2C0), GTXFIFOSIZ, the OTG
  registers (0xCC00+), the PHY bank 0x11100000 or the PMU 0x17463EB0. **However**, the w32 window
  admits all of the 0xC1xx-0xCFFF registers (demonstrated in §7, finding H2).
- GEVNTADR is always `pool_pa + 0` (`:636-637`). `pool_pa` comes only from `usbpool_choose`
  (`shim.c:352-357`), which requires it to lie inside one reserved range and clear of the
  image/FDT/initrd/pstore ranges.
- Compiler note: GCC's IPA bit-CP removed the `(off & 3)` test from w32 (`w32+0x4..0x14` shows
  only the `off-0xC100 > 0xEFF` range check). That is legal because every caller passes an aligned
  constant. It does mean the runtime check is "as good as the call sites", not an independent
  guarantee.

**CSFTRST.** `dwc3_core_soft_reset` (Linux `core.c:322-372`) says "We're resetting only the device
side". Linux issues the same `DCTL |= CSFTRST, &= ~RUN_STOP` on **every** gadget soft-connect
(`gadget.c:2811-2817`: soft_reset → event_buffers_setup → `__dwc3_gadget_start` → run_stop), and
on gs101 that is the stock path. When ABL has left the controller halted (RUN_STOP=0, DEVCTRLHLT=1),
a soft reset is exactly what Linux does before reprogramming, and it also drops ABL's address 13
and its SS speed. I don't expect it to touch the PHYs: GUSB2PHYCFG/PIPECTL are not reset by it,
according to Linux's ordering. That is UNSURE without the databook.

If CSFTRST never clears, the loop exits after 500 ms (`:615-627`), prints "soft reset stuck" and
returns -1 with no further writes. The core stays in reset, nothing DMAs, and the next ABL boot
re-initialises it. Linux allows 10×20 ms on DWC_usb31 ≥1.90a, so 500 ms is ample. Linux also waits
50 ms *after* the clear on DWC_usb31 ≤1.80a (`core.c:363-367`). The shim does not wait (L2), and
the exact version on this phone is unknown.

## 2. DMA safety: PASS

The pool layout (`usbgadget.h:45-53`) has no overlaps. Every region ends below 0x2500, and the
pool is 0x8000 bytes.

| Region | Offset range | Who DMAs | Bound |
|---|---|---|---|
| Event ring | 0x0000-0x0FFF | core writes | GEVNTSIZ = 0x1000 (`:638`). The core wraps at SIZE. `evt_pos` is masked with `0xFFF` (`:700`), reads are 4 B aligned, 16 per poll (`:695`). |
| ep0 OUT TRB | 0x1000-0x100F | core reads/writes | 1 TRB, LST set |
| ep0 IN TRB | 0x1010-0x101F | core reads/writes | 1 TRB, LST set |
| Setup buffer | 0x1040-0x1047 | core writes | TRB len 8 (`:339`). Even an MPS-sized (512) overrun ends at 0x1240, inside the pool. |
| ep0 data/status | 0x1100-0x12FF | core both ways | IN len ≤ min(desc, wLength) ≤ 67, capped at 512 (`:404`). OUT only for SET_LINE_CODING, whose wLength usbproto forces to **7** (`usbproto.c:436`), and usbgadget also caps it at 512 (`:414`). Status uses len 0. Worst case: if the core wrote a full MPS-512 packet, the write ends at 0x1300, which is < 0x1400. A host cannot choose a length larger than 7 because any other wLength stalls. |
| Bulk TRB ring | 0x1400-0x143F | core reads/writes | index masked with `& 3` (`:724`); every TRB is LST (no LINK TRB, no walk) |
| Bulk IN data | 0x1500-0x24FF | core reads | len ≤ 4096 (`:717`), `_Static_assert` at `:198` |

- `usbpool_choose` (`usbpool.c:8-50`): `end = pool + len; end <= pool` rejects wrap and empty
  ranges. A reserved range is used only if `re > rb` (no wrap) and `rb <= pool && end <= re`.
  Overlap is the half-open `b1<e2 && b2<e1`. A truncated table fails closed. GEVNTADR==0 fails
  closed. The math is correct.
- Avoid list (`shim.c:341-351`): image `[_head,_end)` (`_end` includes .bss, the 32 KiB stack and
  the secondary stacks, per `shim.ld`); FDT `[dtb, dtb+totalsize)`; initrd; pstore zone (size 0 if
  unarmed, so correctly "absent"). Gaps: L1 (initrd fail-open when only one property exists), L10
  (framebuffer not listed; it is at 0xFAC00000, far from 0xF8B92000), M4 (any reserved range is
  accepted, not just `abl`). UFS alternative DMA windows are chosen *outside* every reserved range
  (`ufs.c find_dma_spot`), so they cannot overlap a pool that is inside one.
- Reusing ABL's buffers: after CSFTRST, ABL's TRB/transfer state in the core is gone, and ABL no
  longer runs, so nothing else reads or writes 0xF8B92000-0xF8B93FFF. 0xF8B94000-0xF8B99FFF is
  unknown ABL memory (heap or data). The shim never returns into ABL, and a reset reloads ABL.
  UNSURE but low risk: whether anything *other* than ABL (EL3, LDFW, another master) reads the
  `abl` carveout after handoff. Also UNSURE: whether the gs101 S2MPU allows USB DMA to the full
  32 KiB rather than only ABL's two pages. Failure would block the DMA (no events or transfers),
  not corrupt memory.

## 3. Hangs: PASS

| Loop | Bound | Evidence |
|---|---|---|
| CSFTRST poll | 500 ms | `usbgadget.c:615-623` |
| DEPCMD CMDACT poll | 10 ms, then "epcmd timeout" and sticky fault | `:273-289`. If CMDACT never clears, the gadget dies and no further commands are issued. |
| DSTS halt observation | 500 ms (observation only) | `:670-678` |
| Event loop | ≤ 16 events per poll, each ≤ 4 epcmds × 10 ms | `:695` |
| ep0/bulk copies | ≤ 512 / ≤ 4096 | `:404-407`, `:717-720` |
| Window loop | 600 s, signed `(int64_t)(end-now())` compare | `shim.c:373-378` |

- `deadline_ms`: `ticks_freq * ms / 1000`. `ticks_freq` = `counter_freq()` = `TIMER_FREQ` =
  0x1770000 (24.576 MHz, `out/board_config.h`). The maximum product, 24.576e6×500 = 1.2e10, is far
  below 2^64. Zero is rejected before any register write (`:598-601`). The comparison is absolute
  `>=` on 64-bit cntpct, which has no practical wrap.
- `wait_ticks(freq/1000)` (`console.c:37-55`) sets CNTKCTL_EL1.EVNTEN with EVNTI=13, so wfe wakes
  at least every 2^14 ticks ≈ 0.67 ms. Each iteration therefore waits 1-1.67 ms. The same
  primitive drove the 10-minute pager on this phone at Gate 0 (observed), so it does not stall.
- Sticky fault: `usbgadget_poll` returns at `:690`, `usbg_pump` returns because `usbgadget_ready()`
  is 0, and the loop just idles until 600 s. There is no hang, but there is a wasted window (L6).
- con_str inside event handlers can block for 3 s (`screen_room`, `console.c:61-66`) when the screen
  is full, plus 1 ms per char in a dead-UART check. It is bounded.
- Not coverable by software: a write that stalls the bus forever (gated domain without SError).
  The probe read these registers at Gate 0 without a stall, and ABL left the domain on. The
  watchdog is disabled by the shim, so the recovery is Power+VolDown, which works. UNSURE but
  low risk.

## 4. Protocol / register correctness vs Linux: FAIL

**Correct** (checked against Linux core.h/gadget.h at 72a16bc):
- Register offsets: GEVNTADR/SIZ/COUNT 0xC400..C40C, DCFG/DCTL/DEVTEN/DSTS/DALEPENA,
  DEPCMDPAR2/1/0/DEPCMD at +0/+4/+8/+C (core.h:150, 169-172).
- DCTL RUN_STOP bit31, CSFTRST bit30 (core.h:451-452). DSTS.DEVCTRLHLT bit22 (core.h:522).
- DCFG DEVADDR `<<3` mask 0x7f (core.h:435-436). HIGHSPEED = 0 (core.h:441).
- DEVTEN 0x7 = DISCONN|USBRST|CONNECTDONE (core.h:505-507). GEVNTSIZ INTMASK bit31, size in bytes
  (core.h:352-353).
- DEPCMD codes 1/2/4/6/9, CMDACT bit10, status bits 15:12, PARAM `<<16` (core.h:570-590).
- TRB bits HWO0/LST1/ISP_IMI10/IOC11, TRBCTL `<<4`: SETUP 2, STATUS2 3, STATUS3 4, CONTROL_DATA 5
  (core.h:863-878).
- Event decoding: ep bits 5:1, type 9:6, status 15:12. Device event = bit0, type 11:8.
  XferComplete 1, XferNotReady 3. Control status phase = 2 (core.h:1466-1468, 1518-1519). Linux
  also compares `event->status` unmasked (`ep0.c:1153`).
- DEPCFG: EP_TYPE `<<1`, MPS `<<3`, FIFO_NUMBER `<<17`, EP_NUMBER `<<25` (5 bits = physical
  number), MODIFY `2<<30`, XferComplete bit8, XferNotReady bit10 (gadget.h:23-46).
- Bulk ep1 IN = phys 3, FIFO 1, type 2, MPS 512 (gadget.c:604-639: "USB endpoint 0x81 is 0x03";
  `FIFO_NUMBER(dep->number >> 1)`). DEPXFERCFG param0 = 1 (gadget.h:49). STARTTRANSFER
  param0 = TRB hi, param1 = TRB lo. The conndone MODIFY to MPS 64 matches Linux.
- Init order matches Linux soft_connect: reset → event buffer → DEPSTARTCFG(0) on ep0 → ep0/ep1
  DEPCFG INIT + XFERCFG → setup TRB → DEVTEN → RUN_STOP.
- USB level: device, qualifier, config (wTotalLength 67 = 9+9+5+5+4+5+7+9+7+7) and string
  descriptor lengths are correct. bRequest: GET_STATUS 0, SET_ADDRESS 5, GET_DESCRIPTOR 6,
  GET/SET_CONFIGURATION 8/9, GET/SET_INTERFACE 0x0A/0x0B (USB 2.0 Table 9-4). CDC
  SET/GET_LINE_CODING 0x20/0x21, SET_CONTROL_LINE_STATE 0x22, bmRequestType 0x21/0xA1, DTR bit0,
  RTS bit1. HS bulk MPS 512. Interrupt bInterval 16 is valid at HS.

**UNSURE tags resolved:**
1. DEPCFG EP_NUMBER field: Linux uses bits **29:25** (gadget.h:31, gadget.c:632). The code is
   right. NOTES.md "STEP 2" item 1 ("Linux dwc3 puts EP_NUMBER at bits 20:16") is wrong and
   should be corrected.
2. Physical index 3 for ep1 IN: confirmed (gadget.c:627-631 comment). DALEPENA bit = physical
   number.
3. Device event 3: DWC3_DEVICE_EVENT_LINK_STATUS_CHANGE (core.h:65), enabled by DEVTEN bit3
   ULSTCNGEN (core.h:504). The code is right.
4. DEPSTARTCFG param 2 at SET_CONFIGURATION: still current Linux (`ep0.c:663`), but issued on
   **eps[0]** (`gadget.c:568`). The code issues it on ep 3: **wrong**, see H5.

**Wrong or suspicious:** H1, H4, H5, M1, M2, M3, L3, L4, L5, L8, L9 (details below).

## 5. Coherency / ordering: PASS

- bluejay runs `bluejay_probe` before any `mmu_enable` (`shim.c:851-853`, noreturn), and start.S
  writes SCTLR_EL1 = 0x30d00800 (M=0, C=0). All data accesses are therefore Device-nGnRnE and
  non-cacheable, so no cache maintenance is needed.
- TRB before STARTTRANSFER: `trb_write` does 4 volatile stores, with HWO in the last one
  (`:298-302`). The subsequent `epcmd` writes PAR0, then `try_write32` executes `dsb sy`
  (`shim.c:135-136`), which completes the TRB stores before PAR1, PAR2 or DEPCMD are issued. This
  is correct but incidental (L12).
- Event buffer reads happen after the GEVNTCOUNT `try_read32`, whose `dsb sy` follows the load. All
  accesses are volatile (`:696-698`), as are the setup, data and bulk copies (`:380-389`, `:403`,
  `:526`, `:710`). `-mstrict-align` is in CFLAGS and all multi-byte pool accesses are 4-byte
  aligned.
- UNSURE: gs101's system-level cache is memory-side, so it should be coherent between
  non-cacheable CPU accesses and DWC3 DMA. This is not verified.

## 6. Streaming: PASS

`usbg_pump` (`shim.c:282-310`):
- `first = total - ramlog_lines()` is exact, because every `ramlog_line` appends exactly one line
  and eviction drops whole lines (`ramlog.c:111-139`).
- `cursor` is absolute. `(int32_t)(cursor-first) < 0` handles eviction. `(int32_t)(total-cursor) > 0`
  terminates. A 32-bit wrap would need 2^31 lines.
- No con_str (and so no append) happens between computing `first` and the copies.
- Buffer: each line is ≤ 95 chars + CRLF = 97 bytes, ≤ 8 lines per refill, and the
  `len + l + 2 > 4096` check is redundant but safe.
- `usbgadget_write` is all-or-nothing (returns len or 0, `usbgadget.c:713-725`), so `off` only ever
  becomes `len`. Partial acceptance therefore cannot happen, and the `off < len` retry path is
  correct.
- Replay: any ready 0→1 transition (SET_CONFIGURATION + DTR, including DTR re-raise on reopen)
  resets `cursor = first` and drops the half-sent buffer. Every new session replays the retained
  log, which matches the brief.
- Closing the tty while a bulk TRB is pending leaves `bulk_busy=1` until the host reads again.
  The stale chunk is then delivered first. Not harmful.
- Performance: L7.

## 7. The audit: FAIL

The harness is `/tmp/ugrev/byp/run.sh`. Each case copies shim/ into `/tmp/ugrev/byp/cN`, patches
usbgadget.c, compiles it with the exact shim CFLAGS and toolchain, and calls
`check_usbgadget_object()` from `tools/test-bluejay-boot.py`. The runtime check was exercised
separately in `/tmp/ugrev/byp/rt.c`, which includes usbgadget.c and stubs `try_write32` to log.

| # | Attempt | ABS64 relocs (build.sh check) | Object audit | w32 runtime |
|---|---|---|---|---|
| 0 | unmodified control | 0 | passes (correct) | — |
| 1 | `*(volatile uint32_t *)(0x11110000+0xC110) = 0x2000;` in init (GCTL) | 0 | **PASSED (not caught)** | not involved |
| 2 | inline `asm("str %w0,[%1]")` to 0x1111C200 (GUSB2PHYCFG) | 0 | **PASSED (not caught)** | not involved |
| 3 | function pointer smuggled via the hooks struct (`hooks->wr(0x1111C2C0,0)`, shim.c would set `wr = try_write32`) | 0 | **PASSED (not caught)** | not involved |
| 4a | `w32(USBG_REG_LO + volatile 0x10, 0x1000)` = GCTL through the sanctioned helper | 0 | **PASSED (not caught)** | **allowed**: `MMIO WRITE 0x1111c110 = 0x1000 rc=0` |
| 4b | `w32(USBG_REG_LO - volatile 0x10000)` (computed, outside the window) | 0 | **PASSED (not caught)** | caught: `USB gadget fault rc=-1` |
| 5 | second helper `w32b` calling through `static int (*const wp)() = try_write32` | 0 | caught (`R_AARCH64_JUMP26 inside 'w32b'`); GCC folded it to a tail call | — |

`rt.c` output: w32 accepted 0xC110 (GCTL), 0xC200 (GUSB2PHYCFG), 0xC2C0 (GUSB3PIPECTL) and 0xCC00
(OTG), and rejected 0xFFFFC100 and 0xD000.

Conclusion: the writer audit (`tools/test-bluejay-boot.py:1189-1224`) checks only (a) the undefined
symbol set, (b) relocations against the *symbol name* `try_write32`, and (c) a banned-mnemonic list
(smc/msr/dc/atomics). It does not classify stores at all. Unlike the usbprobe and usbproto audits,
there is no store-base proof, so any direct or asm store, and any pointer that reaches try_write32
without naming it, passes. It also does not verify that w32 contains a range check, or what range
that check uses. See H2 and H3.

## 8. Real output: PASS

```
$ cd <worktree> && tools/check-all.sh
GATE PASS 4343995
real 0m14.668s   (rc=0; full log shim/out/gate.log)
```

```
$ cd shim && ./build.sh --board bluejay --avb --usb-gadget 2>&1 | tail -30
self-test caught: object store in an unreachable block -> ...
self-test caught: object unclassifiable instruction (.inst) -> ...
self-test caught: object undefined symbol beyond the surface -> ... memcpy
self-test caught: object non-empty .data -> ...
self-test caught: object non-empty .bss -> ...
self-test caught: object put_ helper that is not static -> ...
PASS usb object self-test: 20/20 bad objects rejected
PASS usb object audit: 119 stores, all stack-based; undef/.data/.bss/instruction bans clean
self-test caught: usbproto undefined symbol (any) -> ... memcpy
self-test caught: usbproto store through a movz/movk absolute pointer -> ...
self-test caught: usbproto smc -> ...
self-test caught: usbproto store through an unclassified register -> ...
PASS usbproto object self-test: 4/4 bad objects rejected
PASS usbproto object audit: 38 stores, all stack/arg/adrp-image based; undef empty; bans clean
PASS usbpool object audit: 3 stores, all stack/arg/adrp-image based; undef empty; bans clean (same no-MMIO audit as usbproto.o)
self-test caught: usbgadget writer bl try_write32 outside w32 -> ...
self-test caught: usbgadget writer address of try_write32 taken (adrp) -> ...
self-test caught: usbgadget writer msr system register -> ...
self-test caught: usbgadget writer LSE atomic stadd -> ...
self-test caught: usbgadget writer undefined symbol wait_ticks ... -> ...
self-test caught: usbgadget writer undefined symbol memcpy -> ...
PASS usbgadget writer self-test: 6/6 bad objects rejected
PASS usbgadget writer audit: try_write32 referenced only by a bl inside w32; undef subset; no banned instructions
PASS header v4 (lz4): kernel=42760 ramdisk=97; host tests & safety checks passed
-rwxrwxr-x 1 user user   315392 Oct 21 05:35 out/Image
-rw-rw-r-- 1 user user 67108864 Oct 21 05:35 out/shim-boot-avb.img
-rw-rw-r-- 1 user user   323584 Oct 21 05:35 out/shim-boot.img
-rw-rw-r-- 1 user user 67108864 Oct 21 05:35 out/shim-boot-lz4-avb.img
-rw-rw-r-- 1 user user    53248 Oct 21 05:35 out/shim-boot-lz4.img
rc=0
```
(Long self-test messages are elided with "..."; the full text is in `/tmp/ugrev/build.out`.) The
linked ELF has `w32 t`, `try_write32 T`, `usbgadget_init T`, `_end = 0x4d000`.

---

## Findings

### Critical
None. No path that can damage the phone, write outside the sanctioned registers, DMA outside the
pool, or hang without a bound was found.

### High

**H1. Bus reset re-arms the setup TRB while it is still active, so the gadget faults on the first reset.**
`shim/usbgadget.c:453-468` (`drop_enumeration(1)`), called from `:473-477`.
Scenario: init arms the setup TRB on ep0 OUT (`:661`). The host attaches and, as every host does,
issues a bus reset, so the first event is RESET. The code then calls `arm_setup()`, which issues
STARTTRANSFER on ep0 OUT while that endpoint's setup transfer is still active. Linux does **not**
restart ep0 on reset when it is in the setup phase (`gadget.c:142-157` `dwc3_ep0_reset_state`:
only `if (ep0state != EP0_SETUP_PHASE)`, and then it first does END_TRANSFER plus stall-and-restart),
citing "SNPS databook 8.1.2, the EP0 should be in setup phase". The 0073 patch's reset handler only
clears DCFG.DEVADDR and never re-arms. The expected core response is a DEPCMD status ≠ 0 ("No
Resource", `DEPEVT_TRANSFER_NO_RESOURCE` in Linux `dwc3_send_gadget_ep_cmd`). That leads to
"USB epcmd status != 0" and then "USB gadget fault", so enumeration never starts. The exact HW
status is UNSURE; the divergence from both references is certain.
The host test encodes the bug: `usbgadget_host_test.c:444` ("bus reset: ... setup TRB re-armed").
The fake does not model an active transfer.
Fix: on RESET, clear only DEVADDR, usbproto, ep1_on, bulk_busy and pending_addr. Re-arm setup only
if `ep0_state != EP0_SETUP`, and then first ENDTRANSFER the active ep0 transfer (DEPCMD 8 |
ForceRM bit11 | `rsc_idx<<16`; Linux `dwc3_ep0_end_control_data`) followed by SETSTALL plus
arm_setup. The minimal 0073-equivalent fix is: do not re-arm on reset at all.

**H2. The w32 runtime whitelist is the whole 0xC100..0xCFFF block, not the spec's register set.**
`shim/usbgadget.c:99-100`, `:235-242`.
The task (`docs/tasks/usb-stage2.md` req. 3) allows only the device-mode registers, GEVNT*0 and
DEPCMD/PAR, and says "never GCTL.PRTCAP, never GUSB2PHYCFG/GUSB3PIPECTL". The window also contains
GCTL 0xC110, GUSB2PHYCFG 0xC200, GUSB3PIPECTL 0xC2C0, GTXFIFOSIZ/GRXFIFOSIZ 0xC300/0xC380, GUCTL,
and the OTG block 0xCC00+. §7 case 4a shows `w32(0xC110, …)` reaching the bus. No current call site
does this (§1 table), so this is not a live bug. It is, however, the **only** runtime barrier, and
the next edit to this file could send PHY writes to the phone unnoticed (see H3).
Fix: replace the range check with an exact switch: `0xC400, 0xC404, 0xC408, 0xC40C, 0xC700, 0xC704,
0xC708, 0xC720`, plus `0xC800 <= off < 0xC840` (phys eps 0-3). Add host-test asserts that 0xC110,
0xC200 and 0xC2C0 are refused.

**H3. The writer object audit cannot see stores. Direct stores, asm stores and hidden function pointers all pass.**
`tools/test-bluejay-boot.py:1189-1224`.
§7 cases 1, 2, 3 and 4a all pass the audit. The audit proves "the symbol try_write32 is named only
by a bl in w32". It does not prove "usbgadget.o stores only to the pool and the stack" or "w32
checks the right range". The comment at `usbgadget.c:83-87` ("keep this file free of inline asm")
is not enforced.
Fix:
- Reuse the usbprobe/usbproto store classifier for usbgadget.o. Allow stores only via the
  stack/x0-x7/adrp-own-.bss (`g`) bases, plus the pool pointer loaded from `g.pool`. Alternatively,
  route every pool store through one more noinline helper `pool_w32(off, v)` with an `off < 0x8000`
  check, and require all non-stack stores to sit in it.
- Add a source-text scan for `asm`/`__asm__` and for `0x1111`/`USBG_DWC3_BASE` outside w32/r32.
- Make the audit assert that w32's disassembly contains the compare against the allowlist.

**H4. The bulk IN TRB uses TRBCTL CONTROL_DATA (5) instead of NORMAL (1).**
`shim/usbgadget.c:324-325` (`start_bulk_trb` passes `USBG_TRBCTL_DATA`).
Linux uses `DWC3_TRBCTL_NORMAL` (core.h:874) for bulk and interrupt (`gadget.c
__dwc3_prepare_one_trb`: `case USB_ENDPOINT_XFER_BULK: … trb->ctrl = DWC3_TRBCTL_NORMAL`).
CONTROL_DATA is defined only for control endpoints. The expected result on HW is a TRB error, or no
data, on the first log write: enumeration succeeds but `/dev/ttyACM0` stays silent. The exact
symptom is UNSURE.
Fix: `#define USBG_TRBCTL_NORMAL (1u << 4)` and use it in `start_bulk_trb`. Add a host-test assert on
TRB[3] bits 9:4 == 1.

**H5. DEPSTARTCFG(XferRscIdx=2) is issued on DEPCMD(3) instead of DEPCMD(0).**
`shim/usbgadget.c:363`.
Linux sends DEPSTARTCFG only through `dwc->eps[0]` (`gadget.c:553-569`), for both index 0 and index
2. The code's comment says "Old Linux dwc3", but this is current behaviour (`ep0.c:663`). Issuing it
on phys ep 3 is outside the documented use. The HW reaction is UNSURE: a command error would trigger
the sticky fault at SET_CONFIGURATION, and silent acceptance would mean no resource reset.
Fix: `epcmd(USBG_EP0OUT, USBG_DEPCMD_DEPSTARTCFG | (2u << 16), 0, 0, 0)`.

### Medium

**M1. SET_ADDRESS is applied after the status stage. Linux and 0073 write DCFG immediately.**
`usbgadget.c:393-396`, `:539-547`.
Linux `dwc3_ep0_set_address` writes DCFG.DEVADDR in the setup phase (`ep0.c:624-627`). The 0073
patch does the same, with the comment "takes effect now (DWC3 handles the timing)". The core applies
the new address after the status stage by itself. The code instead waits for the status
XferComplete and then the next poll (1-1.7 ms, longer if con_str hits `screen_room`). That races
the host's SET_ADDRESS recovery interval (≥ 2 ms) for no benefit. The usbproto comment citing USB 9.4.5
describes the *bus* behaviour, which the DWC3 already implements.
Fix: write DCFG in `ep0_handle_setup` on `USBPROTO_OK_SET_ADDRESS` and drop `pending_addr`.

**M2. No END_TRANSFER for ep1 (or for non-setup ep0 states) on reset or disconnect, so re-enumeration faults.**
`usbgadget.c:453-469`.
Linux ends all active transfers on reset (`gadget.c:4236-4242`, "Databook 3.30a Table 4-2: …
DEPENDXFER for any active transfers"). Scenario: unplug while a bulk TRB is pending. `bulk_busy` is
cleared but the core's transfer stays active. After re-attach, SET_CONFIGURATION runs DEPCFG INIT,
and the next `usbgadget_write` STARTTRANSFER then hits an active resource and faults.
Fix: track the resource index returned in DEPCMD bits 22:16 after STARTTRANSFER. On reset or
disconnect, issue ENDTRANSFER (cmd 8 | bit11 ForceRM | `idx<<16`) on ep 3 when `bulk_busy`, and on
ep0 when `ep0_state != EP0_SETUP`.

**M3. The ep0 OUT data TRB length (7) is not MPS-aligned.**
`usbgadget.c:418`.
Linux bounces every non-MPS-aligned ep0 OUT request to a multiple of maxpacket (`ep0.c:1009-1036`).
The 0073 patch used a 64-byte TRB. SET_LINE_CODING is on the critical path: cdc_acm sends it when
the tty is opened. DMA is safe either way (§2), but the controller's behaviour with a 7-byte OUT TRB
is outside the programming model.
Fix: start the OUT TRB with len 64 (UD_DATA has 512 B), and copy 7 bytes on completion.

**M4. Pool acceptance trusts any reserved range, and init does not check that the core is in device mode.**
`usbpool.c:60-67`, `usbgadget.c:612`.
If GEVNTADR ever holds a non-ABL value inside a different reserved node (for example a stale Linux
CMA pointer after a warm reset into fastboot, or a live firmware carveout), the pool would be
accepted. `fdt_memmap.reserved[i].name` is already available.
Fix:
- Require the containing range to be named `abl` (fail closed otherwise).
- In `usbgadget_init`, read GCTL (read-only) and require PRTCAP (bits 13:12) == 2 before the
  CSFTRST. Linux skips the soft reset in host role (`core.c:332-333`).

**M5. The controller is left running after the 600 s window, and after the late init fault path.**
`shim.c:373-379`, `usbgadget.c:665-678` (an `r32(DSTS)` fault at `:672` returns -1 with RUN_STOP
already set, so shim.c prints "init failed" while the core is live).
During the following pager (~10 min) the host sees a device that stops answering. The core keeps
posting events into the ring and setup data into UD_SETUP, all still inside the pool. That is safe
memory-wise, but untidy.
Fix: add `usbgadget_stop()` that clears RUN_STOP (masking ULSTCHNGREQ) and polls DEVCTRLHLT ≤ 500
ms (Linux `dwc3_gadget_run_stop(false)`). Call it at window end and in init's post-RUN_STOP error
path.

### Low

- **L1** `shim.c:340-341,348-350`: if only one of `linux,initrd-start/end` exists, the initrd avoid
  becomes "absent" (fail-open). Fail closed when exactly one is found.
- **L2** `usbgadget.c:624-629`: there is no 50 ms delay after CSFTRST clears. Linux needs it for
  DWC_usb31 ≤1.80a (`core.c:363-367`), and the version is unknown (read DWC3_VER_NUMBER 0xC1A0 in
  the probe). Add an unconditional `≥50 ms` wait; it is cheap.
- **L3** `usbgadget.c:665`: the RUN_STOP read-modify-write does not mask DCTL.ULSTCHNGREQ (bits
  8:5). Linux `dwc3_gadget_dctl_write_safe` always does.
- **L4** `usbproto.c:316-350` with `usbgadget.c:402-411`: GET_DESCRIPTOR/GET_STATUS with
  wLength=0 returns DATA_IN with len 0, and a 0-byte DATA TRB is started for what is a 2-stage
  request. The ep0 state machine then waits in EP0_DATA and ignores the status XferNotReady. Treat
  `len == 0` as `OK_NO_DATA`. Only a malformed host triggers this.
- **L5** `usbgadget.c:570`: device events are identified by bit0 only. Linux also requires event
  type bits 7:1 == 0 (DWC3_EVENT_TYPE_DEV).
- **L6** `shim.c:373-378`: a faulted or never-configured gadget still holds the user for the full
  600 s before the pager. Break out of the loop when `usbgadget` reports a fault.
- **L7** `shim.c:288` and `:302`: every loop iteration while ready runs `ramlog_lines()` plus up to
  8 `ramlog_copy_line()` calls. Each is a full scan of the 32 KiB ring through *uncached* Device
  accesses, about 300K loads per iteration, i.e. several to tens of ms. It is not a hang, but it
  slows ep0 servicing and replay. Cache the line count, or keep a byte cursor.
- **L8** `usbproto.c:208-238`: interrupt IN 0x83 and bulk OUT 0x01 are advertised but never enabled
  in DALEPENA. cdc_acm submits the interrupt URB at open, so expect host-side transaction errors in
  dmesg. The HW reaction to tokens on a disabled endpoint is UNSURE.
- **L9** `usbproto.c:150-160,330-332`: the device qualifier advertises other-speed operation while
  OTHER_SPEED_CONFIGURATION stalls (USB 2.0 §9.6.2 inconsistency). Linux hosts don't care; some
  Windows versions might.
- **L10** `shim.c:341-351`: the framebuffer `[FB_BASE, +FB_SIZE)` is not in the avoid list. It does
  not overlap today (0xFAC00000 vs 0xF8B92000), but add it for robustness.
- **L11** `usbgadget_host_test.c`: the fake model accepts STARTTRANSFER on an active endpoint, which
  hides H1 and M2. Model "No Resource" (status 1) for a second STARTTRANSFER without a completion or
  END_TRANSFER.
- **L12** `usbgadget.c:264-272`: the TRB-before-DEPCMD ordering relies on the `dsb sy` inside the
  PAR0 `try_write32`. Add an explicit barrier after `trb_write`. `dsb` is not in
  `_USB_BANNED_INSN` (`test-bluejay-boot.py:490-494`), but the file is meant to stay asm-free, so
  the cleanest option is an extern `dma_wmb()` in shim.c added to the allowed undef set.
- **L13** UNSURE: whether the S2MPU allows USB DMA to the full 32 KiB pool (ABL itself only used
  0xF8B92000-0xF8B93FFF). If the first boot shows conndone but no SETUP handling, consider this.
- **L14** `NOTES.md` STEP 2, UNSURE item 1, misstates Linux's EP_NUMBER field (it is bits 29:25, not
  20:16). The code is right; fix the note so no later agent "fixes" the code.

## What the first hardware boot should show (if H1/H4/H5 are fixed)
`USB gadget pool 0xF8B92000`, `USB gadget: running`, then on cable attach `USB EVT reset` and
`USB EVT conndone speed=0` (HS). After that comes `/dev/ttyACM*` on the PC.

With the code unfixed, expect `USB EVT reset` followed by `USB epcmd status != 0` and
`USB gadget fault` (H1). That is the fail-closed path, harmless to the phone.

SAFE TO RUN ON THE PHONE: yes (for 4343995 as-is: no damage path found; expected outcome is a clean
"gadget fault" at the first bus reset). Any further usbgadget.c change must land H2 before it is
booted, and H1/H4/H5 are needed before a run can enumerate and stream.
