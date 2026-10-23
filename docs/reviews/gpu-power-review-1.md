# Review 1: shim --gpu-power (gpupower.c), uncommitted tree on 1c15f0c (2026-10-23)

Reviewer: Claude Opus 5.5 (adversarial, read-only). Nothing was run on hardware. Host-only checks I ran: `gpupower_host_test.c` built
into /tmp (PASS); `gpupower_source_selftest` (16/16) and the object audit + self-test on the existing `shim/out/gpupower.o` (3/3, 32 stores),
loaded read-only from `tools/test-bluejay-boot.py`; 7 extra source mutations of my own (results under F9). All kbase line numbers below
were re-read in `~/pixelios-artifacts/refs/kbase-bluejay/pm_driver.c`.

## Verdict: GO-WITH-FIXES (NO-GO for SHADER_PWRON until F1 is resolved)

The TILER_PWRON -> L2 -> L2_PWROFF part is a faithful copy of kbase JM (all cited lines check out, see the end) and it is well fenced:
zero-write refusal, a 4-entry allowlist, fault-guarded accesses, bounded waits, and a writer audit that holds. Before the first hardware
run: resolve F1 (or drop SHADER_PWRON/PWROFF from the first run), fix F2 and F3, and correct the doc claims in F6. The rest is M/L hygiene.

## Findings

**F1 (H) kbase does not always write SHADER_PWRON: on Valhall it can replace it with the "dummy job" workaround, and the shim never checks this.**
`kbase_pm_invoke` (pm_driver.c:644-647): `if (kbase_dummy_job_wa_enabled(kbdev) && action == ACTION_PWRON && core_type == KBASE_PM_CORE_SHADER
&& !(flags & LOGICAL_SHADER_POWER)) kbase_dummy_job_wa_execute(kbdev, cores); else kbase_reg_write64(...)`. Also at GPU power-on
(pm_driver.c:3795-3799) with the LOGICAL_SHADER_POWER flag. GPU_ID 0x92020010 decodes to arch 9.2, product major 2. That is tBEx (Mali-G78)
r0p1. That decode is mine, a *guess* until it is checked against kbase `gpu_id` macros. The rest is also a *guess* from memory of upstream kbase:
the workaround is needed when the `valhall-1691526.wa` firmware blob is present and the GPU has a TTRX_3485-family issue, and G78 may be
one of those GPUs. Neither `mali_kbase_dummy_job_wa.c`, the hw-issue table for tBEx r0p1, nor the bluejay `/vendor/firmware` was checked.
The author's file:line table and the comment "Sequence = Google's kbase JM power manager" never mention this branch.
Failure scenario: if Pixel's kbase runs the workaround on this GPU, the shader power-up has an erratum that kbase deliberately avoids
with a dummy job. A plain SHADER_PWRON could then leave the cores hung in PWRTRANS (`up=timeout:shader`), or bring them up wrongly while
READY still reads the mask, so the run reports success on a state kbase never produces. The F3 path then writes SHADER_PWROFF into a
transition.
Fix: get the Pixel `mali_kbase_dummy_job_wa.c` and `mali_base_hwconfig_issues.h` (tBEx r0p1 row), and check whether a bluejay
factory/vendor image ships `valhall-1691526.wa`. Cite the result. Until then, run stage 1 with tiler/L2 only (TILER_PWRON + L2_PWROFF).

**F2 (M) Everything kbase_pm_init_hw does before any power-on is skipped, and the precheck does not prove those steps are unneeded.**
Before the L2 state machine ever runs, kbase does: disable and clear the IRQs, soft reset (`protected_mode_disable`), `kbase_pm_hw_issues_apply`
(writes JM_CONFIG 0xF00, SHADER_CONFIG 0xF04, TILER_CONFIG 0xF08, L2_MMU_CONFIG 0xF0C with the errata quirks, pm_driver.c:4171-4187),
`kbase_cache_set_coherency_mode` (COHERENCY_ENABLE 0x304), and a check that protected mode is off (pm_driver.c:4532-4560). Right before
TILER_PWRON it also calls `kbase_pm_l2_config_override` + `kbase_pbha_write_settings` (pm_driver.c:2161-2162; L2_CONFIG 0x48 is written
only if the DT gives size/hash overrides). Whether the gs101 DT gives them was not checked. None of this is in the shim, and none of it is
listed as a deliberate omission.
Failure scenario: ABL left GPU_STATUS or the quirk registers in a state that differs from post-reset (G0 saw only GPU_STATUS/FAULTSTATUS = 0
and RAWSTAT = 0x100). The cores then power up without the errata quirks, and the run reports `up=ok` for a configuration kbase never uses.
Hang risk on the AP bus: low (*guess*). Risk of a misleading "works": real.
Fix: (a) in precheck, also require GPU_STATUS == 0, GPU_FAULTSTATUS == 0 and (RAWSTAT & ~0x100) == 0, i.e. the observed G0 state.
(b) Dump JM/SHADER/TILER/L2_MMU_CONFIG, L2_CONFIG (0x48), COHERENCY_ENABLE, STACK_READY/STACK_PWRTRANS (0xE10/0xE20) and GPU_IRQ_MASK
read-only, before and after. (c) In gpupower.c's header, list each skipped kbase step with its line and the reason it is safe to skip.
Note: RAWSTAT bit 8 is `RESET_COMPLETED` (macros.h:276). Doc 27 still calls it a guess. It is now *read from source*, and it suggests
that a reset completed before the shim and nobody acknowledged it.

**F3 (M) The OFF path issues PWROFF in states kbase never writes from, and its value is not kbase's.**
kbase writes SHADER_PWROFF only from ON with `shaders_ready` (2846-2847), and L2_PWROFF only from L2_POWER_DOWN, reached after PEND_ON finished.
The shim always writes SHADER_PWROFF = present mask, right after any ON failure:
- after `fault:tiler`, `timeout:tiler` or `timeout:l2`, it writes SHADER_PWROFF to shaders that never had PWRON, then L2_PWROFF while the L2 may still be in PWRTRANS;
- after `timeout:shader`, it writes SHADER_PWROFF while the shaders may still be in PWRTRANS.
The host test codifies this (the `stuck_tiler_on` case expects 3 writes, including SHADER_PWROFF and L2_PWROFF to an L2 that never came on).
The comments tag "harmless" as a *guess*, which is honest but unverified.
Failure scenario: a PWROFF that lands during a PWRON transition leaves the power controller with L2 or shaders stuck in PWRTRANS. The run then
reports `down=timeout:*` and leaves the GPU half-on until the next reset.
Fix: before each PWROFF, first wait for that block's PWRTRANS to drain, as kbase's PEND states do. Then read READY: if it is 0, skip the
write (kbase would not write); otherwise write PWROFF. Write the READY mask, which is exactly kbase; widen `w32_allowed` to "nonzero
subset of present" for SHADER_PWROFF only. Alternatively, keep the present-mask write but only when READY == present. Update the host-test expectations.

**F4 (M) Wait bound: polls, not time, and the stated rationale is false.** Doc 17 §2.4 says (*guess*) "kbase has no timeout of its own".
kbase JM has `PM_TIMEOUT_MS = 5000` (pm_driver.c:3244), used by `kbase_pm_wait_for_desired_state` (3344-3355). 100000 polls means 200000
fault-guarded reads per wait, each with `dsb sy` + DAIF toggles + `isb`. My estimate, a *guess*: 0.3-1 us each, so about 60-200 ms per wait
and at most about 1.4 s over the 7 waits. That is likely enough for µs-scale power-up, but nobody has measured it. If the G3D clock runs from
OSCCLK (G0 PLL_CON0 = 0x10 has bit 31, ENABLE, clear: a *guess* about the Samsung PLL layout), transitions may be slower than assumed.
No unbounded stall exists in the C code. The only way to stall indefinitely is an AXI access that never gets a response. The fault guard
cannot help there, and the shim has turned the watchdogs off, so recovery is a long-press. The precheck makes this unlikely: the GPU_ID/PRESENT
reads must succeed first. Fix: bound each wait by `now()` (for example 100 ms, citing kbase's 5 s as the upper reference) and print the
elapsed ticks for each wait, so the run measures it.

**F5 (L) Half-on state and handover.** With `down=timeout:shader`, the shim leaves the L2, tiler and shaders on. Nothing hands this state to
XNU: the `--boot-xnu` `loader_boot()` runs before the GPU section (shim.c:541), so gpu_power_cycle runs only on the probe path, or after
a failed loader. The next boot is a full reset. I *guess* SYSTEM_RESET resets the G3D domain; not checked. Acceptable for a probe. Also note:
the shim leaves POWER_CHANGED bits (9/10) set in RAWSTAT and never clears them. That must not be copied into an XNU-path version.

**F6 (L) Untagged or over-tagged claims.**
(a) Doc 17 §2.4 says "back to the state ABL left (all `*_READY` = 0, *observed* in G0)". G0 read only SHADER_READY and L2_READY. TILER_READY
and all three PWRTRANS were never read (gpuprobe.c:141-142). The precheck reads them live, so this does not affect safety, but the tag is wrong.
(b) "CORESTACK is not driven: MALI_CORESTACK is `default n` (Mconfig:168-171)". A Kconfig default does not show what the Pixel build sets.
Check the Pixel gpu module Makefile/defconfig, and dump STACK_READY (see F2b).
(c) "the HI word is never written": kbase uses `kbase_reg_write64` (pm_driver.c:661), which writes both words. Writing LO only is fine
(HI = 0), but the comment should say that it differs from kbase and why.
(d) Doc 17 §2.4: "kbase has no timeout of its own" is false (F4).
(e) Doc 27 still tags RAWSTAT 0x100 as a guess (F2).
(f) `docs/AUTOMATION.md` still says "quick: ran N of 15 steps"; ALL now has 17 entries. This predates the change, but the change touches that line's context.

**F7 (L) Error labels.** `w32` returns -2 for a pair the allowlist refuses, and the caller reports it as `fault:*`. A future refactor that passes a wrong value would
show up as a "bus fault" on hardware. Report `refused:*` for -2.

**F8 (L) The precheck checks only the LO words of READY/PWRTRANS/PRESENT.** HI is 0 on a 20-core part. Reading 0x104/0x144/0x204 too costs nothing.

**F9 (M) Audit strength. The audit is not loosened, but it has gaps the host test only partly covers.**
- `_c_function_span` regex change (added `const`, `unsigned`, `char` and pointer returns): it now finds strictly more definitions. Every
  existing caller had to match before, or it asserted. Under the new regex the first match could move only if a second definition of the same
  name existed, which is a compile error unless it sits in a disabled `#if`. I judge this not a loosening. All other existing audits are unchanged in the diff.
- The object audit runs on `gpupower.o` for every bluejay build: the body is gated on `SOC_GS101 && GPU_MALI && GPU_TILER_PRESENT`, not on `GPU_POWER`. Good.
- My mutations that the SOURCE audit lets through: (1) the OFF path writes SHADER_PWRON instead of PWROFF; (2) precheck short-circuits with `return 0`;
  (3) `dump()` calls `w32(G_TILER_ON, ...)`; (4) the OFF sequence is skipped (`return 0` after ON); (5) `wait_for` resets `i` (unbounded);
  (6) `wait_for` recurses; (7) GP_POLLS = 4000000000u.
  The host test catches (1)-(3) (I checked: abort). It should also catch (4) by its write counts, and (5)/(6) would hang or crash it in the stuck cases.
  Only (7) passes everything: it would make the host test slow but green.
  Fix: cap GP_POLLS in the audit (for example <= 1000000). Have the audit reject any assignment to the loop variable in `wait_for` and any recursion.
  Keep the host test mandatory in the gate (it is: build.sh runs with `set -euo pipefail`).

**F10 (L) soc.json/boards.py pins: correct.** The pins match G0 (doc 27 table). `_gpu_check` runs inside `c_header`, so a moved value fails the build.
The values are emitted as `UL` and compared against `uint32_t v`, which is fine. One gap: `_gpu_check` does not pin `mali` itself (only the
4 KiB alignment), and the allowlist addresses derive from `GPU_MALI`. A typo in `soc.json` `mali` would move all four writes, and the
GPU_ID/PRESENT precheck is the only guard (a strong one: wrong base means wrong ID). Add `"mali": 0x1C500000` to `_GPU_PINS`.

## Citation check (pm_driver.c, kbase-bluejay)
All verified within ±1 line: need_tiler_control 1839-1850 (true on JM); comment + TILER_PWRON 2164-2186; extra L2_PWRON only if l2_present != 1, 2199-2204;
PEND_ON wait 2212-2218 (kbase masks `trans &= ~ready` (2138), while the shim requires trans == 0, which is stricter and fine); SHADER_PWRON 2659-2660;
wait 2671-2672; SHADER_PWROFF 2846-2847; wait 2855-2856; L2_PWROFF / JM tiler note 2399-2412 (TILER_PWROFF only under CSF + host_pwr_iface);
PEND_OFF 2438; CORESTACK branch 2646-2649. Omitted, and not cited by the author: 644-647 (F1), 2161-2162 and 4466-4560 (F2), 3244 (F4).

## Fault-guard check
`try_write32`/`try_read32` (shim.c:129-153) set `probe_guard`, then run `dsb sy` and briefly unmask SError, so a pending SError from the
access is taken inside the window. `exc_handler` (shim.c:864-880) skips a current-EL Data Abort (ELR + 4) and absorbs an SError (A stays
masked). A faulting write mid-sequence therefore yields `fault:*`, and the OFF path continues with its own guarded writes. The guard does not
cover an access that never completes (F4). IRQs stay masked on this path (only shim.c:1012-1023 unmasks), so POWER_CHANGED cannot reach the shim.

## Re-review

**Verdict: GO-WITH-FIXES.** Fix N1 (one wait) before the first hardware run. Everything else is L/info and can follow.
Reviewer: Claude Opus 5.5, read-only, 2026-10-23. Host-only checks I ran (into /tmp, shim/out untouched; I only copied `out/board_config.h`):
`gpupower_host_test.c` PASS; `gpupower_source_selftest` (all mutations caught) + `check_gpupower_source_text` PASS; I cross-compiled
`gpupower.c` with build.sh's CFLAGS (-O2) + `-DGPU_PROBE -DGPU_POWER`; the object audit and object self-test pass on it (48 stores, 3/3).
F1 re-checked in `~/pixelios-artifacts/refs/kbase-bluejay` (issues.h:345-361, hw.c:206-211, dummy_job_wa.c:217-226, .h:53-68).

### F1-F10
- **F1 fixed.** Header gpupower.c:10-13 and doc 27 addendum. I confirmed the tBEx r0p1 issue list has no TTRX_3485, and `wa_blob_load_needed` needs it. Residual (L):
  the header says the decode as fact, while doc 27 says the GPU_ID field layout is unverified (the gpu_id.h fetch failed). Tag the header to match.
- **F2 partly fixed.** (a) is done (precheck lines 252-259). (c) is done (header lines 29-39). (b) is only partly done: STACK/L2_CONFIG/COHERENCY/IRQ_MASK are dumped
  (lines 311-315), but JM/SHADER/TILER/L2_MMU_CONFIG 0xF00-0xF0C are not. The header claims they "only affect job/MMU behaviour", which is untagged.
  It is a *guess* (SHADER_CONFIG/TILER_CONFIG quirks are core-side). Fix (L): dump 0xF00-0xF0C read-only, and tag the claim.
- **F3 fixed.** `power_off` (265-280): drain PWRTRANS, READY 0 -> no write, partial -> code 2 with no write, write READY (== present), wait. L2_PWROFF is
  written only if the shaders reached 0 (332-335). One gap remains in the same area: N1.
- **F4 fixed.** `wait_for`/`wait_idle` stop at 1 s of CNTPCT or 100000 polls, whichever comes first. Elapsed ticks for the ON and OFF phases are printed.
- **F5 unchanged.** That is acceptable for the probe path. POWER_CHANGED bits are still left in RAWSTAT; keep the note for the XNU path.
- **F6 fixed.** (a)/(d) doc 17 §2.4, (b) tagged unknown + STACK dumps, (c) header (a), (e) doc 27, (f) AUTOMATION.md says 17 = `len(ALL)`. (Unrelated:
  AGENTS.md:42 still says "of 11 steps".)
- **F7 fixed.** `uname` reports -2 as `refused:*`, and `power_off` returns code 4 -> `refused:*`. The host test covers both.
- **F8 fixed.** The PRESENT HI words (238-243) and the READY/PWRTRANS HI words (244-251) must all be 0. The host test covers all 9.
- **F9 fixed.** The source audit caps GP_POLLS (<= 200000), requires the time bound in both waits, rejects writes to the loop variable, rejects recursion through the call graph,
  pins the w32 call sites, the OFF order, and precheck's single `return 0`. All my earlier mutations (1)-(7) are now in the self-test. Cosmetic (L): the
  recursion comprehension in test-bluejay-boot.py reads `if g != fn or True if re.search(...)`. It works only because Python allows chained comprehension `if`s,
  so drop `g != fn or True`.
- **F10 fixed.** `_GPU_PINS` includes `mali` (boards.py:115) and is checked in `c_header`.

### New findings
**N1 (M) L2_PWROFF can still land in a tiler transition.** `power_off(G_L2_OFF, ...)` drains only L2_PWRTRANS (line 270). After `up=timeout:tiler`, the run can still write L2_PWROFF into an unfinished tiler power-up.
This needs the L2 to come up while the tiler is still in PWRTRANS past its 1 s wait.
In that case the OFF path drains L2, sees L2_READY = 1, and writes L2_PWROFF while TILER_PWRTRANS != 0. kbase reaches L2_POWER_DOWN only after
PEND_ON saw the tiler settled (pm_driver.c:2212-2218). That is the F3 failure mode again, now for the tiler. It is unlikely (needs a >1 s transition), but it is the only
path left where a PWROFF is written during a PWRON transition. Fix: drain TILER_PWRTRANS too before the L2 block, e.g. `if (!d && !wait_idle(G_TILER_TR)) { blk = 2; d = 1; }`
before line 334, and add a host case (tiler trans stuck, L2 ready) that expects 1 write.
**N2 (L) The "no-timer" refusal can never fire on gs101.** `counter_freq()` is the compile-time `TIMER_FREQ` 0x1770000 (console.c:27-30), not CNTFRQ. It does not prove that
CNTPCT runs. Harmless: if CNTPCT stood still, the poll bound would still end each wait (about 200k guarded reads), and earlier runs show the shim's other CNTPCT waits work.
Wrap is fine (unsigned u64 difference). The printed ticks are the low 32 bits, which wrap only after about 174 s, beyond the maximum possible run (about 8 waits x 1 s).
**N3 (L) STACK_PWRTRANS is dumped but not prechecked.** A nonzero value (something in flight) would still be allowed. Add `STACK_PWRTRANS == 0` to precheck (G0 never read it).
**N4 (info) The compiled w32 does not re-check `pa` for the 1-valued registers.** -O2 IPA folds the allowlist using the call sites. The binary tests
`(pa & ~0x40) == 0x1C500180 ? v == 0x779FFF : v == 1`, so for the TILER_ON/L2_OFF case the address check is gone. That is correct for every reachable call (all
call sites are constants, see below), but the binary allowlist is not the defence in depth the source suggests. The host test exercises the -O1 x86 build of `w32_allowed`.
Acceptable, because the audit pins the call sites. Do not rely on the allowlist alone if a variable-address call site is ever added.

### Precheck vs the observed G0 state (doc 27: gate open, GPU_ID 0x92020010, PRESENT 0x779FFF/1/1, SHADER/L2_READY 0, RAWSTAT 0x100, STATUS 0, FAULT 0)
Every condition passes on the observed values: no-timer (constant, passes), gate (G0: PMU 1/1, muxes selected), ID, PRESENT LO, `rawstat == 0x100`, STATUS 0, FAULT 0, SHADER/L2_READY LO 0.
The following were not read in G0 and are checked live, so any nonzero value refuses with zero writes: TILER_READY, the 3 PWRTRANS, and all HI words (all expected 0, a *guess*).
No condition wrongly refuses the G0 state. Nothing that G0 shows wrongly passes. Wrong allows that remain: STACK_PWRTRANS (N3), and the quirk registers (F2b, not checked).

### Exact MMIO write set of the shipped code (from the -O2 aarch64 disassembly of my /tmp build of this source)
`try_write32` has exactly one `bl`, inside `w32`. `w32` has 3 call sites: gpu_power_cycle `(0x1C500190, 1)` and `(0x1C500180, 0x779FFF)`, and power_off, called twice with
reg/present `(0x1C5001C0, 0x779FFF)` and `(0x1C5001E0, 1)`. power_off passes the constant `present`, after `READY == present` was checked.
Hence at most these 4 writes, each at most once per boot (gpu_power_cycle is called once, at shim.c:701, after a precheck with zero writes):
TILER_PWRON = 1, SHADER_PWRON = 0x779FFF, SHADER_PWROFF = 0x779FFF, L2_PWROFF = 1. No HI word, no GPU_COMMAND/STACK/quirk/PMU/CMU/SMC write.
All other stores in the object are stack stores (the object audit counts 48).

## Review of the stack variant

**Verdict: GO-WITH-FIXES** (2026-10-23, reviewer Opus 5.5, read-only; uncommitted working tree at 53ae38e; nothing run on hardware).
The sequence, allowlist and failure paths are correct as written. S1 is a test gap that lets a serious mutation through the whole gate. Fix it before the run (cheap).
S2-S5 are hardening and documentation.

### (1) Order vs kbase corestack path (*read from source*, ~/pixelios-artifacts/refs/kbase-bluejay/pm_driver.c)
- ON: STACK_PWRON (`stacks_avail` = STACK_PRESENT, 2593-2598) is written from SHADERS_OFF_CORESTACK_OFF only when `l2_state == KBASE_L2_ON` (2645-2649).
  It then waits `!stacks_trans && stacks_ready == stacks_avail` (2657; kbase masks `stacks_trans &= ~stacks_ready` at 2625, while the shim requires trans == 0, which is stricter)
  and only then writes SHADER_PWRON (2659-2660). The shim does the same: tiler+L2 wait, STACK_PWRON, wait_for(STACK_READY==0x7F, STACK_PWRTRANS==0), SHADER_PWRON. Matches.
- OFF: SHADER_PWROFF (2846-2847), wait `!shaders_trans && !shaders_ready` (2856), STACK_PWROFF = stacks_avail (2857-2859), wait `!stacks_trans && !stacks_ready` (2866).
  The L2 may only leave KBASE_L2_ON when `shaders_state == KBASE_SHADERS_OFF_CORESTACK_OFF` (2297-2300). So on JM the stacks go off AFTER the shaders and BEFORE the L2.
  The shim does exactly that: power_off(shader), then power_off(stack) only if d == 0, then the tiler drain, then the L2. Matches.
  Documented deviation: the STACK_PWROFF write is skipped when STACK_READY == 0. Consequence: after `up=timeout:stack` with READY 0 and PWRTRANS 0, the shim does write L2_PWROFF.
  kbase would instead sit in PEND_ON forever. With nothing ready and nothing in flight, this is harmless (*guess*, L).
- `corestack_driver_control` is `true` only with CONFIG_MALI_CORESTACK, and it is also a 0444 module param (pm_driver.c:63-68). The Pixel value stays unknown (*guess*, correctly tagged).

### (2) 64-bit registers, LO only
- STACK_PRESENT/PWRON/PWROFF/READY/PWRTRANS are 64-bit (enums.h:727-731). kbase writes them with kbase_reg_write64 (pm_driver.c:661). HI = 0 is right for the observed present = 0x7F
  (G0, STACK_PRESENT LO 0x7F; the precheck also requires HI 0xE04 == 0).
- That a LO-only write starts the transition is *observed* for TILER_PWRON (tiler+L2 came up) and SHADER_PWRON (PWRTRANS went to the mask) in run g2power. For STACK_PWRON it is a *guess* by analogy.
  Tag it in the header comment.
- **S3 (L)** The precheck does not read STACK_READY HI (0xE14) or STACK_PWRTRANS HI (0xE44), unlike the other blocks (0x144..0x224). Add them under GPU_POWER_STACK for parity.

### (3) Failure paths (traced in source, and in the -O2 aarch64 disassembly of my /tmp build)
- Tiler/L2 ON fails: no further ON writes. In OFF, shader READY 0 and stack READY 0 cause no writes; then the L2. STACK_PWRON refused/fault: the shaders are never written. In OFF, power_off(stack) drains
  STACK_PWRTRANS first: if it is stuck, `timeout:stack` and no L2_PWROFF; if READY is partial, `partial:stack` and no L2_PWROFF; if READY == 0x7F, STACK_PWROFF then the L2.
- STACK timeout: same OFF handling. Shader ON timeout (the observed g2power case): `wait_idle(SHADER_PWRTRANS)` fails, d = 1, and neither STACK_PWROFF nor L2_PWROFF is written. Stacks+L2 stay on, as in kbase.
- Shader OFF ok, then stack OFF timeout/partial/fault: d != 0, so no L2_PWROFF. Stack OFF ok, then the tiler drain fails: `timeout:tiler`, no L2_PWROFF.
- Result: every PWROFF is preceded by `wait_idle` on that block's PWRTRANS. No path writes a PWROFF during a pending PWRON, and no path writes L2_PWROFF with STACK_READY != 0.

### (4) Allowlist from the disassembly (aarch64-none-elf-gcc 15.2, build.sh CFLAGS + -DUSB_GADGET -DGPU_PROBE -DGPU_POWER, -fno-tree-switch-conversion -fno-jump-tables)
- Stack object `w32` keeps all address tests: {0x1C500180, 0x1C5001C0} -> 0x779FFF; 0x1C500190 -> 1; 0x1C5001E0 -> 1; {0x1C500E20, 0x1C500E30} -> 0x7F; else -2. **Exactly 6 pairs.**
  Call sites: w32(0x190,1), w32(0xE20,0x7F), w32(0x180,0x779FFF); power_off(0x1C0/0x140/0x200, 0x779FFF), power_off(0xE30/0xE10/0xE40, 0x7F), power_off(0x1E0/0x160/0x220, 1).
- Plain object: as in N4, `w32` is folded to `(pa & ~0x40) == 0x1C500180 ? v == 0x779FFF : v == 1`. So the binary has 2 address-checked pairs plus "any address with v == 1", not 4 pairs.
  Call sites: w32(0x190,1), w32(0x180,mask), power_off(0x1C0..), power_off(0x1E0..). No site produces 0xE20 or 0xE30, so **the plain binary cannot write them**.
  The only 0xE20 immediate in it is `sub x0,x0,#0xe20` (a precheck read address).

### (5) Audit strength
- **S1 (M) A dropped `else` before the STACK_PWRON line slips through every check.** Change `\telse if ((e = w32(G_STACK_ON` to `\tif ((e = w32(G_STACK_ON`.
  After a tiler/L2 ON timeout, the code then writes STACK_PWRON and SHADER_PWRON with the L2 not up. It passes the source audit, the stack host test and the object audit (same call-site counts, same 0xE20 count).
  Cause: the stack branch of gpupower_host_test.c (`#else` at :298) has no stuck_tiler_on/stuck_l2_on case (they exist only in the plain branch, :219/:222).
  Fix: run those two cases in the stack build too (expect writes TILER_ON then nothing else ON), or pin `elseif((e=w32(G_STACK_ON` / `elseif((e=w32(G_SHADER_ON` in the audit.
- Other mutations I tried slip the source audit but fail the stack host test: stack-off unguarded, stack-off result ignored, no stack wait before SHADER_PWRON, STACK_PRESENT HI check dropped,
  stack-not-off disabled, shader-off result ignored, L2 guard removed. The host test runs in build.sh under `set -e`, so the gate catches them.
- **S2 (L)** In `check_gpupower_variant_text`, the `#0xe30` immediate test is vacuous. Neither object ever materialises 0xE30 (the stack object computes it as 0x1C500E40 - 0x10).
  The call-site counts are the real pin. Drop the test or say so in the docstring.
- The `_gpupower_variant` split fails safe: any non-exact `#ifdef` spelling, or a nested `#if`, makes the audited view a superset of what the compiler sees.
  A `#define GPU_POWER_STACK` inside the source is caught by the plain object count (4 != 3).
- No existing audit was loosened: the plain allowlist string, the call-site pins, the OFF/tiler-drain order and the precheck refusals are unchanged, and the plain view must not mention G_STACK_ON/OFF/PRES.
  Cosmetic: the allowlist error text still says "four LO words" for the stack view.

### (6) Untagged / stale claims
- **S4 (L)** The gpupower.c header is stale for the stack build. Line 15 says "CORESTACK is not driven" and line 50 says "exactly 4 LO words". Line 36 says that HI words of READY/PWRTRANS are checked, which is not true for STACK.
  Qualify all three with "(plain build)" or "(6 with GPU_POWER_STACK)", and tag the STACK_PWRON LO-only latch as *guess* (see (2)).
- **S5 (L)** The citations are within about ±1 line (2593-2597 is really 2593-2598, 408-420 is really 408-424, 62-66 is really 63-66). Fine. The doc 27 addendum and AUTOMATION.md are tagged correctly.

## Review of --gpu-clk

Reviewer: Opus 5.5, 2026-10-23, uncommitted tree on 3691794. I read the code, cross-built `gpuclk.o`/`acpm.o` into /tmp with build.sh's flags (`-O2 ... -DGPU_PROBE -DGPU_CLK -DGPU_POWER -fno-jump-tables`, aarch64-none-elf 15.2) and disassembled them, and ran the gpuclk/dvfs host tests in /tmp. Sources: soc-gs `cal-if/acpm_dvfs.c`, `cal-if.c`, `exynos-pd.c` (android-gs-bluejay-6.1-android16), local kbase-bluejay `pixel_gpu_dvfs.c`, `pixel_gpu.c`, `pixel_gpu_power.c`, `gs101-gpu.dtsi`, and Linux v6.12 `drivers/clk/samsung/clk-gs101.c` and `clk-pll.c`.

### Verdict: NO-GO as designed. The code is safe enough to run, but the experiment's premise is contradicted by data we already have (C1), and the rate it asks for is not what Linux asks for (C2).

### Findings
- **C1 (critical, premise)** "Both GPU PLLs are OFF" (doc 27 g0b section, AUTOMATION.md, gpuclk.c) comes from an arithmetic slip and a guessed register layout. The doc says `CON3 0xA0620402 (lock bit 29 clear)`, but 0xA0620402 has **bit 31 and bit 29 set**.
  On gs101, Linux treats PLL_CON3 as the PLL control register (`PLL(pll_0517x, ..., PLL_CON3_PLL_SHARED0)`, clk-gs101.c:646-660). For pll_0516x/0517x/0518x the layout is ENABLE = bit 31, LOCK = bit 29, MDIV [25:16], PDIV [13:8], SDIV [2:0] (clk-pll.c:424-431, 1375-1383).
  PLL_CON0 bit 4 is the mux that selects oscclk or the PLL output (`MUX(..., PLL_CON0_PLL_SHARED0, 4, 1)`, parents `{oscclk, fout}`, clk-gs101.c:664, 960). Decoded, both PLL_G3D and PLL_G3D_L2 are enabled and locked: 24.576 MHz x 98 / (4 << 2) = **150.5 MHz, which is the firmware's 151000 kHz**. CON0 = 0x10 means the mux selects the PLL.
  *Read from source + observed values. Guess: CMU_G3D's PLLs use the same pll_0517x/0518x layout as CMU_TOP's.* The exact frequency match makes this very likely. So the GPU clocks are probably already running at the init level. A FREQ_REQ for that same rate is then expected to do nothing, and the stuck SHADER_PWRTRANS needs another explanation. Before any run, fix doc 27 and gpuprobe's interpretation (decode CON3 instead of treating CON0 as on/off).
- **C2 (high)** Linux's boot request is **G3DL2 = 302000, G3D = 151000**, not 151000/151000. `gpu_dvfs_set_initial_level` uses `pc->dvfs.level_min` = the last table row (pixel_gpu_dvfs.c:770-771, 866). In gs101-gpu.dtsi:65 (v1) and :87 (v2) that row is `302000 151000` (gpu0 = G3DL2, gpu1 = G3D).
  Doc 27 says "the DT table was not fetched", but it is in `~/pixelios-artifacts/refs/kbase-bluejay/gs101-gpu.dtsi`. That makes these claims wrong: gpuclk.c "kbase's set_initial_level does exactly that", `gpu.clk_khz` as the single rate for both domains, and boards.py's rule "GPU ceiling = init level = last row".
  If the run is still wanted, mirror Linux: G3DL2 302000 (an observed fvmap row) first, then G3D 151000. That is a real rate change, so CON3 of PLL_G3D_L2 should change (mdiv/sdiv), and that would at least prove the ACPM GPU path end to end. A same-rate request may be a no-op in the firmware (guess), which would make the run inconclusive either way.
- **H1 (high)** The `result: ok` check means nothing. GET already returned 151000 before the request (g0b fvmap init level), so "both read back the requested rate" passes even if the firmware ignored the request. Base the result on the before/after CMU words (CON3 enable/lock/mdiv/pdiv/sdiv, decoded as in C1), or report only "reply ok, effect unknown".
- **M1 (medium)** gpuclk.c says pd_g3d/pd_embedded_g3d are "checked by the gpuprobe gate before this runs". That is not true: `gpu_probe()` returns void, and shim.c:696-705 calls `gpu_clk()` unconditionally. gpu_clk reads no PMU word before its request. Linux's runtime path skips a rate change when the GPU is off (pixel_gpu_dvfs.c:95). Fix: read `PMU_G3D_STAT`/`PMU_EMB_STAT` (read-only, gpuprobe's `rd`) and refuse unless both read 1.
- **M2 (medium)** shim.c ignores gpu_clk's return value, and `gpu_power_cycle` still runs after a failed or timed-out request. A FREQ_REQ that is still queued can then run in the APM during the shader power-on, which no earlier run has done. Skip the power cycle (print why) when gpu_clk failed with `published != 0`.
- **M3 (medium, doc)** "What can be written (exactly): the two FREQ_REQ transactions" (AUTOMATION.md) is incomplete. There are 8 transactions: 2 GETs, then per domain a pre-read GET and a REQ, then 2 read-back GETs. Every one writes the same 8 words (4 slot words, req front, doorbell, rsp rear, INTCR0; `writes=8` in dvfsr2). gpuclk.c's "REFUSED-or-FAILED at the first GET, nothing written" is also wrong once that GET was published. Use the `x->n` and `published` fields the code already has.
- **M4 (medium, claim)** "FREQ_REQ through this client has never run on hardware (dvfs1..4 used patch 0096)" (AUTOMATION.md, doc 27) is wrong. According to HANDOFF session 13 and commit c32fbd5, dvfs1-cl0..dvfs4-all set the CPU rates with this shim client (`build-xnu-image --dvfs`). Patch 0096 only measured them.
  *Observed:* the effect (CPUs at ~1802/1835/1826 MHz). *Not observed:* the FREQ_REQ reply words; the shim's output of those XNU images is not in the run logs.
- **L1 (low)** `acpm_dvfs_set` (CPU, TOUCH_DVFS_WRITE) now finds domains 5/6, because `dvfs_find` loops over `ACPM_DVFS_NALL`. Only its caller (touchprobe, `i < ACPM_DVFS_N`) and the exact domain order pinned in boards.py keep the GPU domains out. Today it could only send 151000 to G3D/G3DL2, and only when their GET differs. Add `i < ACPM_DVFS_N` in `acpm_dvfs_set` and a host-test assert for that refusal.
- **L2 (low)** In an image with both `--touch-probe=dvfs` and `--gpu-clk`, both start `t.seq = 1` on channel 1. That is harmless as far as the source shows, because the reply check is ours. Noted for the record.

### Checks that hold
- (1) Write set (disassembly): gpuclk.o has no `try_write32`/`try_read32` reference; its undefined symbols are the ACPM client, console and `gpu_probe_cmu`. All its stores are to the stack. acpm.o calls `try_write32` from `w32` only, and `w32` is called from 5 sites, all inside `xfer` (slot loop, front, bell, rsp rear, INTCR0).
  The two request sites pass `w1 = 6` then `w1 = 5` with `w2 = 0x24dd8` (151000). `dvfs_rate_ok` with ceiling 151000, which is also the last row, lets only 151000 through for 5/6. `acpm_dvfs_reassert` refuses any other domain. boards.py `dvfs_requests` excludes the GPU domains, and touchprobe iterates `acpm_dvfs_dom(i < 3)`.
- (2) Framing matches `exynos_acpm_set_rate` (acpm_dvfs.c:97-135): `{idx, kHz, FREQ_REQ = 0, ms}`, sequence in `[21:16]` added by the IPC layer. G3D/G3DL2 are `response = true` and are not async-DVFS eligible (only CPUCL0-2/MIF/INT are, :41-56), so channel 1 synchronous IPC is the path Linux uses for them. The return value is reply word 3, nonzero = error (cal-if.c:85-102); the shim treats it the same way.
- (3) kbase does no regulator or voltage write before the rate request (none in pixel_gpu_dvfs.c; voltage comes from the APM's fvmap, *guess* that the APM sets it). Order: `gpu_pm_init`, then `gpu_dvfs_init` (pixel_gpu.c:243-245). PD power-on is `pmucal_local_enable` (+SMC), with no ACPM (cal-if.c:231-266). G3DL2 before G3D matches the source.
  AP hang: the wait is bounded (50 ms + 1e6 polls). If the APM hangs (*guess*: it could happen on a PLL relock with no timeout), the likely outcome is an APM watchdog reset of the SoC. That is reversible under `fastboot boot`.
- (4) After a timeout nothing is rolled back. The later GETs are refused BUSY with zero writes (queue not empty), and STATE is printed. That is correct.
- (5) The `layout_verified`/`elem_unit_verified` flags are justified for channel 1: in dvfsr1/dvfsr2 (`logs/probe/*dvfsr*`) the channel 1 descriptor validates (q_len 3, 16 B slots), and FREQ_GET of CPUCL0-2 is observed with a correct sequence (`rep=000X0000 <kHz> 00000001 00000000`). Those runs show GET only. No GET or REQ for idx 5/6 was ever observed.
  test-bluejay-boot.py: only additions (gpuclk source audit with 14 mutants, object audit with an allowlist that excludes try_read32/try_write32). No existing check is loosened. The `noipa` on gpuclk's string helpers only stops inlining, and their stores remain argument-based for the store audit. In dvfs_host_test.c the "unknown domain" probe moves 5 -> 7, because 5 is now valid. That is correct, not a weakening, but no test covers L1.

### (6) Untagged or wrong claims
C1 ("PLLs OFF", "lock bit 29 clear"), C2 ("kbase does exactly that", the "DT table was not fetched" guess, which is resolvable and false), M1 ("checked by the gpuprobe gate"), M3 ("only writes ... two FREQ_REQ", "nothing written"), M4 ("never run on hardware"). gpuclk.c's 10 ms settle time is tagged guess, which is fine.

## Review of --gpu-rails
Reviewer: Claude Opus 5.5, 2026-10-23, uncommitted tree (gpurails.c/.h, gpurails_host_test.c, `acpm_s11_read`, soc.json `acpm.s2mpg11`, boards.py). Host-only: /tmp cross-build with build.sh's CFLAGS (`-DUSB_GADGET -DGPU_PROBE -DGPU_RAILS`) plus the gpurails host test (PASS). No hardware.

### Verdict: GO (first run: `--gpu-rails` alone, without `--gpu-clk`/`--gpu-power`, see L1). M1 is about how the result is read, not about safety.

### Findings
- **M1 (medium, interpretation)** Mode 1 (`SEC_OPMODE_SUSPEND`) is not evidence of standby or retention. The DT gives the same initial mode to S2MPG10 BUCK1..BUCK6 (gs101-pmic.dtsi:130-210, the CPU/MIF rails, which are plainly powered while the AP runs), and BUCK2S is also `regulator-always-on` (:764-768). So `mode=1` is the expected reading. The doc 27 hypothesis ("may still be at a retention voltage") is tagged *guess*, but it should say this. The test is the voltage: B2S OUT1/OUT2 against the DT window (450-1300 mV) and against B8S. Consider renaming `on-standby(SUSPEND)` to `1(SUSPEND, DT default of the CPU rails too)` so that a run reader does not over-read it. *Read from source.*
- **L1 (low)** shim.c ignores the return value of `gpu_rails()`. In an image that also has `--gpu-power`/`--gpu-clk`, a timed-out S11 read stays queued on IPC channel 2 while the later stages run. The power cycle uses no ACPM, and gpu_clk uses channel 1, so no shared-queue conflict follows from the source. Still, keep the first run single-purpose.
- **L2 (low, test)** gpurails_host_test.c:80 checks only `(w1 & 0xff) == 0`. Tighten it to `w1 == 0` for every request. The disassembly already shows `w1` is the constant 0, so this has no effect on the image.
- **L3 (info)** `acpm_s11_read` does not mask `ACPM_S11_PMIC_ADDR`/`ACPM_S11_SPEEDY` with 0xF the way `pmic_w0` does. boards.py pins both to 1, so the result is the same.

### Checks that hold
- (1) Write set (disassembly of /tmp acpm.o and gpurails.o). gpurails.o has these undefined symbols only: `acpm_cfg_init/describe/s11_read/why`, `con_str` and `counter_freq`. It has no `try_*` reference, and all its stores go to the stack (line buffers, `t`).
  acpm.o in this image has no `acpm_pmic_update` and no `dvfs_set` (both are `#ifdef`'d out). `try_write32` is reached only through `w32`, which is called at the 5 audited `xfer` sites.
  `xfer.constprop.0` has 3 callers, all passing a constant request word 1: `acpm_s11_read` w1 = 0 (FUNC_READ) and w0 = `reg & 0xff | 0x1100`; `acpm_pmic_read` w1 = 0; `acpm_dvfs_get` w1 = 1 (FREQ_GET), mode 1. Word 2 is 0 and word 3 is constant-folded to 0. No caller can form FUNC_WRITE (1), UPDATE (2) or BULK_WRITE (4).
  `reg` is checked against the 10-entry soc.json table before `xfer`. Reply values go only into `v[]` and the printed lines, and are never sent back.
- (2) Framing, checked against GK android-gs-bluejay-6.1-android16 (fetched today). In acpm_mfd.c:15-56 `exynos_acpm_read_reg`, cmd[0] = TYPE<<8 | REG | CHANNEL<<12, and cmd[1] = FUNC_READ (enum value 0, acpm_mfd.h:66) with value = cmd[1][15:8] and RETURN = [31:24] (acpm_mfd.h:12-37).
  s2mpg11-core.c:52-60 uses `u8 channel = 1` and `i2c->addr` = `I2C_ADDR_PMIC` 0x01 (:29, :348). S2MPG10 uses channel 0 (s2mpg10-core.c:58). The DT `s2mpg11mfd` node has `acpm-ipc-channel = <2>` and `acpm-speedy-channel = <1>` (gs101-pmic.dtsi:713-716), the same IPC channel as S2MPG10 (:35-38). So w0 = 0x11RR is correct.
  All 10 registers match s2mpg11-register.h: STATUS1 0x0C (:38), B2S 0x15/0x16/0x17 (:48-50), B8S 0x25/0x26/0x27 (:64-66), L1S_CTRL1/2 0x41/0x42 (:79-80) and LDO_CTRL1 0x52 (:96). None of them is an INT register (0x00-0x05, clear-on-read). STATUS1 is in the regmap read-only set (s2mpg11-core.c:199).
  The S2MPG10 twin is observed with this exact framing: logs/probe rails1/rails2 show `ACPM read L25M rep=0001015c 00002c00 ...` (seq 1, TYPE 1, CHANNEL 0, value in [15:8], RETURN 0).
- (3) Hang and queue state. This uses the same `xfer` as the S2MPG10 read observed above, so its gates (empty queues, not full, timer) and its bounded wait (50 ms, `ACPM_MAX_POLLS`) apply.
  On a timeout the request stays queued and nothing is rolled back. gpurails breaks at the first failure (no retry, no second register) and prints STATE. A later ACPM user then refuses BUSY with zero writes.
  On the APM side, speedy channel 1 is the bus Linux reads at every boot (CHIPID probe, s2mpg11-core.c:339-341). That probe treats a failed read as "device not found ... not an error", i.e. a failed read returns a code and does not hang (read from source, *inferred*). If the APM did hang (*guess*, unlikely), the worst case is an APM watchdog reset, which is reversible under `fastboot boot`.
- (4) Decode, against s2mpg11-regulator.c:227-272 (BUCK2S/8S: vsel `*_OUT1`, 256 steps, enable `*_CTRL` mask 7:6; LDO1S: vsel L1S_CTRL1 mask 0x7F, enable LDO_CTRL1 mask 5:4) and register.h:308-315 (group 1 200000 + 6250 uV, group 4 300000 + 12500 uV). The shifts `>>6`, `>>4` and `& 0x7F` are right.
  `enabled = mode != 0` matches `s2m_is_enabled`. "OUT1 is the live output" is tagged guess in the code, the printed note and doc 27. The decode is printed only after all 10 reads succeed, and no "works" claim is made.
- (5) Audits. The `git diff tools/test-bluejay-boot.py` has 172 insertions and 0 deletions: a new gpurails source audit and allowlist (`try_read32`/`try_write32` are not allowed). No existing check is loosened.

## Review of --gpu-rail-on
Reviewer: Claude Opus 5.5, 2026-10-23. Uncommitted tree at b632f5c (gpurailon.c/.h, `acpm_s11_rail_set`, shim.c, soc.json `rail_on`, boards.py, test-bluejay-boot.py). Host-only: a /tmp copy built with `--board bluejay --avb --pstore-fast --usb-gadget --gpu-power --gpu-rail-on` (all host tests and audits PASS), and `out/shim.elf` disassembled. GK sources fetched from android-gs-bluejay-6.1-android16 today; s2mpg11-regulator.c there is identical to the local copy. No hardware.

### Verdict: GO-WITH-FIXES. The write set is exactly as claimed. Decide M1 before the first run. Run the rail alone first (no `--gpu-power`), then add the power cycle.

### Findings
- **M1 (medium, correctness vs source)** Linux writes 0x40 (mode 1), not 0xC0, to this rail. *Read from source:* the probe sets `opmode[id] = enable_mask` (s2mpg11-regulator.c:718). The DT gives BUCK2S `regulator-always-on` and `regulator-initial-mode = SEC_OPMODE_SUSPEND` (gs101-pmic.dtsi:766-779). The regulator core applies the initial mode through `s2m_set_mode` (opmode := 1<<6) before the always-on enable (`set_machine_constraints`, upstream core.c, line not checked). `s2m_enable` then writes 0x40 under 0xC0, because the rail was found off (observed 0x38). So in stock Android the only value proven on this rail is mode 1. Mode 3 is not exotic: it is the driver's default for rails without an initial mode. But consider the failure case: if the restore fails or the AP hangs, Linux will not rewrite an already-enabled rail (`need_sync`, :728-732), and a 0xC0 left behind persists into Android. A 0x40 left behind would equal stock Android. Fix: switch `val_on` to 0x40 (soc.json pin, boards.py `_ACPM_S11_ON_PINS`, audits, host test, doc 27 W1), or record why 0xC0 was chosen.
- **L1 (low)** A failed OUT1 read sets `t.b.published` through `xcopy`. The enable then returns UNVERIFIED and prints "request published but failed". The restore then most likely refuses BUSY and prints `!!! RESTORE FAILED ... may be LEFT ON`. Both messages are false: only reads were made, and CTRL read 0. Fix: give `acpm_s11_rail_set` a "the UPDATE was published" flag and print it.
- **L2 (low)** A rail left on (hang mid-cycle, failed restore) cannot be cleared by the shim. The next run's enable refuses with mode != 0 and does no restore. Persistence: *guess*. A PSCI SYSTEM_RESET is a SoC reset, not a PMIC reset, so S2MPG11 keeps the value until a PMIC power-off. The consequence (*guess*): the shader rail sits at 600 mV with idle, gated cores, so leakage only, no damage. With M1 (0x40) this state is identical to stock. Say this in the run instructions.
- **L3 (low)** The restore turns the rail off even when `gpu_power_cycle` did not confirm the shaders are off (the PWRTRANS timeout). That returns to the found state (*guess*: harmless), but log SHADER_READY/PWRTRANS just before the restore so the run can be read.
- **I1 (info, unknown bits)** `B2S_CTRL` bits 5:0 = 0x38, while `B8S_CTRL` bits 5:0 = 0x18 (observed). s2mpg11-register.h defines no field for them, so the meaning of bit 5 is unknown. The APM read-modify-write preserves them. If one of them selects OUT2 (0x30 = 500 mV, *guess*), the cores would run below the fvmap 562.5 mV at 151 MHz. That is an undervolt (a stuck or flaky power-up), never an overvolt, because both OUT values are at or below 600 mV. Print OUT2 in the readback as well.
- **I2 (info, sequencing; answers 3)** The Linux GPU driver holds no regulator handle. pixel_gpu_power.c only does `pm_runtime_get_sync` on the `top`/`cores` pd devices (:258-261), and its `rail_state_log` is bookkeeping only. `embedded_g3d_on` (flexpmu_local.h:447-458: `EMBEDDED_G3D_OUT |= 0x2`, a 280 us delay, then CONFIGURATION) contains no PMIC step. The S2MPG11 DCTRLSEL pin sources include no G3D signal (register.h:341-353), so the PMU cannot switch BUCK2S (*guess* that DCTRLSEL is the only pin control).
  In stock boot, the rail likely also comes up under an already-on embedded_g3d domain: ABL leaves the domains on (G0), and the s2mpg11 regulator probe then enables BUCK2S (*guess* on GKI module order). So no domain re-sequence is required by source. The shader core power switches are open while SHADER_READY = 0 (*guess*), which limits inrush. No GPU MMIO happens between enable and power-on, so this step adds no ITMON exposure.
  ACPM: *guess* that the APM writes BUCK2S OUT1 on G3D DVFS. This image sends no DVFS request, and the update is a read-modify-write executed by the APM itself, serialized with its own speedy traffic, so nothing fights it. Do not combine this flag with `--gpu-clk`.
  Voltage: 600 mV (observed OUT1) is at or above both the G3D (562.5 mV) and G3DL2 (600 mV) 151 MHz fvmap levels. Enabling at OUT1 is what Linux does too: it never writes vsel at enable.

### Checks that hold
- (1) Write set, from the linked `shim.elf`. `xfer` has 6 callers:
  - `acpm_s11_rail_set` has 3: pre-read w0 0x1115 / w1 0; OUT1 read w0 0x1116 / w1 0; update w0 0x1115 / w1 = `0x00C00002 | csel(on ? 0xC0 : 0) << 8`, i.e. 0x00C0C002 or 0x00C00002 only.
  - `acpm_s11_read`, `acpm_pmic_read` and `acpm_dvfs_get` pass w1 = 0 (FUNC_READ) or 1 (FREQ_GET).
  - `try_write32` is reached only via the three `w32` helpers. acpm's `w32` is called only from its 5 `xfer` sites. The other two `w32` helpers are gpupower (2 sites, reviewed earlier) and usbgadget.
  - `gpu_rail_enable` calls rail_set(on = 1) once and `gpu_rail_restore` calls rail_set(0) once.
  - `shim_main` order: gpu_probe -> gpu_rails -> gpu_rail_enable -> gpu_power_cycle -> gpu_rail_restore.
  - The OUT1 window is `sub #0x38; cmp #0x18; b.ls`, i.e. codes 56..80 = 550000..700000 uV.
  - No voltage register, other rail, FUNC_WRITE or BULK write is reachable.
- (2) Register, mask and mode encodings match the source (register.h:48, :335; regulator.c:35-111, :259-260). Disable writes 0 under the mask (:95-111).
- (4) Failure branches. A refused pre-read or OUT1-window check writes nothing and runs no power cycle and no restore. A published failure or a readback mismatch runs no power cycle and does restore. The restore uses its own preflight, at most one update, a readback and a loud failure line. `ACPM_E_ON` is returned only by `refuse()`, never by `xfer`. The soft-start wait is about 1.0 ms at 24.576 MHz.
- (5) test-bluejay-boot.py has 190 insertions and 0 deletions. New: a source audit (19 mutants), object audits of gpurailon.o and acpm_railon.o (3+4+3 mutants), the shipped acpm.o checked with the same rule, and an `nm` gate that keeps `acpm_s11_rail_set` out of non-rail-on images. boards.py pins every `rail_on` field and bounds the window inside the DT 450-1300 mV range. Nothing is loosened.
- (6) Tagging. doc 27:132 "the shader cores cannot power because their rail is off" is a hypothesis; tag it *guess* until the run. gpurailon.c:7 and doc 27:143 tag "Linux writes mode 1" as a guess; it is *read from source* (see M1).

## Review of --gpu-job
Reviewer: Opus, 2026-10-23. Source: commit 77b8447 (plus doc-only 8bc5b25). Disassembly: my /tmp cross-build of gpujob.c / gpupower.c / gpus2mpu.c with the
build.sh CFLAGS + `-DUSB_GADGET -DGPU_PROBE -DGPU_RAILS -DGPU_JOB -DGPU_S2MPU -DGPU_RAIL_ON -DGPU_POWER -fno-tree-switch-conversion -fno-jump-tables`
(aarch64-none-elf-gcc 15.2), board header generated into /tmp. Nothing was run on hardware and shim/out was not touched.

### Verdict: GO-WITH-FIXES. The write set is exactly as claimed and every constant matches kbase / Mesa. Before the run, fix M1 and M2 (both are cheap; without them a `timeout`/`failed` result is ambiguous). M3 is about audit strength and can follow.

### Findings
- **M1 (M) Job slot 0 is not justified. Read JS0_FEATURES first.** kbase sends only fragment atoms to slot 0. Everything else, including the chains Mesa puts WRITE_VALUE jobs in, goes to slot 1 (`kbase_js_get_slot`; *from memory, not in the local copies*). The doc never says why it uses JS0. Fix: in `precheck`, read `JS0_FEATURES` (GPU_MALI + 0x0C0, `regmap_jm_enums.h:70`) and refuse unless `JS_FEATURE_SET_VALUE_JOB` (bit 2, `regmap_jm_macros.h:230`) is set. Read `JS_PRESENT` (+0x01C) bit 0 the same way. These are reads only. The usual JM default for JS0 is 0x20E, which includes bit 2 (*guess*). Without this check, a `failed`/JOB_CONFIG_FAULT result can't be told apart from a table or header bug.
- **M2 (M) Header Index = 0 is unproven.** v9.xml defines the field (word 4 bits 31:16). As far as I know, Mesa never emits Index 0: `pan_jc_add_job` uses `++jc->job_index`, so the first job is 1 (*from memory: check src/panfrost/lib/pan_jc.h at e130c230*). The dependency scoreboard is index-based. Fix: Index = 1 (`GPU_JOB_HDR_W4 = 0x10004`), with the cite, and update the boards.py pin and the doc. This costs nothing and matches the only producer known to work.
- **M3 (M) The audits pin the CPU write set but not where the GPU writes.** The GPU's DMA target is whatever L3[1] holds. `w32_allowed` accepts any value inside the scratch window, and the source audit pins the `w32(...)` call sites but not the arguments of the `build(j, ...)` call or the `jc.l0_lo/hi = sc` assignments. I mutated `dlo` mentally to `0x80000000u`: every check still passes, and the GPU would write 8 bytes plus the descriptor write-back to arbitrary DRAM. The shipped code is correct: `build()` is called with sc + 0x1000/0x2000/0x3000/0x4000, and TRANSTAB = sc. Fix: pin the `build(...)` call text and the two `jc.l0_*` assignments in `check_gpujob_source_text`, and add the mutant to the selftest. Better: make `build()` derive the next-level PAs from `j->base` itself so there is nothing to pass.
- **L1 (L) The MMU-fault path differs from kbase.** On a page or bus fault, `wait_event` returns and there is no hard stop. The IRQ clear runs before the AS0 disable. kbase's JM kill path does hard-stop -> UNMAPPED -> clear fault. In the shim order, the stalled job is aborted by the UNMAPPED update after the clear, so JOB_IRQ bit 16 (and maybe a new MMU bit) is left raw. Harmless here because IRQs are masked and the phone resets. Use kbase's order: hard stop if JS0 is still active, AS off, then clear. Don't copy the current order into an XNU path.
- **L2 (L) `as_update` writes AS_COMMAND without first checking AS_ACTIVE == 0.** kbase `write_cmd` waits *before* the command (`mmu_hw_direct.c:194-210`). In JM it does not wait after UPDATE at all; only CSF does (`:356-358`). Doc 28 section 2 cites `wait_ready` as a post-UPDATE wait, which is inaccurate. It matters on the OFF path after a hung job. Add the pre-wait. If it times out, skip the UPDATE and report it.
- **L3 (L, accepted) A hung job leads to a powered-off core with an active job, then the rail is cut.** After one HARD_STOP_1 that doesn't take, the code does AS off, then SHADER_PWROFF with a job still in HEAD/NEXT. kbase never does that; it soft-resets the GPU. Then `gpu_rail_restore` cuts BUCK2S unconditionally. Worst case (*guess*): the GPU never answers an MMIO read, `try_read32` can't catch a stall, and the watchdogs are off, so recovery is a long-press. This is the same class as F4. It is acceptable for a probe, and the precheck plus the M1 check make it less likely. A job stuck in NEXT, which HARD_STOP does not touch (kbase would write `JS_COMMAND_NEXT = NOP`), stays pending until the reset. Report `JS0_COMMAND_NEXT` in the result block.
- **I1 START_MMU: the code is right.** `JS_CONFIG_START_MMU` (1 << 10) is defined (`regmap_jm_macros.h:164`) but is in the comment only, not in the code (`jm_hw.c:142-203`). Independent confirmation: kbase's own dummy-job workaround starts jobs with `CONFIG_NEXT = DISABLE_DESCRIPTOR_WR_BK | as` (`kbase-bluejay/mali_kbase_dummy_job_wa.c:91-92`): no START_MMU and no flush. It is not the first suspect for a timeout. A missing JS0 capability (M1) is a better one.
- **I2** The S2MPU was observed disabled (doc 28 section 10). Calling "disabled = pass-through" a guess is too weak. `s2mpu-lib.c:168` says "make sure that the S2MPU is enabled and hence blocking traffic", i.e. disabled does not block. Tag it *read from source (comment), not observed*.
- **I3** `PRE_COHERENCY_ENABLE = 0`. In upstream kbase, `COHERENCY_ACE_LITE` = 0 and `COHERENCY_NONE` = 31 (*from memory, not in the local copies*), so 0 may mean ACE-Lite is selected. That is harmless here because every leaf is non-shareable and NC, and the PTW uses PTW_SH = 0. Tag the doc's "unknown" accordingly. In this boot the GPU never saw kbase's soft reset or its quirk writes (JM/SHADER/TILER/L2_MMU_CONFIG). If the result is `failed`, that is a possible cause (*guess*).

### Checks that hold
- (1) **Write set, from the disassembly.** `try_write32` has exactly one `bl`, inside `w32`, after `w32_allowed`. Decoded allowlist:
  - RAM: `[base, base + 0x5000)`, 4-byte aligned.
  - AS0: 0x1C502430 = 0x42000006 or 0x42000001; 0x1C502434 = 0; 0x1C502408/0C = 0x888D8F88 / 0x4C8D; 0x1C502400/04 = l0 or 0; 0x1C502418 = 1.
  - JS0: 0x1C501840 = 0x1000; 0x1C501844 and 0x1C501854 = 0 (`pa & ~0x10`); 0x1C501850 = 0x779FFF; 0x1C501858 = 0x83B00; 0x1C501860 = 1; 0x1C501820 = 7.
  - IRQ clear: 0x1C501004 and 0x1C502004 = 0x10001.
  - Every other store in gpujob.o is a stack line buffer, or the `*v = 0xdeadbeef` out-parameter (stack in every caller). gpus2mpu.o has stack stores only and its only undefined symbols are `try_read32` and `con_str`.
  - gpupower_job.o differs from gpupower.o only by `U gpu_job_run`.
  - No GPU_COMMAND, LOCKADDR, IRQ mask, AS1+, JS1+, S2MPU, PMU, CMU or ACPM address is reachable from gpujob.o. `gpu_job_cache_flush` (shim.c) runs only `DC CIVAC` over the scratch range.
- (2) **Constants, re-derived from the sources.**
  - MEMATTR: bytes 88/8F/8D/88/8D/4C (`mem.h:114-175`, `mmu_jm.c:38-45`).
  - TRANSCFG: 6 | 2 << 24 | 1 << 30, and UNMAPPED 1 with the same PTW bits (`mmu_hw_direct.c:323-350`, `mode_aarch64.c:71-80`).
  - TRANSTAB: base mask bits 63:4 (`regmap_jm_macros.h:77`).
  - Table entry: 0x403. Leaf: 0x457 = PA | ATE_L3 3 | idx 5 << 2 | ACCESS_RW 1 << 6 | AF 1 << 10. NX and SH are clear (`mode_aarch64.c:29-183`).
  - Walk: VA 0x1000 gives L0/L1/L2 index 0 and L3 index 1.
  - CONFIG_NEXT: 0x300 | 0x3000 | 0x800 | 0x80000 = 0x83B00 (`jm_hw.c:151-201`).
  - AFFINITY: shader_present (`jm_hw.c:60-96`).
  - HARD_STOP_1 = 7 acts on JOB_CHAIN_FLAG = 1 (`regmap_jm_macros.h:155`).
  - v9 header: Type bits 7:1 = 2, so word 4 = 0x4. 128-byte alignment is met at VA 0x1000. Next, deps and barrier are 0.
  - Payload at +32: Address +0, Type +8 = Immediate 64 = 7, value +16 (`v9.xml` Job Type :37-48, header :1444-1458, payload :1487-1491, aggregate offset 32 :1507). The target at 0x1100 is 8-aligned.
  - The magic value 0x504958454C494F53 matches LO/HI.
- (3) **Memory model.**
  - The CPU runs MMU-off at EL1 (`start.S` SCTLR 0x30d00800, HCR_EL2 = RW only), so loads and stores are Device and don't allocate. The `DC CIVAC` before the build writes back stale ABL lines over zeros, not over the tables. The one after the build finds nothing to clean.
  - The GPU's data accesses (descriptor read and write-back, and the target write) all use attribute 5 (NC). Only the PTW allocates in the GPU L2, and those lines are clean, so nothing dirty is left for the L2 power-down.
  - The scratch is `.bss`, inside `image_size = _end` (`start.S:8`), so a loader honouring the header doesn't place the DTB or ramdisk there. `bluejay_probe` is noreturn and runs before `loader_boot`, so no XNU payload is in RAM.
  - GPU-side DMA is confined to the 5 pages, because only VA 0x1000 is mapped. Any other VA faults.
- (4) Failure paths are bounded: 2 x 100 ms at most for the job, plus 2 x 100 ms for the AS. AS0 is always set back to UNMAPPED, and the OFF sequence and rail restore follow. A GPU-side S2MPU or ITMON error is reported to the GPU (bus fault) and not as a CPU abort (*guess*). A stray SError would only show up as one `unreadable` on the next guarded access.
- (5) **Audits.** `git diff 77b8447~1 77b8447 -- tools/test-bluejay-boot.py` has 14 removed lines:
  - the `_c_function_span` regex, which only gains return types;
  - `_gpupower_variant`, generalised to two `#ifdef`s with the same nesting and `#else` refusals.
  - Nothing is loosened. New: a strict source audit with 21 mutants, an object audit of gpujob.o, a hook-placement audit of gpupower_job.o, and the `nm` gate that keeps the job out of other images. The host test passes in /tmp. M3 is the gap.
- (6) **Tagging (doc 28).**
  - Section 5: "nobody else uses [the scratch]" is *read from source* (image_size) plus a *guess* that ABL honours it. Tag it.
  - Section 2: "empty TLBs" after power-up is untagged and should be *guess*. The UPDATE also invalidates, so the claim isn't load-bearing.
  - Section 10, S2MPU: see I2.
  - Section 9 item 1 should point at I1.
