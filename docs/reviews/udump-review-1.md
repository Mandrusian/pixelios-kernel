# udump review 1 (Spec 01, branch t3/udump @ 63c0979)

STATUS: COMPLETE

Two independent passes by Sonnet 5.5 (read-only; no adb/fastboot, no sudo, nothing edited except this file) are merged here. Pass A = everything
from this header down to "Not verified" (reviewed 63c0979). Pass B = the sections from "Pass B" to the end (second reviewer, finished the
file; same diff, re-verified at HEAD 00a5955, which already contains the fixes for pass A's M2 and M4). Where the two passes agree, the finding is
listed once. Pass B added H1-confirmation, H2 and the points listed under "Added by pass B".
Scratch trees (removed at the end): /tmp/udump-base (6ea2970), /tmp/udump-new (63c0979), /tmp/bypass, /tmp/rv (harnesses).
Diff reviewed: `git diff 6ea2970..HEAD` (12 files). I read udump.c/.h, the ufs.c/shim.c/build.sh hunks, usbgadget_write, usbg_pump,
udump-recv.py, check-udump.sh, the object/source audits in test-bluejay-boot.py, and the host test.

**Pass A conclusion (superseded by the final line at the end of this file): conditional on (a) `shim/ufs.c`, `ufsinit.c`, `ufsdiag.c` being exactly the 63c0979 versions (I verified the
only CDB opcodes are 0x28 READ(10), 0x25 READ CAPACITY(10), 0x00 TUR, plus NOP OUT; the gate's write-opcode scan is bypassable, see M1, so this
is a human-verified fact, not an audited one), (b) starting with `--udump 1`, and (c) NOT running `tools/run-probe.sh` (its usb-log.py reader)
at the same time as `udump-recv.py` (H1). No Critical findings. Nothing in the udump path can write storage or leave the userdata GPT range.**

## Verdict table

| # | Requirement | Verdict | One line |
|---|---|---|---|
| 1 | READ-ONLY | PASS (code) / audit strength FAIL (M1) | Only READ(10)=0x28 to LUN0 inside userdata is reachable from udump; the "no WRITE opcodes, structurally" audit claim is overstated: 5 re-spellings of WRITE(10) pass source audit and a full `--udump` build |
| 2 | BOUNDS | PASS | Fails closed in all 16 edge cases I ran; two layers (udump_run + ufs_read_userdata); tests miss 4 of 8 bounds mutations (M3) |
| 3 | MEMORY SAFETY | PASS | stage 64 KiB exactly fits 16 blocks; pieces <= 4096 = USBG_BULK_MAX; usbgadget_write copies; no statics in udump.o |
| 4 | HANGS/TIMEOUTS | PASS | every loop has a deadline; worst-case waits listed; throughput/thermal unmeasured (M6) |
| 5 | PROTOCOL | PASS (Lows) | wire format and zlib CRC32 verified against an independent decoder; no resync by design (fail closed); live-capture END detection fragile (L1) |
| 6 | DEFAULT BUILD UNCHANGED | PASS (with caveat) | byte-identical Image in 4 configs when the build id is equal; raw Image differs by the embedded git hash only |
| 7 | RUN + PASTE | PASS | GATE PASS 63c0979; `--udump 1` build exit 0; host test 10/10 |

## Findings

### Critical
None.

### High

**H1 (workflow, not safety): the Spec's own hardware procedure puts two readers on /dev/ttyACM0.**
`tools/run-probe.sh:29-31` starts `usb-log.py capture` on the gadget tty and the Spec says "`tools/run-probe.sh` while `tools/udump-recv.py`
records". Linux ttys allow several readers; each gets an arbitrary subset of the bytes. The dump will arrive split between the two files, fail
the receiver's CRC/seq checks (fail closed, so no wrong data is accepted) and the run is wasted. Also udump_run waits only 5 s for DTR
(udump.c:190), so the receiver must already hold the tty open before udump runs.
Fix: do the dump run by hand (`udump-recv.py` started first, then `fastboot boot`, then `adb` wait), or give run-probe.sh a `--no-capture`
mode and make udump-recv.py the only reader; document it in the brief.

### Medium

**M1 (audit strength, inherited but now load-bearing): the WRITE-opcode scan is regex-based and the udump.o audit does not cover the SCSI path.**
`tools/test-bluejay-boot.py:147-210` only flags a bare literal in `{..}` first-element, `x[0] = <bare literal>`, `cmd[16+i] = <bare literal>`.
I patched `read_blocks_dma` (ufs.c:259) in a scratch copy with each of these and ran `check_ufs_safety_text`:
`{ 0x3A - 0x10, ...}` PASSES, `cdb[0] = 0x28 | 2;` PASSES, `*cdb = 0x2a;` PASSES, `#define OPW 0x2a` + `{ OPW, ...}` PASSES,
`cdb[0] ^= 2;` PASSES; the plain `{ 0x2A, ...}` control is caught. Two of them (A: `0x3A-0x10`, C: `*cdb = 0x2a`) were put through the complete
`build.sh --board bluejay --avb --usb-gadget --pstore-fast --udump 1`: exit 0, all 35 PASS lines including "ufs safety" and "udump object
audit". The udump.o audit proves udump.o has no MMIO stores and a 4-symbol undefined set, but it allows indirect calls (`blr` is in `_USB_NO_WRITE`)
through `h->read`, so the report sentence "no SCSI/UFS command path can even be called" (report-01, test-bluejay-boot.py:1401) is false: the
reader hook is set in shim.c, which is neither source- nor object-audited. No object-level audit of ufs.o exists at all.
I verified by reading that today's tree is clean: CDBs are only at ufs.c:251,259 (0x28), 354/546 (TUR 0x00), 363/559/650 (0x25), 370 (0x28);
ufsinit.c:233 `uic(0x16)` is the UIC DME_LINK_STARTUP command, not the QUERY UPIU; disassembly of the udump build shows `mov w5,#0x28; strb ... [sp,#82]`.
Fix: add an object audit of ufs.o that extracts every immediate stored into the CDB area (or at minimum allowlists the five CDB initialisers by
exact source line text), and pin `UPIU` flags/type constants the same way. Until then the lead's eyes on `git diff -- shim/ufs.c` are the control.

**M2: the gate never builds `--udump`.** `tools/check-all.sh:10-12` builds `--avb`, `--avb --pstore-fast`, `--avb --usb-gadget`. The `#ifdef UDUMP`
code in ufs.c/shim.c/ufs.h (the part that actually touches UFS) is therefore never compiled by the gate, so "GATE PASS 63c0979" says nothing
about the image that will be flashed. It builds today (I ran it, below), but a later edit could break it or add a path unseen.
Fix: add `step build-udump bash -c 'cd shim && ./build.sh --board bluejay --avb --usb-gadget --pstore-fast --udump 1'`.

**M3: host tests miss bounds mutations (8 tried, 4 survived).** Run against shim/udump.c with the shipped test:
| mutation | result |
|---|---|
| partition bound `blocks > part_blocks + 1` | caught |
| `ud_first == 0` check removed | caught |
| `blocks > UDUMP_MAX_BLOCKS + 1` | caught |
| 32-bit limit `> 0x100000000` instead of `> 0xFFFFFFFF` | **survives** (second layer ufs_read_userdata still stops it) |
| `ud_last < ud_first` check removed | **survives**: without it `part_blocks = ud_last-ud_first+1` wraps to ~2^64 and the dump proceeds; the only test case (`FAKE_FIRST-1`) is also refused by the next check. Second layer still blocks, so safe today |
| odd tail `n2 = 2` instead of 1 (reads one block past N) | **survives**: no test asserts max LBA read == first+N-1 for odd N. Unreachable on device (N is MIB*256, always a multiple of 16) |
| `lba++` skew after seq 3 | **survives**: no test exceeds 3 chunks (40 blocks max) |
| header lba truncated to 32 bits | survives (equivalent: lba < 2^32 enforced) |
report-01/NOTES claim "10/10 mutations caught"; that is true only for the mutations they chose. Fix: add (ud_first=1000, ud_last=500, N=1), an odd N whose `rd_max_lba == first+N-1`, and a >64-block run asserting contiguous reads.

**M4: GPT lookup is not validated and `udump_gpt_found` is set even when nothing matched.** ufs.c:730-737 sets `udump_gpt_found = 1` unconditionally at the end of
`ufs_full_sequence` (with `ud_first = ud_last = 0` when no `userdata` entry matched). `ufs_read_userdata` (ufs.c:604-617) has no `first != 0`
check, so it would serve LBA 0 in that state; today only udump_run's `ud_first == 0` refusal (udump.c:162) prevents it. Additionally: no "EFI PART"
signature or header/entry CRC check (pre-existing parse, ufs.c:670-690), name compared on the low byte of each UTF-16 unit only (ufs.c:704), no
check of `last` against the LUN0 capacity that is read two screens earlier (the device itself rejects LBA > capacity with a CHECK CONDITION, rc 4,
so this fails closed), no assertion that the LUN block size is 4096 (a different size would yield a residual != 0 and rc 3, again closed).
Fix: set `udump_gpt_found = (ud_first != 0 && ud_last >= ud_first)` and reject `last >= capacity` using the already-read READ CAPACITY value.

**M5: check-udump.sh compares against a live, mounted filesystem.** check-udump.sh:28 `dd`s userdata in the Android that has booted since the shim dump
(f2fs mounted RW, checkpoints and metadata updated). The first 1 MiB matched in the earlier proof, but 64 MiB of f2fs metadata (NAT/SIT/SSA/main) can
legitimately differ: expect a false MISMATCH. Fix: compare per record CRC windows over the first 1 MiB only, or capture Android's `dd` and the dump
without mounting (recovery with `/data` unmounted).

**M6: throughput and CPU time are unmeasured and likely CPU-bound.** The shim runs with MMU and caches off (bluejay_probe never calls mmu_enable; start.S sets
SCTLR with caches off), so instruction fetches and every data byte are uncached. udump.c:56-65 does a bitwise CRC32 (8 iterations/byte) **twice** per byte
(chunk CRC and running total, lines 232-233) and upiu_submit byte-zeroes 8 KiB per command. 64 MiB may not finish inside the remaining gadget window;
the code then stops cleanly with `UDUMP window over at seq=N` (udump.c:246), so this is bounded, not a hang. Thermal risk is low (one core, <= window).
Fix if needed: derive the total CRC from the per-chunk CRCs with `crc32_combine`-style math, or a 256-entry table placed on the stack/arg.

### Low

- **L1 receiver live capture** (`tools/udump-recv.py:218-224`): stops as soon as `UDUMP-END` plus a `\n` appears anywhere in the byte stream, including in payload. I fed a pty a stream whose first payload contained `UDUMP-END\n`: capture stopped at 4095/8336 bytes and parsing reported "truncated" (a false failure, fail closed). A marker split across two reads is not seen, so it waits the 60 s idle timeout, then parses fine. Fix: stop on the end-record parse, not on a text search.
- **L2 receiver crash on hostile digits**: `udump-recv.py:76` `int()` of a >4300-digit field raises an uncaught ValueError (traceback, nonzero exit). Wrap it.
- **L3 no resync** after a lost or garbled frame, by design: every malformed input I tried (log replay inside the stream, stray NUL after a record, truncated payload, bad nblocks, missing END, unterminated BEGIN, seq gap) was rejected with a clear message. A zero-block dump (`count=0` plus an end record with crc 0) is accepted as OK, and a second back-to-back dump is ignored. Acceptable for a verify-or-fail tool.
- **L4 end record deadline** (udump.c:260) uses a fresh 5 s, not clamped to `window_end`; worst case 5 s past the window, then usb_gadget_finish's >= 8 s. Bounded.
- **L5 build.sh `--udump 010`**: shell sees 10, but `-DUDUMP_MIB=010` is C octal 8 (smaller, so safe); `--udump 08` fails to compile (closed). `--udump 0` silently means "off". Quote/normalise the integer.
- **L6 byte-identical claim**: build.sh embeds `git rev-parse --short HEAD` as `PSTORE_BUILD_ID`; the raw Image at 63c0979 differs from a 6ea2970 baseline by that string only (see Req 6).
- **L7 "userdata not found in GPT" message** (shim.c) is also what prints when the UFS sequence aborted early; wording only.
- **L8 pump replay**: `usbg_pump` replays the entire retained log whenever `ready` toggles 0->1. If the host bounces DTR mid-dump the replayed text lands inside the record stream; udump aborts on the next `usbgadget_ready()` check and the receiver rejects the stream, so no corrupt data is accepted.

### Added by pass B (not in pass A)

**H2 (audit hole, High): the udump.o "no MMIO" object audit can be bypassed with a store through a pointer built from a hook-struct field.**
Mutation (scratch copy, `udump.c` first line of `udump_run`): `*(volatile uint32_t *)(uintptr_t)(h->ticks_freq + 0x11110000u) = 0xdead;`
compiles to `ldr x1,[x0,#32]; mov x0,#0x11110000; str w3,[x1,x0]` (objdump of the scratch udump.o) and `check_udump_object` reports
`AUDIT PASSED: 57 stores` (baseline 56). The audit treats x0-x7 as trusted "argument" bases even after the register has been overwritten by a load,
and accepts a register-offset store. In the full `build.sh --udump 1` this mutation only failed by accident (exit 139: `udump_host_test` segfaulted on
the wild address, *before* the boot-safety step), so a store to a host-mapped address would pass everything. report-01 and CONTEXT.md "Gate honesty"
rely on this audit as the proof that udump.c has no MMIO; it is a proof only for the current source (which I read in full: every store is to
`line`, `tmp`, `hdr` or a helper's own argument). Fix: in `check_usb_object_text` kill the trusted status of x0-x7 on ANY definition (ldr/mov/add/...),
reject register-offset and pre/post-index store forms outright for udump.o, and add this case to `udump_object_selftest`.

**M1 addendum**: three more spellings that pass the source audit AND a full `--udump 1` build (exit 0, 31 PASS lines, `mov w5,#0x2a; strh w5,[sp,#80]`
in `read_blocks_dma.constprop.0`): `{ '*', ...}` (char literal), `{ 0x30 - 5 - 1, ...}`, and `cdb[0] += 2;` after the 0x28 initializer.

**M3 addendum (test gaps, pass B mutations on udump.c vs the shipped host test)**: caught: `blocks>part_blocks` removed, `ud_first==0` removed, stage_len check
removed, `seq++` removed, `total_crc` chained wrongly, header CRC xor, END text typo, sliding ready deadline (hangs -> caught by timeout), `past(deadline)` removed.
SURVIVED: `ud_last < ud_first` removed (my 2M-case fuzz shows this check is then the only guard against the wrapped `part_blocks`; the ufs.c layer still stops
it), odd-tail `n2=2`, the per-chunk 5 s budget replaced by "window only", and the mid-dump `usbgadget_ready()` check in `send_pieces`.

**M4 addendum**: `elba` (GPT entries LBA) is read from 2 of the 8 header bytes (ufs.c:687) and `nent` from 3 of 4 (:686); the entry-name match ignores the high byte of each UTF-16
unit (:707) and takes the first match without a duplicate check. Resolved by 00a5955: `udump_gpt_found` is now `ud_first != 0 && ud_last >= ud_first` (ufs.c:739).

**L9 (observation)**: `usb_gadget_finish` waits out the rest of the 600 s gadget window after a finished dump (shim.c:421-423), so the phone stays in the shim for ~10 min
from gadget start regardless of how fast the dump was. Pre-existing, but with `--udump` the user will expect a quick return to Android.

## Requirement details

### 1. READ-ONLY (PASS for the code; audit claim FAIL, see M1)
Everything the udump path can send, enumerated from udump.c -> hook -> ufs.c:
| UPIU/command | opcode | direction/flags | where | udump path? |
|---|---|---|---|---|
| COMMAND, SCSI READ(10) | 0x28 | data-in, UPIU flags 0x40, UTRD DD=2 | `read_blocks_dma` ufs.c:259 via `ufs_read_userdata` ufs.c:614; LUN 0; <= 2 blocks | **yes, the only one** |
| NOP OUT | UPIU 0x00 | none | ufs_run_attempt ufs.c:538 | no (earlier in boot) |
| TEST UNIT READY | 0x00 | none | ufs.c:548 | no |
| READ CAPACITY(10) | 0x25 | data-in | ufs.c:363,559,650 | no |
| READ(10) LBA 0 / GPT / userdata_crc | 0x28 | data-in | ufs.c:370,666,693,628 | no (earlier, existing) |
No query UPIU (0x16 type) exists (`enum upiu_type` has two values, `_Static_assert` intact; ufs.c enum scsi_dir untouched). No SSU, UNMAP, or any WRITE.
`git diff` shows no change to upiu_submit/scsi_dir.
Bypass experiments (scratch copies of ufs.c, the `read_blocks_dma` CDB made a WRITE(10) 0x2A):
| spelling | source audit | full `--udump` build + all audits |
|---|---|---|
| A `{ 0x3A - 0x10, ... }` | passes | **passes, exit 0** |
| B `cdb[0] = 0x28 \| 2;` | passes | (not built; same class) |
| C `*cdb = 0x2a;` | passes | **passes, exit 0** |
| D `#define OPW 0x2a` + `{ OPW,...}` | passes | (not built) |
| E `cdb[0] ^= 2;` | passes | (not built) |
| control `{ 0x2A, ... }` | caught | n/a |
The compile-time control (no write value in `enum scsi_dir`) is not defeated by these because direction flags would still say READ, but the opcode on the wire would be WRITE(10); what the UFS device does with that mismatch is unknown. Hence M1.

### 2. BOUNDS (PASS)
udump_run (udump.c:154-185), in order: blocks==0 or > 16384; stage_len < 64 KiB; ud_first==0; ud_last<ud_first; blocks > ud_last-ud_first+1 (u64);
overflow guard; last LBA > 0xFFFFFFFF. All refusals precede any send. `ufs_read_userdata` re-checks found, nblocks 1..2, lba <= 0xFFFFFFFF,
lba >= first, lba+n-1 <= last (no overflow since lba <= 2^32-1). Harness `/tmp/rv/h` linked against the real udump.c, results (first last N -> outcome, reads min..max):
- `100 99999 3` OK, 100..102; `100 100 1` OK, 100..100; `100 100 2` refused "N > partition"; `100 101 2` OK; `100 102 4` refused; `100 109 0` refused; `0 99 1` refused ud_first=0
- `1 UINT64_MAX 16384` OK, 1..16384 (8192 reads of 2); `.. 16385` refused
- `0xFFFFFFFE 0xFFFFFFFF 2` OK; `0xFFFFFFFF 0xFFFFFFFF 1` OK; `0xFFFFFFFF 0x100000000 2` refused "LBA > 32-bit"; `0x100000000 .. 1` refused
- `5 3 1` refused "GPT range invalid"; `UINT64_MAX UINT64_MAX 1` refused (LBA>32); `... 2` refused
N_MIB larger than the partition -> refused "N > partition"; N=0 -> refused (build.sh `--udump 0` means off; > 64 rejected by build.sh and by `#error`).
GPT parse caveats are in M4. 4 KiB vs 512 B: all units are 4096 (BLOCK, GPT LBAs on the 4 KiB LUN); a mismatch fails via residual (rc 3).

### 3. MEMORY SAFETY (PASS)
- `st + b*4096` for b in {0,2,..,14}, n2 <= 2 -> highest byte written is chunk_end <= 64 KiB = `ufs_udump_stage` length and the stage_len check (udump.c:158). upiu_submit zeroes and DMAs exactly `count*4096` <= 8192 with 2 PRDT entries.
- Pieces: `want <= 4096` (udump.c:124); `usbgadget_write` clamps to USBG_BULK_MAX=4096, copies into the pool before returning (usbgadget.c:747), so the stage buffer is free to be overwritten immediately; the return value (partial accept) is honoured (`off += ret`). Short-write test: a 1000-byte-per-call fake produced a byte-identical stream to the full-write run.
- Header: `UDUMP_HDR`=24 matches `<4sIQII`; put_header writes bytes 0..23. `line[48]`/`[24]` formatters are capacity-checked; max BEGIN line 33 chars. udump.o has no .data/.bss, no memcpy/memset reference.
- The only statics are in ufs.c (stage, copied dma struct) and shim.c (hooks struct), all inside the shim image.

### 4. HANGS / TIMEOUTS (PASS)
| wait | bound |
|---|---|
| host not attached/no DTR | 5 s ready loop (udump.c:190-197) then "gadget not ready" |
| text drain before first record | 5 s (udump.c:207) |
| host stops reading / bulk busy | per chunk min(5 s, window_end) (udump.c:238-252) |
| device drops (configured/DTR lost) | next `send_pieces` iteration returns "stalled" |
| UFS command timeout | upiu_submit 1 s + UTRLCLR then rc 1 -> "read failed" and abort (no retry) |
| end record | 5 s (L4) |
| after udump | usb_gadget_finish >= 8 s, then `usbgadget_stop` (<= 500 ms), then pstore-fast 5 s, `bluejay_reset_chain` (PSCI SYSTEM_RESET, SYSTEM_RESET2, WDT CL0, last resort `wfi` with a hold-Power message) |
The shim disables both watchdogs at start (pre-existing), so a bug-induced hang would need Power long-press, which is the documented recovery and does not touch storage. CPU time/thermal: M6. Nothing writes to UFS at the end; the reset chain is unchanged.

### 5. PROTOCOL (PASS)
Magic "UDMP", seq u32, lba u64, nblocks u32, crc u32, all little endian, CRC32 zlib reflected 0xEDB88320, init/final ~0, chained correctly (`crc32_update(total, ...)`). Independent check: I produced a 40-block stream from the real udump.c with my own harness (fake reader, not the implementer's test) and fed it to `udump-recv.py --from`, whose Python zlib verified every record and the end record: `UDUMP OK first=100 blocks=40 bytes=163840 crc32=53916014`; a run with 1000-byte partial writes gave a byte-identical stream. (I did not separately recompute the pattern CRC by hand: /tmp scratch was wiped by something outside this review before I could re-run that one cross-check, so that is not claimed.) Receiver selftest: `PASS ... good stream parsed; 8/8 bad streams rejected`. Seq and LBA continuity enforced, nblocks <= 16, end CRC vs zlib of all payload, BEGIN count vs received, UDUMP-END required. Raw tty setup passes all 256 byte values through a pty unmodified.
Malformed inputs tried against `parse_stream`/`capture_tty` (details in L1-L3): payload containing `UDUMP-END\n` (false truncation), payload containing `UDUMP-BEGIN` text (ignored, correct), 5000-digit `first=` (uncaught ValueError), stray byte between records (rejected), replayed BEGIN line mid-stream (rejected), huge nblocks (rejected), truncated everywhere (rejected). The final SHA-256 is done by check-udump.sh only (M5).

### 6. DEFAULT BUILD UNCHANGED (PASS)
Raw `out/Image` of baseline vs udump HEAD differs only because build.sh embeds `git rev-parse --short HEAD` (L6). With `build_id=fixed00` in both scratch build.sh copies (nothing else altered):
| config (no --udump) | baseline 6ea2970 | udump 63c0979 |
|---|---|---|
| `--avb` | a6549565c205d8817a285ff9dff7fe4f26daf319f66783fe5f0f2b2672fbd4d0 | same |
| `--avb --pstore-fast` | 5850cf7d28274f23... | same |
| `--avb --usb-gadget` | c221bdfbad939a78... | same |
| `--avb --usb-gadget --pstore-fast` | 98f199e78b8e4d97... | same (`cmp -l`: 0 bytes) |
`--udump` only adds code: with the same config, console/usbgadget/usbproto/ufsinit/usbprobe/ramlog/pstore .o are byte-identical; shim.o adds `udump_gadget_poll`, `udump_log_pending` and a longer `shim_main` (1063 -> 1149 insns); ufs.o adds `ufs_read_userdata`, `ufs_userdata_found`. Some existing functions change instruction count only through gcc re-inlining (`usbg_service` 136 -> 34 as `usbg_pump` is outlined; `upiu_submit.isra` 299 -> 291 because `read_blocks_dma` has a new non-constant caller); their source is unchanged. Image grows 286720 -> 364544 B (the 64 KiB stage plus usb/udump code).

### 7. Real output

`tools/check-all.sh` (in <worktree>):
```
GATE PASS 63c0979

real	0m17.213s
```

`cd shim && ./build.sh --board bluejay --avb --usb-gadget --pstore-fast --udump 1` (tail; exit 0):
```
PASS usb object self-test: 20/20 bad objects rejected
PASS usb object audit: 119 stores, all stack-based; undef/.data/.bss/instruction bans clean
PASS usbproto object self-test: 4/4 bad objects rejected
PASS usbproto object audit: 38 stores, all stack/arg/adrp-image based; undef empty; bans clean
PASS usbgadget writer audit: try_write32 referenced only by a bl inside w32; undef subset; no banned instructions
PASS udump object self-test: 4/4 bad objects rejected
PASS udump object audit: 56 stores, all stack/argument-based; .data/.bss empty; undef subset (no SCSI surface, no MMIO)
PASS header v4 (lz4): kernel=52573 ramdisk=98; host tests & safety checks passed
-rwxrwxr-x 1 user user   364544 Oct 21 13:55 out/Image
exit=0
176980a60c53dfb66789a6f30605a8c3ceb8e50144b507a27aa97cf18259feb5  out/Image
```
(build_id there is the real 63c0979.)

Host test: `gcc -Wall -Wextra -Werror -I. -o /tmp/udump_test udump.c udump_host_test.c && /tmp/udump_test` ->
```
PASS udump_host_test: 10/10 tests
```
Receiver: `python3 -I tools/udump-recv.py --selftest` -> `PASS udump-recv self-test: good stream parsed; 8/8 bad streams rejected`.

## Not verified (needs the phone / lead)
- Whether UFS DMA into the stage buffer (high in the shim image, 64 KiB) behaves like the verified 8 KiB `data[]` (S2MPU hypothesis in HANDOFF); failure mode is a read timeout -> "UDUMP read failed", closed.
- Real throughput (M6), the 5 s/chunk budget, and whether the host attaches within udump_run's 5 s ready wait.
- check-udump.sh was only read, never run.

## Pass B: independent second pass (evidence, same requirements)

## Requirement 1: READ-ONLY  -> PASS for the code as written; FAIL for the gate's ability to keep it that way (see M1, H2)

Reviewed at 63c0979 (HEAD moved to 00a5955 mid-review; the delta is 4 lines in ufs.c + 1 gate line, noted where relevant). Scratch: /tmp/rv2 (build tree `/tmp/rv2/wt`, a copy of the worktree without .git; /tmp/bypass from the
earlier reviewer was already gone). Nothing in the real worktree was edited.

### Every UFS transaction the --udump image can issue (all call sites of `upiu_submit`, grep over shim/*.c)

| # | UPIU type | CDB / payload | dir (UPIU flags / UTRD DD) | where | on udump path? |
|---|---|---|---|---|---|
| 1 | NOP OUT (0x00) | none | NONE (0x00 / 0) | ufs.c:538 `ufs_run_attempt` | boot-time, pre-existing |
| 2 | COMMAND (0x01) | TEST UNIT READY `00 00 00 00 00 00` | NONE | ufs.c:548 (x3 retries), ufs.c:357 | pre-existing |
| 3 | COMMAND | READ CAPACITY(10) `25 ...`, 8 B in | READ (0x40 / 2) | ufs.c:560, :654 (LUN 0..3), :364 | pre-existing |
| 4 | COMMAND | READ(10) `28 00 lba[4] 00 count[2] 00` | READ | ufs.c:259 `read_blocks_dma` (LUN fixed 0 by every caller), :251 `read_blocks`, :371 `rd10` | **yes**: only via ufs.c:614 `ufs_read_userdata` |

No other `upiu_submit`/`scsi` call exists. The only new caller is ufs.c:614 (`ufs_read_userdata`, `nblocks` in 1..2). `udump.c` has
no SCSI code at all: its undefined-symbol set is exactly {usbgadget_write, usbgadget_ready, con_str, con_hex2} (nm of udump.o,
enforced by `_UDUMP_ALLOWED_UNDEF`, test-bluejay-boot.py:1404). `udump.c` reaches UFS only through an INDIRECT call `h->read(...)`
bound once in shim.c:`udump_dump()` (`uh.read = ufs_read_userdata`). There is no WRITE(10)/UNMAP/START STOP UNIT (0x1B)/SYNCHRONIZE
CACHE/FORMAT, no QUERY REQUEST (0x16) and no DATA OUT UPIU in the diff or in the shipped objects: `enum scsi_dir`
{NONE,READ} and `enum upiu_type` {NOP_OUT,COMMAND} and their `_Static_assert`s are unchanged (ufs.c:100-119; diff of the enums is
empty). `git diff 6ea2970..HEAD -- shim/ufs.c` adds no CDB, no register write and no `upiu_submit` call. LUN is hard-coded 0 in the
new path (ufs.c:614). Not UFS-protocol writes but worth listing: `upiu_submit` zeroes the *destination buffer* (ufs.c:185
`zero(buf, len)`) before DMA - for udump that is the shim's own `ufs_udump_stage`, never device data.

### Three bypass spellings of a WRITE(10) (opcode 0x2A), actually built end to end

Method: copy of the worktree in /tmp/rv2/wt, patch `read_blocks_dma`'s CDB initializer (ufs.c:259, the only CDB the dump uses),
then full `./build.sh --board bluejay --avb --usb-gadget --pstore-fast --udump 1` (which runs every host test + test-bluejay-boot.py).
Control (`{ 0x2a, ...}`) is rejected: `forbidden opcode 0x2a (WRITE(10)) as first CDB element in initializer`, build rc=1.

| spelling | build.sh / gate | proof the WRITE opcode reached the object |
|---|---|---|
| A `{ '*', 0, ...}` (char literal, 0x2A) | **MISSED**, rc=0, 31 PASS lines, `PASS header v4 (lz4)` | `read_blocks_dma.constprop.0`: `mov w5,#0x2a; strh w5,[sp,#80]` = cdb[0] |
| B `{ 0x30 - 5 - 1, 0, ...}` (constant-folded) | **MISSED**, rc=0, same PASS set | same folding to 0x2a |
| C `cdb[0] += 2;` after the 0x28 initializer | **MISSED**, rc=0, same PASS set | same |
| (D, source-audit level only) `#define OPW 0x2a` + `{ OPW, ...}` | MISSED by `check_ufs_safety_text` (script run on the patched text) | - |

(I did not try `0x28 | 2`: the literal `2` is itself in `FORBIDDEN_OPCODE_BYTES` (0x02 Set Flag), so that spelling would be a
false-positive catch. Likewise `0x30 - 6` would trip on 6.) Why: `check_ufs_safety_text` (test-bluejay-boot.py:147-220) looks only for a
*bare literal* in cdb[0]/initializer-first-element; there is no object-level check of ufs.o, and the enum-based "can't express a
write" claim covers only the UPIU data-direction flag - a WRITE(10) CDB with the READ flags (0x40/DD=2) still reaches the device as a
WRITE opcode. The audits also scan **only** `shim/ufs.c` (test-bluejay-boot.py:1526); `ufsdiag.c`/`ufsinit.c` are not source-scanned.
These are gate-strength findings (M1), not defects in the current code: the current code has none of these spellings.


## Requirement 2: BOUNDS  -> PASS for the read window; FAIL on two defence-in-depth sub-items (GPT validity, LUN capacity) - both fail closed in practice

Two independent layers: `udump_run` (udump.c:154-185) and `ufs_read_userdata` (ufs.c:604-615). Math, all u64:

- udump.c:154 `blocks==0 || blocks>16384` refused; :162 `ud_first==0` refused; :166 `ud_last<ud_first` refused.
- udump.c:170 `part_blocks = ud_last - ud_first + 1`: cannot wrap because ud_first>=1 and ud_last>=ud_first (max result UINT64_MAX).
- udump.c:171 `blocks > part_blocks` refused. udump.c:175 `ud_first > UINT64_MAX-(blocks-1)` (unreachable, harmless). udump.c:181 `ud_first+blocks-1 > 0xFFFFFFFF` refused (blocks>=1 so no underflow).
- Read loop (udump.c:224-231): `h->read(lba+b, n2, st+b*4096)`, b even in 0..14, n2 = 2 except the last odd block; `b*4096+n2*4096 <= 65536` = the stage-length check at udump.c:158. Final read ends at `first+blocks-1` exactly (off-by-one verified below).
- ufs.c:608-613 (fn at 606): `!found || nblocks==0||>2`, `lba>0xFFFFFFFF || lba<first`, `lba+nblocks-1 > last` (no wrap: lba<=2^32-1). Then `(uint32_t)lba` into the CDB: no truncation after the 32-bit check. `count*BLOCK` in read_blocks_dma: count<=2, no overflow. 4 KiB units throughout (GPT LBAs are in 4096-byte blocks on UFS LUN 0; CDB LBA is in logical blocks of the same size; BLOCK=4096 matches the READ CAPACITY block size printed at ufs.c:664).

Evidence (my own harness, NOT the implementer's test): real udump.c (CRC stubbed for speed; the bounds code untouched) driven by a
read hook that flags any LBA outside [first,last], outside [first,first+N-1], above 0xFFFFFFFF, equal to 0, a read of 0 or >2 blocks,
or a total block count != N on a successful dump. Inputs: first,last in {0,1,2,3,4,15,16,17,255,256,16383,16384,16385,0x7FFFFFFF,
0xFFFFFFF0,0xFFFFFFFD..0xFFFFFFFF,2^32,2^32+1,2^63-1,2^64-16,2^64-1,0x22F1E6,0x1DC03FA} x (last-1,last,last+1) x
N in {0,1,2,3,15,16,17,255,256,257,16383,16384,16385,65536,0xFFFFFFFF}, plus 2,000,000 random triples biased to the 2^32 edge:
`cases=2028125 complete=741163 bad=0`. Mutation sanity (the harness must fail when a check is removed): drop `blocks>part_blocks`
-> bad=71582; drop the 32-bit check -> bad=760571; drop `last<first` -> bad=4247; `blocks>part_blocks+1` (off-by-one at the far end)
-> bad=141. So the harness does see end-of-range off-by-ones.

Behaviours asked for:
- N_MIB larger than partition: refused "N > partition" (udump.c:171), nothing sent. N_MIB>64: build.sh:30 refuses at build time (also `#error` udump.h:47-48).
- N_MIB=0: build.sh treats 0 as "udump OFF" (build.sh:26 `!= 0`); `-DUDUMP_MIB=0` by hand gives blocks==0 -> refused (udump.c:154).
- Malformed GPT: ufs.c:689 requires `esz==128 && 0<nent<=128` (entry buffer is 128*128 B, :693 reads <=4 blocks: safe); on any early `return` `udump_gpt_found` stays 0 -> "userdata not found in GPT". A GPT whose entry named "userdata" is missing leaves `ud_first=0`, and the udump_run `ud_first==0` refusal catches it.

Sub-items that are NOT satisfied (all are in pre-existing ufs_full_sequence, but the dump now relies on them for its safety property):
- GPT header is never validated: no "EFI PART" signature check (ufs.c:676-689 only prints it), no header CRC32, no entries-array CRC32, no myLBA/revision check. Entries LBA is taken from only 2 of its 8 bytes (`gpt[72] | gpt[73]<<8`, ufs.c:687) and the entry count from 3 of 4. A damaged/foreign block 1 with esz==128 is parsed as a GPT. Consequence is bounded (read-only, entry name must literally be "userdata", range is still capped at 64 MiB), but the property "bounded to the userdata partition" is then only as good as unauthenticated GPT bytes. -> M4.
- Name match uses only the low byte of each UTF-16 unit (ufs.c:707 `e[56+2*c]`), so U+0175 etc. also match "u"; the first match wins and duplicates are not detected. -> M4.
- `ud_last` is never checked against the READ CAPACITY(10) result of LUN 0 (ufs.c:650-664 reads it, prints it, discards it). A GPT range extending past the LUN is accepted; the device would answer the READ with CHECK CONDITION (rc 4 -> "UDUMP read failed", fail closed), so no harm, but the brief's explicit check is missing. -> M4.
- At 63c0979 `udump_gpt_found = 1` was set unconditionally at the end of ufs_full_sequence (including when no userdata entry matched, first=last=0), so "userdata not found in GPT" (shim.c:453) could never print and ufs_read_userdata's own check would accept LBA 0 if called with first=0; only udump_run's `ud_first==0` refusal saved it. **Fixed by 00a5955** (ufs.c:739 `udump_gpt_found = ud_first != 0 && ud_last >= ud_first;`, also `gate builds --udump`), which landed in the worktree while this review was running; I re-verified the final state at 00a5955 (see Requirement 7). Residual: ufs_read_userdata still does not itself reject first==0 (unreachable now). -> M4 (resolved by 00a5955).
- build.sh:30 accepts `--udump 010` (the shell test is decimal) but the C compiler reads `-DUDUMP_MIB=010` as octal 8 (verified with `gcc -E`: `((uint32_t)010 * 256u)`): a surprising N, still within 64. `08`/`09` fail the build. -> L5.

## Requirement 3: MEMORY SAFETY  -> PASS

- Buffers: stage = `ufs_udump_stage[65536]`, 4 KiB aligned (ufs.c:587), checked by `stage_len >= 16*4096` (udump.c:158). Largest write into it: `h->read(lba+b, n2<=2, st+b*4096)`, b<=14 -> end offset 14*4096+2*4096 = 65536 exactly (never beyond). The UFS command descriptor has exactly two PRD entries (`prdt[2]`, ufs.c:~67) and `nprd = ceil(len/4096) <= 2` because count<=2 is enforced in `ufs_read_userdata` (ufs.c:608) before the DMA is set up. The stage is part of the Image (bss is inside `_end`, `truncate -s $size`, image_size = `_end` in start.S:8; `.bss` is zeroed by start.S:29-31), so it cannot overlap anything ABL loads next to the kernel and it is excluded from the USB pool by `avoid[0]`.
- USB bulk buffer: `usbgadget_write` clamps `len` to USBG_BULK_MAX (4096) and *copies* into its own pool buffer byte by byte (usbgadget.c:736-754), so the stage is never DMA'd by the DWC3 directly and can be refilled right after the write returns. `send_pieces` never asks for more than UDUMP_PIECE=4096 (udump.c, `want`), handles a partial/zero return (`off += write(...)`) and re-polls. A write return larger than `want` is impossible by the callee's contract.
- Frame header: stack `hdr[24]`, `put_header` writes bytes 0..23 exactly (magic 4, seq 4 @4, lba 8 @8, nblocks 4 @16, crc 4 @20). CRC: zlib reflected poly 0xEDB88320, `~` in/out, chainable; checked end to end below (device framer -> receiver -> independent python zlib: equal). Header u64/u32 are byte-assembled, so `-mstrict-align`/MMU-off alignment faults cannot occur.
- Text: `line[48]` and `line[24]` are only written through `put_str/put_dec/put_hex8`, all `pos+1<cap` guarded; worst case "first=" + 10 digits + " count=" + 5 digits = 28 < 48. console.c copies into a 96-byte ramlog line with its own cap (RAMLOG_LINE_MAX=96).
- No static state in udump.c (`.data/.bss` empty is enforced in the object audit); the only mutable statics are `uh` (shim.c, hooks struct) and the ufs.c stage/win-dma. Stack frame of `udump_run` is 224 bytes (objdump: `stp x29,x30,[sp,#-224]!`) on a 32 KiB stack.
- Nothing writes outside shim-owned buffers: the audited store classes are stack and argument-register bases only. **But see H2: that audit has a hole**, so this verdict rests on my reading of the source plus the audit, not on the audit alone. I read the whole of udump.c: every store is to `line`, `tmp`, `hdr`, or `dst[pos]`/`p[i]` of a helper argument.
- Interleaving of the text pump into a record: the pump only writes when `usbg.off<usbg.len` or new ramlog lines exist, and `usbgadget_write` is single-in-flight (`bulk_busy`), so text can only land *between* pieces if something logs mid-dump. The success path logs nothing; on UFS error (`upiu_submit` prints 8+ "UFS ocs" lines) the dump aborts at once and the lines follow the last complete record. A host DTR toggle (ready 1->0->1 inside one `usbgadget_poll`) is not seen by the pump (no replay) and loses in-flight bytes -> the receiver's CRC/seq check catches it.

## Requirement 4: HANGS / TIMEOUTS  -> PASS (all waits bounded); one design observation (L9)

| situation | what bounds it | evidence |
|---|---|---|
| no host attached / DTR never set | `ready_dl` computed once (5 s), loop `h->poll(); past()` -> ERR_NOT_READY, nothing sent | udump.c:190-197; mutation "deadline slides" -> host test hangs (timeout exit 124), so it IS covered |
| host stops reading mid-dump | bulk TRB never completes (`bulk_busy` stays 1 forever, no TRB timeout in usbgadget.c), `usbgadget_write` returns 0, `send_pieces` spins until `dl = min(now+5 s, window_end)` -> ERR_STALLED "UDUMP stalled at seq=N" | udump.c send_pieces + chunk loop |
| host drops (ready 0) | `send_pieces` checks `usbgadget_ready()` every iteration -> returns 1 -> STALLED | |
| text-log drain before BEGIN | `drain_dl` 5 s, STALLED seq=0 | udump.c:207-213 |
| UFS timeout mid-dump | `upiu_submit` waits 1 s on the doorbell then UTRLCLR + rc 1 (ufs.c:202) and 10 ms for OCS write-back; `udump_run` returns ERR_READ at the first failing read; no retry loop | |
| window over | `dl` is clamped to `window_end`; a chunk read+CRC phase is NOT window-checked, so at most one extra chunk of CPU work (a few hundred ms) after the window ends | |
| CPU spin loops | CRC and `zero()` loops are length-bounded; no unbounded `while` outside the three deadline loops above | |
| watchdog | both Exynos WDTs are disabled early in bluejay_probe (shim.c:473-474) and the dump runs after every other probe; no petting needed | |
| end of run | `udump_dump()` -> `pstore_save()` -> `usb_gadget_finish()` (shim.c:602-606) keeps polling for >=8 s so END text drains, then `usbgadget_stop()` and, with --pstore-fast, 5 s + `bluejay_reset_chain()` (PSCI SYSTEM_RESET -> RESET2 -> WDT) | unchanged chain |

Mid-dump aborts are always reported *after* sending stops (`report_stall`, `report_seq_line`), so an error line cannot land inside a record.
Observations: (a) `usb_gadget_finish` waits until `usbg_end` (600 s after gadget start) when that is later than now+8 s (shim.c:421-423), so after a *successful* dump the phone still sits in the shim for the rest of the 10-minute window before it resets (pre-existing behaviour, unchanged by udump, but it means "dump done" is not "phone back in <1 min"). (b) UNSURE (hardware only): the dump reads UFS tens of seconds after the last command of the UFS section, via the saved `udump_win_dma`; if the link auto-hibernates in between, the first READ times out after 1 s and the dump fails closed with "UDUMP read failed at seq=0". The lead's N=1 run will show this.
Throughput is not claimed: `zero()` (volatile byte stores, ufs.c:185), two CRC passes and the byte-wise copy in `usbgadget_write` all run byte-at-a-time with the MMU off; N=64 may hit the window end and finish with rc=5 (`UDUMP window over at seq=`), which is still a clean, verifiable partial dump.

## Requirement 5: PROTOCOL  -> PASS for format/CRC/seq/end; NOT SUPPORTED: resync after a lost frame (not demanded by the spec, but see L1/L3)

End-to-end check (not the implementer's tests): I linked the *real* udump.c (real CRC) to a file-writing fake with the pump's text format (`"%s %s\r\n"` from con_str, BEGIN before the first record, END after) for first=0x22F1E6, 256 blocks, and fed the file to `tools/udump-recv.py --from`: `UDUMP OK first=2290150 blocks=256 bytes=1048576 crc32=4B3CDEB5`; an independent `python3 zlib.crc32` of the same 256 pattern blocks gives `4B3CDEB5`. So polynomial, reflection, endianness (all LE), seq numbering from 0, absolute LBA, and the end record (`lba = first+blocks`, `nblocks=0`, crc = all-payload CRC32) agree between firmware and receiver. `--selftest`: 8/8 bad streams rejected.

Three malformed inputs against the receiver:
1. **Payload that contains the text `UDUMP-END\r\n`** (a copy of these logs or the repo lying in the first 64 MiB of userdata): `capture_tty` (udump-recv.py, `if b"UDUMP-END" in data ... break`) treats it as the end of the dump. Sent 131215 bytes; the live capture stopped after **4096** bytes -> `truncated: payload cut at seq=0`, while `parse_stream` on the full bytes is OK. A good dump fails spuriously. A marker that straddles two 4096-byte reads is the opposite case (missed; falls back to the 60 s idle timeout, parse still fine). -> L1/L3.
2. **Giant digit string** `UDUMP-BEGIN first=9999...(5000 digits)`: `int()` raises `ValueError: Exceeds the limit (4300 digits)` which is not a `DumpError` -> uncaught traceback (exit 1, so still nonzero) instead of a MISMATCH line. -> L2/L3.
3. **`count=0` with an immediate end record**: accepted as success (`first=5 blocks=0 bytes=0`, exit 0, writes a 0-byte file); `check-udump.sh` then compares sha256 of nothing with `dd count=0` and prints `MATCH`. The firmware can never emit count 0, so this is defence in depth. -> L2/L3.
Also: a single flipped payload bit anywhere in a 1 MiB dump (`flip.bin`) gives `MISMATCH: record seq=15 CRC ...`, exit 1, and **no file and no raw capture is written** - the whole dump is discarded; there is no resync (`parse_stream` aborts at the first bad record, `capture_tty` never saves `buf`). Lost frame -> `seq gap`, same. Early disconnect (ENODEV/EIO or node vanished) -> partial buffer -> `truncated`. The END line's `total=`/`blocks=` text is not compared (only the binary end record is), and the "final SHA-256" exists only in check-udump.sh (receiver prints CRC32). `check-udump.sh`: see M5.

## Requirement 7 (pass B): real output at HEAD 00a5955 -> PASS

`git rev-parse --short HEAD` = 00a5955 (the delta from 63c0979 is `udump_gpt_found = ud_first != 0 && ud_last >= ud_first` in ufs.c and a `build-udump` step in check-all.sh).
```
$ tools/check-all.sh
GATE PASS 00a5955          (gate.log steps: build, build-fast, build-usb, build-udump, host-tests, boot-safety, unit, validate)
$ cd shim && ./build.sh --board bluejay --avb --usb-gadget --pstore-fast --udump 1      # exit 0, 35 PASS lines; the last ones:
PASS udump_host_test: 10/10 tests
PASS udump object self-test: 4/4 bad objects rejected
PASS udump object audit: 56 stores, all stack/argument-based; .data/.bss empty; undef subset (no SCSI surface, no MMIO)
PASS header v4 (lz4): kernel=52579 ramdisk=98; host tests & safety checks passed
$ ./out/udump_host_test
PASS udump_host_test: 10/10 tests
$ python3 -I tools/udump-recv.py --selftest | tail -1
PASS udump-recv self-test: good stream parsed; 8/8 bad streams rejected
$ bash -n shim/build.sh tools/check-udump.sh        # ok
```
(At 63c0979 the same three commands gave `GATE PASS 63c0979`, the same 35 PASS lines and 10/10.) 00a5955's only code change sits inside `#ifdef UDUMP`, so the
requirement-6 result (default image unchanged apart from the embedded build id) still holds. I left the worktree's gitignored `shim/out/` holding the `--udump 1` build; nothing tracked was modified.

## Final verdict (pass A + pass B, HEAD 00a5955)

| # | Requirement | Verdict | Note |
|---|---|---|---|
| 1 | READ-ONLY | PASS (code) / FAIL (audit strength: M1, H2) | only READ(10)/READ CAPACITY/TUR/NOP OUT reachable; 3 + 5 WRITE(10) spellings pass the gate |
| 2 | BOUNDS | PASS | 2.03M-case fuzz of the real udump_run: 0 reads outside [first, min(last, first+N-1)], never LBA 0 or > 2^32-1; 4/4 injected bounds mutations detected by the fuzz; GPT/capacity validation gaps fail closed (M4) |
| 3 | MEMORY SAFETY | PASS | stage 64 KiB exact fit; pieces <= 4096; usbgadget_write copies; no statics |
| 4 | HANGS/TIMEOUTS | PASS | every wait bounded; UFS-idle-after-minutes behaviour is UNSURE (hardware) |
| 5 | PROTOCOL | PASS | firmware -> receiver CRC32 equals independent zlib; no resync (fail whole dump, nothing saved); live END detection fragile (L1); H1 workflow |
| 6 | DEFAULT BUILD UNCHANGED | PASS (build-id caveat) | |
| 7 | RUN + PASTE | PASS | GATE PASS 00a5955, build exit 0, host test 10/10 |

Critical: none. High: H1 (two readers on /dev/ttyACM0: confirmed in tools/run-probe.sh:29-31, which starts `usb-log.py capture` while the spec says to run it alongside udump-recv.py;
the dump is split between readers and every CRC/seq check fails - closed, but the run is wasted) and H2 (udump.o MMIO audit bypass, above). Neither affects the safety of the 00a5955 code.

SAFE TO RUN ON THE PHONE: conditional on H1 only (one reader on /dev/ttyACM0: run udump-recv.py by hand started before `fastboot boot`, do NOT use run-probe.sh as is) and starting with `--udump 1`; storage-wise yes (read-only, bounded to the GPT userdata range, fail-closed), H2/M1 are gate-hardening items for later changes, not defects in this code

