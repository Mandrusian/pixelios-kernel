# Review 1: ACPM rails client (`shim/acpm.c/.h`, `railsr`/`rails`), branch `t3/92730390` @ 7f4b4b1

Reviewer: Claude Opus 5.5 (independent; author Sonnet). Date 2026-10-22. No device access was used.
Ground truth: the Google kernel files fetched to `~/pixelios-artifacts/scratch/acpm-src/` (prefix `ipc.c` = `drivers_soc_google_acpm_acpm_ipc.c`,
`ipc.h`, `acpm.c` = `drivers_soc_google_acpm_acpm.c`, `mfd.c` = `drivers_soc_google_acpm_acpm_mfd.c`, `mfd.h` = `include_soc_google_acpm_mfd.h`,
`fw.h` = `drivers_soc_google_acpm_fw_header_framework.h`, `reg.c` = `drivers_regulator_s2mpg10-regulator.c`, `pmic.dtsi`, `gs101.dtsi`) and
`~/pixelios-artifacts/scratch/touch-src/` (`gk_s2mpg10-register.h`, `ftm5_fts.c`, `bj_*.dtsi`). I re-derived everything from those files, not from doc 23.
Tags: *read from source*, *host test observed* (run here), *guess*. Nothing here is *observed on hardware*.

Merge note: `git merge t3/92730390` conflicted in `shim/AGENTS.md` and `docs/HANDOFF.md` (both sides appended sections); resolved by keeping both.

## Verdict summary

| # | Severity | Finding |
|---|---|---|
| C1 | **Critical** (blocks every gates-open run) | `elem_unit = 4` (q_elem_size in words) contradicts the source: q_elem_size is in **bytes**. With the gates opened, the request goes to the wrong slot and the APM executes a stale slot. |
| H1 | **High** | The post-publish undo (restore `front` when the APM's `rear` has not moved yet) races the APM. If it loses, the APM sees q_len-1 pending entries and replays stale commands. |
| M1 | Medium | `rails_describe` prints the raw descriptor only when validation passes. Step 1 therefore shows nothing in exactly the cases it exists to diagnose. |
| M2 | Medium | The fake APM in `acpm_host_test.c` shares the implementation's assumptions (word units, a synchronous APM, the generated INTGR1/INTCR0 macros). It cannot catch C1 or H1, and `ELEM_UNIT 1` (the correct value) makes it fail. |
| M3 | Medium | The vsel ranges are not tied to the regulator source or DT limits. The generator accepts any range (0..63 would pass). |
| M4 | Medium | With the gates open, `railsr` writes (queue slot, front, doorbell, reply rear, INTCR0). The update code is compiled into `railsr` builds, and only an `#ifdef` in `shim.c` keeps it from being called. |
| L1-L6 | Low | Missing `idx == id` and `type == TYPE_QUEUE` checks; `cmd[3]` set to 0; no 1 ms DVDD→AVDD gap and reset not held; no compile-time pins of TYPE/SPEEDY/regs/mailbox offsets; guard widening undetected; a stale reply blocks forever. |

## Q1. Protocol vs the Linux driver

All of these match the source (*read from source*):
- Descriptor layout. `fw.h:36-81` with u32 pointers: `ipc_channels` at +0x08, `ipc_ap_max` at +0x18. `fw.h:97-133`: `struct ipc_channel` = id, bitfield word, owner, type, then `channel_info` (13 u32), then `ap_poll`. That gives rx_rear/front/base at 0x10/0x14/0x18, tx_rear/front/base at 0x28/0x2c/0x30, q_len 0x34, q_elem_size 0x38, ap_poll 0x44, and a stride of 0x48. `acpm.c:29-42` matches.
- Direction. AP→APM requests use the descriptor's `rx_*` (`ipc.c:976-978`) and replies use `tx_*` (`ipc.c:970-972`). `acpm.c:34-39` matches.
- One `q_len` for both queues (`ipc.c:969,975`). Matches.
- Command words. TYPE [11:8] (mask 0xF), REG [7:0], CHANNEL [15:12], FUNC [7:0], UPDATE_VAL [15:8], UPDATE_MASK [23:16], RETURN [31:24], DEST [15:8] (`mfd.h:11-45`). FUNC_READ = 0 and FUNC_UPDATE = 2 (`mfd.h` enum `mfd_func`). TYPE = `i2c->addr` = 0x01 for the PMIC block and CHANNEL = 0 for S2MPG10 (`s2mpg10-core.c:30,58,130-135`; S2MPG11 uses channel 1, `s2mpg11-core.c:55`). `acpm.c:327` and `:473` match.
- Sequence number. Bits [21:16] (`ipc_ctrl.h:31`), range 1..63 and never 0 (`ipc.c:747-750`). `acpm.c:325-327` matches.
- Queue-full checks. `(tx_front+2)%len != rx_front` (`ipc.c:712`) and `(tx_front+1)%len == tx_rear` (`ipc.c:726-729`). `acpm.c:348-350` matches, with "refuse" in place of wait/panic.
- Slot and publish. Slot at `base + size*front`, then `front := (front+1)%len`, then doorbell (`ipc.c:762-772`). Same order.
- Doorbell. `writel(1 << channel->id, intr + APM_INTGR)` with `APM_INTGR = INTGR1 = 0x40` (`ipc.c:410-413`, `ipc.h:110,114`). `acpm.c:365-367,380` matches.
- Reply. Read entries from `rear` up to `front`, then `rear := front` (`ipc.c:426-455`), then `INTCR0 (0x24) = 1 << id` (`ipc.c:458`, `ipc.h:106,116`). `acpm.c:402-409` matches. RETURN/DEST decoding (`mfd.c:40-41`) matches `acpm.c:411-412`.
- Addresses. Mailbox 0x17610000, SRAM 0x2039000 size 0x40000, initdata 0xA000 (`gs101.dtsi:496-499`). Channel 2 (`pmic.dtsi:38`). `soc.json` matches.

Deviations:
1. **Element unit (C1). The author is wrong.** `ipc.c:762` is `memcpy_align_4(tx_ch.base + tx_ch.size * tx_front, cfg->cmd, tx_ch.size)`. `base` is a `void __iomem *`, so the offset is in bytes. `memcpy_align_4(dest, src, n)` copies `n >> 2` words and warns if `n % 4` (`acpm.c:257-271` of the Google tree), so `n` is in bytes. The MFD callers pass `unsigned int command[4]` (`mfd.c:20,66,115,163,214`), and `check_response` copies `size` bytes back into it (`ipc.c:446,468`). If `size` were 16 words, Linux would overrun a 16-byte stack array by 48 bytes on every PMIC call. So for channel 2, q_elem_size = 16 bytes = 4 words is the only consistent reading. Doc 23 lines 64 and 167 ("size is a word count") are wrong, and `soc.json` `"elem_unit": 4` follows them.
2. Channel lookup by descriptor id (`acpm.c:226-233`). Linux indexes `acpm_ipc->channel[channel_id]` with the id that `request_channel` returned (`ipc.c:363`, `:696`), so it implicitly assumes index == id. The author's lookup is safer, but a dump where `idx != id` would mean the firmware differs from what Linux exercises. Require `idx == ch_id` (L1).
3. No check of `type` (d[3]) == `TYPE_QUEUE` = 1 (`fw.h:247`). Linux treats `TYPE_BUFFER` differently (`ipc.c:491`) (L1).
4. `cmd[3] = 0` where Linux puts a millisecond timestamp (`mfd.c:28,124,224`). I *guess* the firmware only uses it for logging; harmless (L2).
5. A non-empty queue is refused. Linux drains replies first (`check_response(channel, NULL)`, `ipc.c:744`) and waits on full. This is stricter. Side effect: a stale reply left by the ABL makes every run refuse BUSY, and no drain option exists (L6).
6. Exactly one reply entry is required (`cnt != 1`). Linux matches by sequence number among several. Stricter; fine.
7. Barriers. Each `try_write32` ends in `dsb sy` (`shim.c:144-152`), and the SRAM is mapped Device (`mmu.c:8,25`). That is at least as strong as Linux's `writel`. Fine.

## Q2. Safety of the writes, gates, undo

- **Allowlist (`acpm.c:64-77`).** Every write is confined to the one slot window, the request front (old or new value only), the reply rear (only after a reply, only the front value just read), and INTGR1/INTCR0 (only `1 << ch_id`). The addresses are *data-dependent*: they come from SRAM words. `acpm_describe` bounds them to the 256 KiB SRAM window, 4-byte alignment, indices < q_len and no overlap among the 6 regions (`acpm.c:220-276`). That is the strongest bound possible without a verified dump.
- **C1 makes the slot window wrong.** With `elem_unit = 4` and a real q_elem_size of 16, the code computes `esz_b = 64`. It writes 16 words at `base + 64*front`, but the APM reads 4 words at `base + 16*front`. For `front != 0` the APM executes whatever stale command sits in its real slot, possibly an old PMIC write from the ABL. That is an unrequested PMIC write, and doc 23's "no PMIC change" claim for the read-only IPC step is false. The extra 12 words also overwrite slots `4f+1..4f+3`, and past the real queue end when `4f+3 >= q_len`. The 6-region overlap check *may* refuse by luck (the 4×-sized buffers likely overlap each other), but nothing guarantees it. Severity Critical as soon as `layout_verified`/`elem_unit_verified` are set to 1, which is exactly what the handoff's plan step (2) says to do. Fix: `elem_unit = 1`, plus a hard `esz_b == 16` requirement for this channel (the MFD protocol is 4 words). Fix the fake APM in the host test the same way. Add a read-only dump of the first `q_len*16` bytes of both queue buffers to `describe`. The ABL's old commands (seq in [21:16], 0x01 in [11:8]) at a 16-byte stride then *verify* the unit on hardware.
- **Gates.** These are fail-closed in every path I traced (*read from source*, and *host test observed*: `refused()` asserts `nw == 0`). `xfer` returns before arming the guard when `ch_id >= 32`, when describe fails, when `!layout_verified`, when `!elem_unit_verified`, when a queue is non-empty, or when a full condition holds (`acpm.c:337-350`). The guard (`g.armed`) is set only at `:356`. `w32` refuses everything while it is unarmed (`:66-67`). `acpm_pmic_update` refuses wrong arguments and `vsel_known == 0` before the pre-read (`:456-463`), refuses an enabled rail or an out-of-range vsel after the pre-read (`:468-471`), and `boards.py` turns any `null` bound into `VSEL_KNOWN 0`. `railsr` with the shipped gates: step 0 only reads, and step 1 stops at `ACPM_W_LAYOUT` with zero writes (`rails_host_test.c:95-99`, *host test observed* PASS).
- **Undo (H1).** `undo()` (`acpm.c:288-312`) restores `front` when `front` is published but the APM's `rear` is unchanged. The APM's sequence is: read front, read the slot, run the SPEEDY transaction, *then* advance rear. "Rear unchanged" therefore does not mean "not taken": the APM can be mid-transaction. If the undo writes `front := rf` in that window, the APM then sets `rear := rf+1` while `front = rf`. To the APM, `(front - rear) mod q_len = q_len - 1` entries are now pending, so it replays every stale slot in the ring: old ABL PMIC writes, possibly buck voltages. The window is exactly the timeout or fault path, the one taken when the APM is slow. The `counter_freq() == 0` case makes `lim = 0`, so the undo runs right after the doorbell, the most likely moment to hit the window. Linux never rolls back a published request. Fix: once the front write has landed (it is read back at `:374`), never write `front` again. On a doorbell fault, poll fault or timeout, report and stop; the request stays valid and the APM will consume it. The `front_old` allowlist entry and the slot restore can then go: an unpublished slot is never read by the APM. Delete the post-publish undo outright. The test `M_IGNORE → undone == 1` encodes the racy behaviour and must change.
- **Reply-rear write.** `rear := sf` comes before the sequence check (`:407-414`). Linux does the same (it drains everything), so it is harmless. Write-fault paths after the doorbell return without further writes. OK.

## Q3. Hardware risk

- **Bit 7 of 0x5C/0x5D.** With TYPE 0x01 and SPEEDY 0, these registers are `S2MPG10_PM_L25M_CTRL = 0x5C` and `L26M_CTRL = 0x5D` (`gk_s2mpg10-register.h:108-109`). Their enable masks are `_MASK(_7)` (`reg.c:275-278`, `ENABLE_MASK_7 = 1<<7`, `register.h:334`). No other regulator descriptor uses these registers (`reg.c`, only `:275-278`), so bit 7 controls only LDO25M/LDO26M (*read from source*). The same offsets in other blocks are different registers: METER 0x0A has `ACC_DATA_CH4_5/6` at 0x5C/0x5D (`register.h:204-205`), and TRIM is TYPE 0x0F (`s2mpg10-core.c:34`). The TYPE value is therefore the critical constant. It is pinned only by the host test's literal `0x0001015C` (my config mutation PMIC_ADDR 15 was caught); add a `_Static_assert` (L4).
- **Loads.** `pmic.dtsi:535-553`: `L25M_TSP_DVDD` (1.60-1.95 V) and `L26M_TS_AVDD` (2.5-3.3 V), subsystem "Touch". Their only consumers are `gs101-common-touch.dtsi:126-137` (vdd/avdd of spitouch). The bluejay dtsi files do not reassign them (grep of `bj_*.dtsi`: no `ldo25`/`ldo26`), and bluejay uses the FTS touch (`bj_gs101-bluejay-touch.dtsi:6`). No charger or thermal effect: the S2MPG10 LDOs are not the charger (*read from source*; the charger is a different chip, *guess* from general knowledge).
- **UPDATE vs Linux.** `s2m_enable` is `update_reg(enable_reg, opmode[id], enable_mask)` with `opmode[id] = enable_mask = 0x80` (`reg.c:77-86,890`). DT `initial-mode SEC_OPMODE_ON` maps to `(3 << 7) & 0x80 = 0x80` (`reg.c:33-71`). The shim's `UPDATE 0x5C, val 0x80, mask 0x80` is byte-for-byte what Linux sends to enable the rail, so the firmware's read-modify-write under mask 0x80 cannot change vsel bits [5:0] (*read from source*).
- **Voltage (OTP default).** Enabling does not set a voltage; Linux sets vsel separately. The `vsel_min/max` gate is the only protection against an out-of-spec default. Values from source: LDO25M is group 4, `700 mV + 25 mV*code` (`register.h:319-320`), so 1.60-1.95 V = codes **0x24-0x32**. LDO26M is group 5, `1.8 V + 25 mV*code` (`register.h:322-323`), so 2.5-3.3 V = codes **0x1C-0x3C**. These are derived by hand, not run (M3: put them in `soc.json` with a generator assertion against the DT µV limits).
- **Order.** `ftm5_fts.c:5395-5422` holds reset low, enables DVDD, waits 1 ms, enables AVDD, waits 5 ms, then 10 ms more before releasing reset. The shim keeps DVDD-then-AVDD (correct, *read from source*) but has no 1 ms gap and does not drive reset low first. Ordinary power-on with reset pulled up is what the IC sees at cold boot anyway (*guess*). Add `wait_ticks(freq/1000)` between the two updates (L3). If the second update fails, DVDD stays on alone: a state Linux also passes through, harmless (*guess*).
- **Worst plausible outcomes.** Gates closed: nothing is written. A secure-only SRAM read would be caught by `try_read32`'s guard; an uncaught SError would hang or reset the shim. Either way, recovery is a reboot. Gates open, current code: C1 or H1 can make the APM execute stale PMIC commands, which can change an unrelated rail and cause an instant hang or reset. That is volatile; a power cycle restores the OTP/ABL state (*guess*: no ACPM path writes OTP). I see no plausible brick path: no fuse, OTP or bootloader storage is reachable through MFD UPDATE/READ on TYPE 1. After the fixes: a correct enable of two touch-only LDOs whose voltage is gated, with the same risk as Linux's own power-up.

## Q4. Audit and tests

- *Host test observed:* `tools/check-all.sh --quick` → **`GATE PASS 8dc2086`** (merged tree; I had to link the gitignored `tools/third_party` into this worktree). It includes `PASS acpm host test`, `PASS rails host test (verified=0/1)`, acpm source self-test 7/7, acpm object self-test 3/3, and `PASS acpm object audit ... 121 stores outside w32`.
- **Nothing was loosened**, with one exception: `_TOUCH_ALLOWED_UNDEF` gained the six `acpm_*` symbols (`test-bluejay-boot.py:1211-1212`), so `touchprobe.o` may now call a write-capable function (`acpm_pmic_update`). That is acceptable because `acpm.o` gets its own source and object audit with a smaller undef set (`try_read32`, `try_write32`, `touch_ticks`, `counter_freq`).
- **What the audit proves.** "The only write path is `w32`, it checks an address-dependent allowlist first, and there are no asm, volatile, casts or absolute pointers." It does **not** prove "writes only these addresses": the allowlist content comes from SRAM data at runtime. The address bound is enforced by `acpm_describe`'s checks and verified only by the host test. State this in the audit's PASS line rather than implying an address proof.
- **My mutations** of `acpm.c`, run against both host tests (*host test observed*), 12 of 13 caught:

| Mutation | Result |
|---|---|
| M1: slot window widened by one word (`slot_hi + 4`) | **MISSED**: the guard built in `xfer` is never tested, and nothing writes there |
| M2: slot index `nf` in place of `rf` (off by one slot) | caught |
| M3: request-queue empty check removed | caught |
| M3b: both empty checks removed | caught |
| M4: doorbell bit 0 in place of `1 << ch_id` | caught |
| M5: doorbell on INTGR0 (0x20) | caught |
| M6: undo ignores APM rear | caught |
| M7: full check removed | caught |
| M8: seq check removed | caught |
| M9: reply read at front | caught |
| M10: vsel_max + 1 | caught |
| M11: TYPE nibble 0xF (TRIM) | caught |
| M12: ap_max bound 64 | caught |
| M13: timeout ×10 | caught |

- **Config mutations** (an edited `board_config.h`):

| Mutation | Result |
|---|---|
| PMIC_ADDR 15 | caught |
| SPEEDY 1 | caught |
| CH_ID 3 | caught |
| RAIL0_REG 0x5B | caught |
| INTGR1 0x20 | **MISSED** |
| INTCR0 0x20 | **MISSED** |
| ELEM_UNIT 1 (the source-correct value) | **fails the suite**: the fake APM hard-codes word units (M2 finding) |

- **Fixes.** Pin `0x40`/`0x24`/`0x2c` (`ipc.h:106-116`) and the DT addresses as literals in the test or as `_Static_assert`s in `acpm.c`. Make the fake APM byte-unit. Model an asynchronous APM that consumes the request after N polls and is mid-transaction during the timeout. Factor the guard construction into a function the test can inspect (catches M1).

## Q5. First hardware step

`tools/probe-run --touch-probe=railsr --label acpm-describe` with the shipped `soc.json` (layout_verified 0, elem_unit_verified 0, vsel null):
- `build.sh` defines `TOUCH_PROBE` and `TOUCH_RAILS`. It does not define `TOUCH_PROBE_RUN` (no `run`) or `TOUCH_RAILS_WRITE` (`build.sh` diff).
- `shim.c` runs `touch_probe_dump()` (existing, read-only), then `touch_probe_rails(0)` (reads only), then `(1)`, which refuses at the layout gate before arming any write. *Host test observed*: `nw == 0`, `nbell == 0` with the real generated macros.
- The reads are the APM SRAM words Linux itself reads (`ipc.c:952-978`) and mailbox INTSR0. INTSR0 is not read-to-clear: Linux clears it explicitly via INTCR0 (`ipc.c:536-541`).

Conditions: the build log shows `-DTOUCH_RAILS` and neither `-DTOUCH_RAILS_WRITE` nor `-DTOUCH_PROBE_RUN`; the `soc.json` gates are unchanged; the device check passes as usual. Caveat (M1, not a safety issue): if describe refuses (for example `ESZ`, `OVERLAP` or `QBUF`, which the wrong `elem_unit` can trigger), the run prints no descriptor words. Fix M1 first if you want the dump to be useful in that case.

The handoff's intermediate step, "set both verified flags to 1 and rerun `railsr`", is **a write step**, not a read step. It falls under STEP 2's conditions.

## Required before any gates-open run (`railsr` with flags set, or `rails`)
1. C1: `elem_unit = 1` from source (`ipc.c:762`, Google `acpm.c:257-271`, `mfd.c` `command[4]`), require `esz_b == 16`, fix doc 23 lines 64 and 167 and the fake APM, add the queue-buffer dump to describe, and confirm the 16-byte stride on a dump before setting `elem_unit_verified`.
2. H1: remove every write to `front` after it is published; no undo after the front write.
3. M3: set the vsel ranges to LDO25M 0x24-0x32 and LDO26M 0x1C-0x3C (derivation above), with a generator check against the DT µV limits and the S2MPG10 MIN/STEP values.
4. M1: print the raw descriptor whatever the validation result, and compare the dump with `fw.h:97-133` before setting `layout_verified`. Also require `idx == ch_id` and `type == 1` (L1).
5. L4: `_Static_assert` PMIC_ADDR == 1, SPEEDY == 0, rail regs 0x5C/0x5D, INTGR1 0x40, INTCR0 0x24, CH_ID 2.
6. Re-review the fixes by a model family other than the author's (AGENTS.md rule 6).

STEP 1 (railsr dump, gates closed): SAFE: with the shipped soc.json gates (layout_verified 0, elem_unit_verified 0, vsel null) and a build without TOUCH_RAILS_WRITE/TOUCH_PROBE_RUN, the path only reads APM SRAM and mailbox INTSR0 through fault-guarded try_read32, and step 1 refuses at the layout gate before any write (host tests: 0 writes, 0 doorbells).
STEP 2 (rails write): NOT SAFE: requires C1 fixed (elem_unit = 1 bytes per ipc.c:762/memcpy_align_4, esz_b == 16, confirmed by a queue-buffer dump), H1 fixed (no front rollback after publish), vsel ranges set to 0x24-0x32 / 0x1C-0x3C and generator-checked, test fake corrected, and a re-review; this also applies to rerunning `railsr` with the verified flags set.
