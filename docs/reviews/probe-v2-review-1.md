# Probe v2 review 1 (read-only audit)

Scope: `git diff 3b88e02..HEAD` in `<worktree>` (branch `t3/cb4d40d7`, HEAD `fda0328`).
Reviewer touched nothing except this file. Scratch work is in `/tmp/rev/` (copies only). No adb/fastboot, no sudo, no commit.
Observed = I ran it or read the exact lines. Guess = marked UNSURE or "assumes".

## Verdict table

| # | Requirement | Verdict | One line |
|---|-------------|---------|----------|
| 1 | Only NOP OUT / TUR / READ CAP(10) / READ(10), device-to-host or none | **PASS** for the code as written | One doorbell store (`ufs.c:186`); every reachable (type, CDB) pair is in the allowlist. Enforced by convention + a regex linter only, not at the doorbell (see H2, M1). |
| 2 | Doorbell / UTRLCLR hygiene, no stale slot, nothing left in flight | **FAIL** (functional; storage-safe) | Stale `IS.UTRCS` is never cleared on the normal completion path, so every command after the first successful one is falsely "completed" and aborted (H1). |
| 3 | A5/A6 window never overlaps anything live, fails closed | **FAIL** | Reserved list silently truncated at 16 (fail-open), DTB/initrd/memreserve/ABL buffers never excluded, wrapped reserved ranges ignored, one input hangs the picker (H3, M2, L1, L2). Empty map, no-bank and malformed-DT cases do fail closed. |
| 4 | RAM log + pager | **PASS** (one UNSURE) | Fuzzed against an oracle under ASan/UBSan, exact 32768 fit OK. Only unguarded pager recursion on a fault (L3) and the `wfe` wake assumption (L4, UNSURE) remain. |
| 5 | `fdt_collect_memmap` | **FAIL** (one item) | Reads are bounded and 64-bit cells are correct, but `FDT_MAX_BANKS=8` / `FDT_MAX_RESERVED=16` overflow is silently dropped with no flag (same root cause as H3). |
| 6 | Real build/test output | **PASS** | All commands exit 0; output below. |
| 7 | Break the safety checker | **FAIL** (checker is bypassable) | 14 of 16 spelling variants passed `check_ufs_safety`, including 7 for WRITE(10), 4 for the UPIU write flag, 3 for query-write; all 16 compile (H2). |

Storage-write summary: I found **no path in the current source** that can put a SCSI WRITE, UNMAP, FORMAT, UPIU QUERY, or host-to-device DD onto the doorbell. The weakness is that nothing *prevents* one from being added later (H2).

---

## Requirement 1 - every place a UTRD/UPIU is built or submitted

Single doorbell write in the whole tree: `ufs.c:186` `ufs_wr(REG_UTRLDBR, 1)` inside `upiu_submit()` (`ufs.c:137-216`).
Confirmed in the linked binary: `shim.elf` has exactly one store of 1 to `0x14700058`, at `0x5e84` (`mov x23,#0x58; movk x23,#0x1470,lsl#16; str w0,[x23]` in `upiu_submit.isra.0`). `grep` for `UTRLDBR|UTRLCLR|UFS_BASE|ucd|utrl|upiu_submit|scsi(` outside `shim/ufs.c` finds nothing (only the checker and boards.py).

Bytes that reach the doorbell come only from `upiu_submit` itself:
- `cmd[0] = type` (`:151`), `cmd[1] = dir==READ ? 0x40 : 0x00` (`:152`), `cmd[2]=lun`, `cmd[3]=++tag`, `cmd[12..15]=len`, `cmd[16+i]=cdb[i]` (`:159-160`).
- UTRD `dw0 = CT=1 | DD=(dir==READ ? 2 : 0)<<25 | I=1` (`:165`). DD=2 is device-to-host; host-to-device (DD=1) cannot be produced.

Call sites (every one; line = where `upiu_submit` is reached):

| Site | Via | type byte | CDB opcode | dir | Notes |
|------|-----|-----------|-----------|-----|-------|
| `ufs.c:230` `scsi()` | caller supplies cdb/dir | 0x01 | whatever caller passes | caller | callers below |
| - `:238` `read_blocks()` | local `cdb[10]={0x28,...}` | 0x01 | **0x28** READ(10) | READ | LBA/count are operands only |
| - `:342` `test_ufs()` | `static const tur[6]={0x00}` | 0x01 | **0x00** TUR | NONE | up to 3x |
| - `:349` `test_ufs()` | `static const cap10[10]={0x25}` | 0x01 | **0x25** READ CAP(10) | READ | |
| - `:356` `test_ufs()` | `static const rd10[10]={0x28,..,2,0}` | 0x01 | **0x28** | READ | |
| `ufs.c:246` `read_blocks_dma()` | local `cdb[10]={0x28,...}` | 0x01 | **0x28** | READ | callers `:588` (LBA 0, 2 blocks), `:612` (GPT entry blocks, 1 block each, `elba+b`) |
| `ufs.c:528` `ufs_run_attempt` | cdb=NULL, len=0 | **0x00 NOP OUT** | none | NONE | |
| `ufs.c:538` `ufs_run_attempt` | `tur[6]={0x00}` | 0x01 | **0x00** TUR | NONE | up to 3x |
| `ufs.c:550` `ufs_run_attempt` | `cap10[10]={0x25}` | 0x01 | **0x25** | READ | 8 bytes |
| `ufs.c:576` `ufs_full_sequence` loop | `cap10[10]={0x25}`, `lun=0..3` | 0x01 | **0x25** | READ | `uint8_t lun < 4` |

Confirmed: the reachable set is exactly {type 0x00; type 0x01 with CDB 0x00/0x25/0x28}; directions NONE or READ only. There is no table/function-pointer dispatch; the CDB arrays are `static const` or locals whose byte 0 is a literal and never reassigned.

`test_ufs()`/`scsi()`/`read_blocks()`/`fs_type()`/`list_partitions()` (the v1 path) are **linked into the bluejay image** (`nm`: `T test_ufs` at 0x6520) but **never called** on bluejay: `shim_main` calls the `noreturn` `bluejay_probe` first (`shim.c:646-647`), and the only `bl` to `test_ufs` count in `shim.elf` is 0. They are only live for the sweet build (`shim.c:662`).

CDB overflow: `ufs.c:159-160` copies `cdb_len` bytes into `u->cmd[16+i]` with no bound. All callers pass `sizeof` of a <=10 byte array, so no overflow today. `cmd[]` is 512 bytes so even a mistaken len would stay in the struct, but a UPIU CDB field is only 16 bytes (L5).

`upiu_submit` accepts `enum upiu_type`, but a C enum is just an int: any caller can pass `(enum upiu_type)0x16`. The `_Static_assert`s (`:102`, `:116`) only assert the enumerator values, they do not constrain arguments. The comment at `:93-97` ("a write cannot be expressed through this interface at all") is true for the *direction* flags and false for the CDB bytes and the type.

## Requirement 2 - doorbell and UTRLCLR handling

What is good (observed in source):
- Slot is fully rebuilt before every ring: `zero(utrd)`, `zero(u)` then all fields (`:145-146`). A stale CDB/UTRD from a previous command cannot be re-rung.
- Takeover refuses unless `UTRLDBR==0` first (`:759`), sets the list base, verifies it was accepted, clears stale doorbells with `UTRLCLR=~stale` and waits for `UTRLDBR==0` (`:486-516`).
- Every `rc==1` (timeout) path in `ufs_run_attempt` (`:531-535, 541-545, 553-557`) and `ufs_full_sequence` (`:579-583, 590-594, 612-616`) calls `ufs_slot_clear`, which waits up to 1 s for DBR bit 0 to clear.
- A torn UTRD/UCD (host fetching while we re-zero it) can only contain zeros or final values, none of which is a write opcode, so the worst case is a NOP/TUR/READ.

Problems:
- **H1** stale `IS.UTRCS` (below).
- `upiu_submit` timeout (`:202-204`) and IS path (`:197-199`) write `UTRLCLR` and return without waiting for the doorbell bit to drop. v2 callers follow with `ufs_slot_clear` on timeout, but the IS path has no such wait, and `ufs_slot_clear`'s own failure (`:476-477`, "dbr stuck") is ignored by every caller (`:533, 543, 555, 581, 592, 614`). The next attempt's takeover would catch a stuck bit (`:504-508`), but the final attempt can end with a command still owned by the host while the pager runs for ~10 minutes with `UTRLRSR=1` and `UTRLBA` pointing at shim memory. Only a read/NOP can be outstanding, so this is not a storage risk; the host could DMA into shim `.bss`/the alt window late.
- Tag is not checked on the response (`:209-215` only checks response type), so a late response from a timed-out command can be accepted for the next one. Low.

### H1 - stale IS.UTRCS makes every command after the first falsely complete (High, functional)

`ufs.c:188-206`: the loop breaks on `!(dbr & 1)` (`:191-192`) without clearing `IS`. UTRD has `I=1` (`:165`), so the host sets `IS.UTRCS` (bit 0) on every completion, independent of `IE`. `IS` is only cleared inside the `is & 1` branch (`:199`) and once per attempt in `ufs_takeover_list` (`:512-514`). `NOTES.md:8` says "the other is cleared/consumed"; the DBR path does not clear it.

Sequence on a host where the NOP OUT works (the case the probe is trying to reach):
1. NOP OUT completes via DBR. `IS.UTRCS` stays 1.
2. TUR try 1: first loop iteration reads `dbr=1` (host has not completed yet) and `is&1=1` -> treated as complete; `UTRLCLR=~1` aborts the live command; `ocs = utrd->dw2 & 0xff = 0x0f` (the value `:166` pre-loaded) -> `rc=2`, printed as `tur rc=00000002 ocs/st=00000F00`. That is indistinguishable from "host never fetched the UTRD". The false path clears `IS`.
3. TUR try 2 is then correct (IS clear), completes via DBR, leaves `IS.UTRCS` set again, and the cycle repeats: CAP(10) false-completes with `rc=2`, and since `rc==2` is not `1`, `ufs_run_attempt` returns 1 (`:558-560`) and declares the attempt a winner.
4. In `ufs_full_sequence` the LUN 0..3 loop alternates correct/false results (`:575-587`), and the GPT-entry reads only treat `== 1` as failure (`:612`), so a false `rc=2` leaves `entries[]` zero/partial and the GPT listing silently wrong.

Confidence: high that the code does this given the standard UFSHCI IS semantics; UNSURE whether this particular Exynos host sets `UTRCS` identically (the v1 run never got past the first command).
Suggested fix: write-1-to-clear `IS` (`ufs_wr(REG_IS, ufs_rd(REG_IS))`) immediately before ringing the doorbell and again after every completion; treat IS only as a hint and require `DBR==0` (or `IS.UTRCS && DBR==0` with a grace re-read) before reading OCS; wait for DBR bit 0 to clear after the `UTRLCLR` in the IS/timeout paths; return distinct codes for "OCS still 0x0F" (host never fetched) vs OCS error; check `resp[3]` against the tag.

## Requirement 3 - A5/A6 alternate DMA window

Placement logic: `pick_alt_dma_address` (`:725-741`) -> `find_dma_spot` (`:705-721`), called at `:806` only when `have_map` and `placement_questionable()` (`:798-800`).

Verified good (read + host harness `/tmp/rev/alt_harness.c`, which compiles lines 645-741 of the real file):
- Window need is `0x8000`; layout is UTRL `+0x0` (0x400 B), UCD `+0x1000` (0x410 B), data `+0x2000` (0x2000 B), entries `+0x4000` (0x4000 B) = ends exactly at `+0x8000` (`:813-816`). Max index `entries + 3*4096 + 4096` = `+0x8000`: inside.
- Alignment: 2 MiB-aligned window, so UTRL (1 KiB) and UCD (128 B) alignment hold.
- Image: `_head.._end` includes `.text/.rodata/.data/.bss`, the 32 KiB boot stack (`shim.ld`: `_stack_top` before `_end`), the 8 secondary stacks, and the MMU `pt_pool` (in `.bss`, `mmu.c:60`; MMU is not even enabled in the bluejay path). The window starts >= `_end + 8 MiB`.
- Empty/absent map: `!have_map` -> A5/A6 never added (`:797`); empty map -> `pick_alt_dma_address` returns 0 -> "no plain-DRAM gap found" (harness case E: `alt=0`). Banks below the image return 0 (`start + need > end`).
- A normal two-bank map picks `0x100000000` (harness case A), skips a *listed* reserved range at the bank start (case B -> `0x104000000`).
- Both unprotected-write targets are the CPU storing the UTRL/UCD and the device writing READ data into `+0x2000`/`+0x4000`; neither can touch UFS storage.

Failures:
- **H3** reserved list truncated, fail-open (below) and **M2** nothing excludes the DTB blob, initrd, `/memreserve/` entries or ABL's own buffers, and the below-4 GiB fallback pass is allowed.
- **L1** `reserved_hit` ignores a range whose `base+size` wraps (`:661-662`, `e > b` false).
- **L2** `find_dma_spot` can loop effectively forever: a bank at e.g. `0xFFFFFFFFFFF00000` size `0xF0000` makes `start = (base+0x1fffff)&~0x1fffff` wrap to 0 (`:710`), so `a` walks from 0 in 2 MiB steps (`:714`). Harness case F did not return in 20 s (`timeout` exit 124). Requires a corrupt DT, so it is only a hang, not a storage risk.

### H3 - reserved/bank arrays silently truncate, so the alt window can land on a dropped carve-out (High, RAM-corruption/abort risk; not storage)

`fdt.c:361-371` drops entries past `FDT_MAX_BANKS` (8) / `FDT_MAX_RESERVED` (16) with no flag, count, or failure; `fdt_collect_memmap` returns 1 (`fdt.c:376-399`), and the host test `fdt_host_test.c:388-415` asserts the silent drop as intended behaviour. Real GS101/Pixel `reserved-memory` trees can have more than 16 children (I could not verify the count for this phone: no dump is in the repo, UNSURE). Harness case C: a carve-out at the start of the high bank that is the 17th entry is invisible, and `pick_alt_dma_address` returns exactly that address (`0x100000000`).
Failure scenario: the real list has >16 entries, one of them (no-map, TZ/S2MPU/GPU/modem) at the bank start; the CPU store to the UTRL at `alt` raises an external abort/SError (`exc_handler` slot 7 is only recoverable under `probe_guard`, which is not set here) -> blue panic -> reset after the pager; or the device DMA corrupts a live secure carve-out.
Fix: add `overflow` to `struct fdt_memmap` (set when anything is dropped), make `fdt_collect_memmap` return 0 / `have_map=0` on overflow so A5/A6 are skipped, and/or raise the arrays (e.g. 64 reserved, 16 banks; the struct is ~0.8 KiB on the stack).

### M2 - window is not checked against the DTB, initrd, `/memreserve/`, or ABL buffers (Medium)

`test_ufs_bluejay` has `dtb` and the live FDT but `find_dma_spot` only knows `image_end`. Not excluded: `[dtb, dtb+totalsize)`, `chosen/linux,initrd-{start,end}`, the FDT header memory-reservation block (`/memreserve/`, never parsed by `fdt_collect_memmap`), `reserved-memory` children with only `size`/`alloc-ranges` (dynamic, no `reg`), and wherever ABL staged the boot image (kernel 272 KiB + 97 B ramdisk + DTB, UNSURE of the address). Pass 1 (>= 4 GiB) usually dodges these; pass 0 (`:727-739`) falls back to the first free 2 MiB slot above `_end + 8 MiB` in the low bank, where those objects most likely live, and is also the below-4 GiB region the code itself calls S2MPU-suspect (`:694-695`).
Failure scenario: a machine with no usable high bank (or the high bank rejected) falls to pass 0 and DMA/CPU writes 32 KiB over ABL's staged boot image or DTB. The shim has already walked the DTB, so the probe survives, but the dump is garbage if the FB info/DTB is reused afterwards.
Fix: pass the dtb/initrd/memreserve ranges into `find_dma_spot` and skip them; drop pass 0 (only try >= 4 GiB, matching the stated S2MPU rationale); add >= 8 MiB margin around each excluded range.

## Requirement 4 - RAM log and pager

Verified (observed output, not just reading):
- `shim/out/ramlog_host_test` PASS. Additionally `/tmp/rev/ramlog_fuzz.c` (own oracle that keeps a deque of strings, evicts whole lines, truncates at 95 chars) built with `-fsanitize=address,undefined`: 300 rounds of random line lengths 0..200 across four length mixes, compared line-by-line against `ramlog_lines`/`ramlog_copy_line`; plus an exact-fit case: 341 x 95-char lines + one 31-char line = 32768 bytes exactly -> 342 lines retained, no loss, no off-by-one; one more line evicts correctly. No ASan/UBSan report.
- Bounds: `idx()` subtracts at most once (`head < SIZE`, `pos+len < SIZE`... sum `< 2*SIZE`), `len <= 95`, `while (used+len+1 > SIZE) drop_oldest_line()` always terminates (a drop removes >= 1 byte or empties the log). A line longer than the whole buffer is capped at 95 (`ramlog.c:76`), tested at `ramlog_host_test.c:84-95`.
- Whole-line eviction: `drop_oldest_line` advances to just past the first `\n`; fuzz shows only whole retained lines. Contract caveat: a stored line containing `\n` (not possible from any current caller; GPT names are sanitized to 32..126 at `ufs.c:626`) would split into two lines.
- Page math: `FB_H=0x960 (2400)`, `MARGIN=24`, `CH_H=48` -> `pager_content_rows = (2400-48)/48 - 1 = 48`; footer at row 48 -> y0 = 2328, bottom 2376 <= 2400, so it fits. `rows==0` gives `pages==0` -> "log empty" branch (no divide by zero: `ramlog_page_count` guards it). `first=(p-1)*rows`, `count` clamp OK. Columns visible: `x0+24 <= 1080` -> 44 columns; longer lines are clipped (same as live).
- 64-bit timer: `PAGER_HOLD_S*freq = 12*24,576,000`, total `600*freq = 1.47e10`: no 32-bit truncation (`counter_freq()` returns `uint64_t`). The ~10 min bound is by a counter (`remaining -= 12`), not wall clock, so it terminates after 50 pages even if `wait_ticks` returned early.
- Pager never calls `con_*` (no self-logging); `reset()` logs `RESET` before replay, so the panic lines are in the last page.
- Reentrancy from the panic path: `exc_handler` calls `con_str -> ramlog_line` with no lock. If a fault happens inside `ramlog_line`, state after the interruption is at worst `head` advanced and `used` not yet reduced (`ramlog.c:64-65`) or `used` not yet incremented (`:84`): both stay in-bounds (everything goes through `idx()`), so memory-safe, the log may show a stale fragment. Secondary CPUs never call `con_*` (`secondary_main` only fills `cpu_report`), so no cross-CPU race in the normal run.

Findings:
- **L3** (Low) no recursion guard: a fault inside `pager_replay` (or anything in `reset()` after `con_str`) re-enters `exc_handler -> reset(40) -> pager_replay`. Each level costs a few hundred bytes of the 32 KiB stack (`shim.ld`: `_bss_end + 0x8000`); a *persistent* fault would walk the stack down into `.bss`/`ramlog_buf` (writes to Device memory with the MMU off are not trapped). Fix: static `in_reset` flag; if already set go straight to `smc(PSCI_SYSTEM_RESET)` with a short bounded wait.
- **L4** (Low, UNSURE) `wfe` wake assumption (`console.c:37-53`): wake relies on the CNTKCTL_EL1 event stream (`EVNTEN`, `EVNTI=13`, ~0.67 ms at 24.576 MHz). The `|=` does not clear previous `EVNTI` bits, so a non-zero ABL value yields a different (still working) period. If the stream never fires, `wfe` sleeps indefinitely (IRQs are masked, and only masked-interrupt/`SEV` events wake `wfe`); the symptom would be the pager stuck on page 1 with no reset, which is benign for storage (user holds power). Evidence it works: the previous run printed the UFS section after several `delay_ms(3000)`/`wait_ticks` waits that already depended on this (`GATE0_RESULTS.md:14-15`). The new part is only the length of the waiting (10 min vs the previous 120 s that was not recorded as observed to complete). Fix: after enabling, read back `cntkctl` and fall back to a spin on the counter when `EVNTEN` did not stick, or issue `sev` first and use a bounded `wfe` loop.
- **L6** (Info) `fmt_dec` into `char d[10]` (`console.c:303,...`) needs 11 bytes for a 10-digit value; unreachable (pages <= ~800).
- **L7** (Info) logged lines are `label + " " + text` while the live screen aligns text at column 8 (`LABEL_COLS`), so the pager's yellow/white split (first 8 characters yellow) does not match the label boundary for labels shorter or longer than 7 characters. Cosmetic.
- The watchdog might fire during the 10 minute pager; not observed either way (UNSURE). A watchdog reset is harmless.

## Requirement 5 - `fdt_collect_memmap` (`fdt.c:309-399`, `fdt.h:4-25`)

- Bounds: relies on `fdt_walk` (`fdt.c:145-247`, untouched by this diff): total size <= 2 MiB, token/property/`nameoff` bounds checked, `val + len > fdt_end` rejected. The new reg loop `for (off = 0; off + stride <= len; off += stride)` (`:355`) stays inside the property. `copy_name` truncates to 19 chars + NUL. Node depth: `/reserved-memory` children are exactly `depth==3` (`names[1]=="reserved-memory"`).
- 64-bit cells: `read_cells` handles 1 and 2 cells; `fdt_reg_stride` returns 0 for 0 or >2 cells so such nodes are skipped (never mis-read). `no-map` is applied whatever the property order (`:343-351`; test at `fdt_host_test.c:315-386`).
- Failure return: `fdt_walk` returns 0 (malformed), or 2 (complete); callbacks always return 0, so `rc != 0` is correct. `mm` is zeroed first (`:378-390`), so `report_placement` on an unfilled `mm` reads zeros, not stack garbage.
- **FAIL item**: array limits (H3). Also not collected: `/memreserve/` block, reserved-memory nodes without `reg`, `status="disabled"` (counted anyway, conservative), >2-cell addresses.

## Requirement 6 - real output

```
$ cd <worktree> && shim/build.sh --board bluejay --avb      # EXIT=0
PASS ramlog host test: page count, copy, wraparound, overflow, whole-line eviction
... (8 "self-test caught:" lines per run) ...
PASS ufs safety self-test: 8/8 known-bad sources rejected
PASS header v4 (raw): kernel=278528 ramdisk=97; host tests & safety checks passed
PASS ufs safety self-test: 8/8 known-bad sources rejected
PASS header v4 (lz4): kernel=26227 ramdisk=97; host tests & safety checks passed
-rwxrwxr-x 278528 out/Image
-rw-rw-r-- 67108864 out/shim-boot-avb.img
-rw-rw-r--    286720 out/shim-boot.img
-rw-rw-r-- 67108864 out/shim-boot-lz4-avb.img
-rw-rw-r--  36864 out/shim-boot-lz4.img

$ python3 tools/test-bluejay-boot.py shim/out/shim-boot-avb.img                          # EXIT=0
PASS ufs safety self-test: 8/8 known-bad sources rejected
PASS header v4 (raw): kernel=278528 ramdisk=97; host tests & safety checks passed

$ cd shim && ./out/host_test; ./out/ramlog_host_test; ./out/fdt_host_test
(host_test: no output, exit 0)
PASS ramlog host test: page count, copy, wraparound, overflow, whole-line eviction   (exit 0)
(fdt_host_test: no output, exit 0)
```
Notes: `host_test` and `fdt_host_test` print nothing on success (only the exit code shows pass). `NOTES.md:10` says `ramdisk=98` for the same image; I observed `ramdisk=97` (doc nit). `git status` is clean after the run (`shim/out/` is gitignored).

## Requirement 7 - try to break `check_ufs_safety`

Method: `/tmp/rev/bypass.py` copies `shim/ufs.c` to `/tmp/rev/ufs_<name>.c`, applies one edit, and runs the real `check_ufs_safety` from `tools/test-bluejay-boot.py` on the copy (original untouched). Each variant was also compiled with `aarch64-none-elf-gcc -fsyntax-only` using the real build flags: **all 16 compile**. Control (unmodified copy) passes.

| Variant | Edit (in the copy) | Checker |
|---------|--------------------|---------|
| W1 | `rd10` first byte `'*'` (0x2A WRITE(10)) | **PASSED** |
| W2 | `{ 0x2b - 1, ...}` | **PASSED** |
| W3 | `#define OPC_X 0x2a` then `cdb[0] = OPC_X;` in `read_blocks` | **PASSED** |
| W4 | compound literal `(const uint8_t[10]){ 0x2a, ...}` passed to `scsi()` | **PASSED** |
| W5 | `cdb[0x0] = 0x2a;` | **PASSED** |
| W6 | `*cdb = 0x2a;` | **PASSED** |
| W7 | `cdb[0] = 0x28 \| 0x0 + 2;` | **PASSED** |
| F1 | `#define FLG_W 0x20` and `cmd[1] = ... : FLG_W` | **PASSED** |
| F2 | `u->cmd[0x1] = ... : 0x20;` | **PASSED** |
| F3 | `uint8_t *hdr = u->cmd; hdr[1] = ... : 0x20;` | **PASSED** |
| F4 | `... : (uint8_t)(UPIU_COMMAND << 5)` | rejected (literal `5`) |
| F5 | extra `*(uint32_t *)u->cmd \|= 0x2000;` (byte 1 = 0x20) | **PASSED** |
| Q1 | add `UPIU_QRY = 22` to the enum and build a query UPIU for it (`cmd[5]=0x81, cmd[12]=0x07`) | **PASSED** |
| Q2 | call site `(enum upiu_type)0x16` | rejected (0x16 regex) |
| Q3 | `u->cmd[12] = type == UPIU_COMMAND ? len >> 24 : 0x08;` (query opcode Write Descriptor) | **PASSED** |
| Q4 | `u->cmd[5] = 0x81; u->cmd[13] = 0x06;` (query write function / Toggle Flag) | **PASSED** |

Why: scans (a)-(c) only match a literal in `name[N] = {`, `x[0] =`, or `cmd[16+i] =`, and only when the right-hand side is *one bare integer literal* (`_bare_literal`); (d)/(f) only fire when the exact index token is `1`/`0` (so `0x1`, aliases, word stores bypass them) and only on integer literals; nothing inspects `cmd[5]`/`cmd[12]` (the QUERY function/opcode bytes) or the `type` value flowing into `cmd[0]`; (e) catches only the hex spelling `0x16`. The check also reads only `shim/ufs.c`, not `console.c`/`shim.c`/new files. The self-test's 8 cases are all the spellings the checker was written against.

---

## Findings ranked

### Critical
None. I found no path that can write to UFS storage in the current source.

### High

**H1 - stale IS.UTRCS aborts/falsely completes every command after the first** - `ufs.c:188-206` (clearing only at `:199` and `:512-514`).
Scenario and fix above. Priority 2 (hang/misdiagnosis), not storage.

**H2 - the write-protection is a regex lint plus convention; it is bypassed by 14 of 16 spellings and nothing is checked at the doorbell** - `tools/test-bluejay-boot.py:119-193`, `ufs.c:93-117,137-216`.
Failure scenario: a later edit (or a delegate's) adds a CDB via a macro/enum/char/arithmetic/compound-literal, or a query UPIU via an enum value spelled in decimal, the build still prints `PASS`, and the shim would send it on the next phone boot. `docs/SAFETY.md:30-31` states "A host check fails the build if a write opcode appears"; this holds only for literal spellings, and `ufs.c:93-97` ("cannot be expressed ... at all") is wrong for CDB bytes and the type.
Fix (cheap, non-bypassable): in `upiu_submit` immediately before `ufs_wr(REG_UTRLDBR, 1)` (`:186`) verify the *built* buffers, not the source: refuse (return a new code, never ring) unless `u->cmd[0] <= 1`, `u->cmd[1]` in {0x00,0x40}, `((utrd->dw0 >> 25) & 3) != 1`, and for type 1 `u->cmd[16]` in {0x00,0x25,0x28} with `cdb_len <= 16`, and for type 0 `cdb_len == 0`. Keep the Python scan as a second layer but extend it to resolve macros or at least ban `#define`/enum values for UPIU bytes; also scan every file that includes UFS register access. Consider compiling out the unreachable v1 path on bluejay (`#if !BOARD_BLUEJAY` around `test_ufs`, `scsi`, `read_blocks`, `fs_type`, `list_partitions`) to shrink what must be audited.

**H3 - FDT arrays silently truncate; A5/A6 window can land on an unseen carve-out** - `fdt.c:361-371`, `fdt.h:4-5`, `ufs.c:806`. See above.

### Medium

**M1 - `upiu_submit` has no allowlist or length check (same root as H2, but also a crash path)** - `ufs.c:137-160`. `cdb_len > 16` spills past the 16-byte UPIU CDB field (`cmd[]` is 512 B so it stays in the struct). Fix is part of H2's pre-doorbell gate.

**M2 - alt window not excluded from DTB/initrd/memreserve/ABL buffers; pass 0 fallback below 4 GiB** - `ufs.c:705-741`. See above.

### Low

- **L1** `reserved_hit` ignores a reserved range whose end wraps (harness case D: `size=~0` -> overlap not detected) - `ufs.c:660-662`. Treat `e < b` as "to the top of memory".
- **L2** `find_dma_spot` effectively non-terminating on a malformed top-of-address-space bank (harness case F, 20 s timeout) - `ufs.c:709-714`. Reject `start < base` after rounding.
- **L3** No re-entrancy guard on `reset()/pager_replay()`; a persistent fault inside the pager recurses through `exc_handler` until the stack overruns - `shim.c:336-348`, `console.c:324-366`, `shim.c:405-422`.
- **L4** `wait_ticks` `wfe` relies on the event stream with no fallback; `EVNTI` is OR-ed, not set (UNSURE it fires; the same code already ran in the previous run) - `console.c:37-53`.
- **L5** `cdb_len` copy loop unbounded - `ufs.c:159-160` (covered by the H2 gate).
- **L6** `fmt_dec` into `char d[10]` is one byte short for 10-digit values - `console.c:303-310` (unreachable).
- **L7** Pager label split differs from the live layout; long lines clip at 44 columns in both - cosmetic.
- **L8** Late response of a timed-out command can satisfy the next command (response tag never compared with `cmd[3]`) - `ufs.c:207-215`.

## Not verified (assumptions)

- Exynos IS.UTRCS behaviour, UTRLCLR semantics, and S2MPU coverage: no hardware access, and the earlier run only reached the first command.
- Number and location of `reserved-memory` entries on this phone and where ABL stages the boot image: no live DTB in the repo (docs say dumps stay out of git).
- Event stream firing and watchdog behaviour for a 10 minute wait: only the earlier short waits are evidence.
