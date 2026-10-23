# 27: G0 result, read-only GPU probe on the Pixel 6a (2026-10-23)

*Observed on hardware*: `tools/probe-run --build-args "--pstore-fast --gpu-probe" --expect "GPU SUMMARY" --label g0` (shim built from HEAD 904aef2; run dir `logs/probe/20261022T223315Z-g0_`, not in git), `fastboot boot` only, all reads fault-guarded, no stores. Result PASS.

**The GPU is already powered and clocked when the shim starts** (ABL leaves it on); the guess in research 17 section 2.3 ("gate closed, pd_g3d-off") was wrong.

| Register | PA | Value |
|---|---|---|
| PMU G3D_CONFIGURATION / STATUS | 0x17461E00 / E04 | 1 / 1 |
| PMU EMBEDDED_G3D_CONFIGURATION / STATUS | 0x17462000 / 2004 | 1 / 1 |
| CMU_G3D MUX_..._USER TOP / COREGROUP / STACKS | 0x1C400620 / 600 / 610 | 0x10 each (bit 4 set) |
| CMU_G3D PLL_CON0_PLL_G3D / _L2 | 0x1C400140 / 180 | 0x10 / 0x10 |
| GPU_ID | 0x1C500000 | **0x92020010** |
| L2 / TILER / MEM / MMU_FEATURES | +04 / +0C / +10 / +14 | 0x08130206 / 0x809 / 0x301 / 0x2830 |
| AS_PRESENT / JS_PRESENT | +18 / +1C | 0xFF / 0x7 |
| GPU_IRQ_RAWSTAT / GPU_STATUS / FAULTSTATUS | +20 / +34 / +3C | 0x100 / 0 / 0 |
| THREAD_MAX_THREADS / THREAD_FEATURES | +A0 / +AC | 0x400 / 0x48000 |
| SHADER_PRESENT (lo/hi) | +100 / +104 | 0x779FFF / 0 (20 cores, popcount) |
| TILER / L2 / STACK_PRESENT | +110 / +120 / +E00 | 1 / 1 / 0x7F |
| SHADER_READY / L2_READY | +140 / +160 | 0 / 0 (cores not yet powered by the GPU's own power manager) |
| COHERENCY_FEATURES | +300 | 1 |
| JOB / MMU_IRQ_RAWSTAT | 0x1C501000 / 0x1C502000 | 0 / 0 |

Reading: top and embedded domains on, CMU muxes selected, GPU control block readable from EL1 (JM layout, MMU at 0x2000 consistent). `GPU_IRQ_RAWSTAT=0x100` is bit 8 = `RESET_COMPLETED` (*read from source*, kbase macros.h:276; resolved 2026-10-23: a reset completed before the shim and nobody acknowledged it). GPU_ID 0x92020010 is recorded raw; product id mapping not yet checked against kbase (rule 7).
**Consequence:** G1 (power/clock via ACPM) is no longer a blocker for G2: stage 1 can start with GPU_ID read (done) then the shader/L2 power-on through `SHADER_PWRON`/`L2_PWRON` (writes: Opus review first).

## Addendum: G2 stage 1 write sequence, source table (2026-10-23, *read from source*, not run on hardware)

Source: Google android-gs-bluejay-6.1-android16 kbase, local copy `~/pixelios-artifacts/refs/kbase-bluejay/` (`pm_driver.c`, `enums.h` offsets, `Mconfig`). Implemented in `shim/gpupower.c` (`--gpu-power`, docs/AUTOMATION.md).

| Step | Register (LO word) | Offset (PA) | Value written | kbase file:line |
|---|---|---|---|---|
| ON 1 | TILER_PWRON | 0x190 (0x1C500190) | `TILER_PRESENT` = 1 | pm_driver.c:2167-2169 comment, 2185-2186 call; `need_tiler_control()` true on JM, 1840-1851. Powers the L2 too. |
| (wait) | TILER_READY 0x150, L2_READY 0x160, *_PWRTRANS 0x210/0x220 | | tiler/L2 ready = present, PWRTRANS 0 | pm_driver.c:2212-2218 |
| ON 2 | SHADER_PWRON | 0x180 (0x1C500180) | `SHADER_PRESENT` = 0x779FFF | pm_driver.c:2659-2660 (CORESTACK branch 2646-2649 skipped: Mconfig:168-171 `default n`) |
| (wait) | SHADER_READY 0x140, SHADER_PWRTRANS 0x200 | | ready = mask, PWRTRANS 0 | pm_driver.c:2671-2672 |
| OFF 1 | SHADER_PWROFF | 0x1C0 (0x1C5001C0) | `SHADER_PRESENT` (kbase writes the READY mask: equal when all are on) | pm_driver.c:2846-2847 |
| (wait) | SHADER_READY, SHADER_PWRTRANS | | both 0 | pm_driver.c:2855-2856 |
| OFF 2 | L2_PWROFF | 0x1E0 (0x1C5001E0) | `L2_PRESENT` = 1 | pm_driver.c:2410-2412 ("Powering off the L2 will also power off the tiler"); the TILER_PWROFF branch 2401-2408 is `#if MALI_USE_CSF` and needs `has_host_pwr_iface` |
| (wait) | L2_READY, L2_PWRTRANS, then TILER_READY | | all 0 | pm_driver.c:2438 |

Differences from the task spec found while reading kbase: (1) on JM kbase powers the tiler off through `L2_PWROFF`, never writes `TILER_PWROFF` (0x1D0, CSF + host power interface only), so the shim does not write 0x1D0 and has 2 ON + 2 OFF writes, not 2 ON + 3 OFF. (2) kbase waits for the shaders to be off before `L2_PWROFF`, so the shim skips `L2_PWROFF` if the shaders do not power off.

Review 1 follow-ups (2026-10-23, `docs/reviews/gpu-power-review-1.md`):
- **Dummy-job workaround does not apply** (*read from source*). kbase replaces `SHADER_PWRON` by `kbase_dummy_job_wa_execute()` only when `kbase_dummy_job_wa_enabled()` (pm_driver.c:644-647; also at GPU power-on, 3795-3799). `GPU_ID` 0x92020010 = product tBEx (Mali-G78), version r0p1: the table entry `{ GPU_ID_VERSION_MAKE(0, 1, 0), base_hw_issues_tBEx_r0p1 }` is hw.c:206-211; `base_hw_issues_tBEx_r0p1` (issues.h:345-361) does not contain `KBASE_HW_ISSUE_TTRX_3485`; the WA blob is loaded only for that issue (`wa_blob_load_needed`, mali_kbase_dummy_job_wa.c:217-226). So kbase writes the plain `SHADER_PWRON`. The bit decode of `GPU_ID` (arch_major [31:28], product_major [19:16], version_major [15:12], version_minor [11:4], status [3:0]) is the one `shim/gpuprobe.h` cites from kbase `mali_kbase_gpu_id.h:29-44`; the header itself is not among the local copies and an online fetch failed (HTTP 404) in this session, so the field layout is **unverified here**; `gpu_model_name(0x92020010)` = Mali-G78 (TBEX) agrees with the hw.c product switch (hw.c:62, 409) and the version fields 0/1/0 follow from that layout.
- **Steps kbase does that the shim skips** (and why): listed in the header comment of `shim/gpupower.c` (IRQ clear/mask pm_driver.c:3587-3602, soft reset 3763-3778, quirk registers 0xF00-0xF0C 4171-4187, L2 config override/PBHA 2161-2162, coherency and protected mode). The precheck instead requires `GPU_STATUS` = 0, `GPU_FAULTSTATUS` = 0 and `GPU_IRQ_RAWSTAT` = 0x100, and the run dumps `STACK_READY` (0xE10), `STACK_PWRTRANS` (0xE40, enums.h; not 0xE20), `L2_CONFIG` (0x48), `COHERENCY_ENABLE` (0x304), `GPU_IRQ_MASK` (0x28) read-only first.
- OFF path now matches kbase more closely: wait PWRTRANS 0, write PWROFF only if READY != 0 (value = READY = the present mask; a partial READY is reported `down=partial:*` and not written). Waits are bounded by polls and 1 s of CNTPCT; kbase's bound is 5 s (pm_driver.c:3244).
- Not verified: the *guess* that a `PWROFF` with a mask wider than READY is a no-op is no longer relied on (the shim never writes it).

## G2 stage 1 first hardware run (2026-10-23, `tools/probe-run --build-args "--pstore-fast --gpu-probe --gpu-power" --label g2power`, run dir `logs/probe/20261022T230930Z-g2power_`, shim built from commit with Opus re-review GO-WITH-FIXES applied)
*Observed on hardware.* Precheck passed (all PRE_ values as in G0; additionally PRE_STACK_READY=0, PRE_STACK_PWRTRANS=0, PRE_L2_CONFIG=0x07130000, COHERENCY_ENABLE=0, GPU_IRQ_MASK=0, JM/SHADER/TILER/L2_MMU_CONFIG=0).
- `TILER_PWRON=1` worked: TILER_READY=1, L2_READY=1, PWRTRANS 0, RAWSTAT 0x100 -> 0x700 (bits 9,10 = power-changed, read from source macros.h; guess for exact names), GPU_STATUS=3.
- `SHADER_PWRON=0x779FFF` did NOT complete: SHADER_PWRTRANS stayed 0x00779FFF, SHADER_READY 0 for the whole 1 s wait (summary `up=timeout:shader`, ticks on=0x014A4D06 (about 0.9 s at 24.576 MHz... freq register reads 0x01770000 = 24.576 MHz), off=0x05AD53E8). The OFF path then also timed out (`down=timeout:shader`, L2 correctly left on, no L2_PWROFF written). Phone reset itself at the end and came back (fastboot reachable); no persistent change.
- ACPM fvmap (run fvmap1): G3D domain (idx 5) init_lv=11 of 12 levels, G3DL2 (idx 6) init_lv=6 of 7: ABL leaves both at their lowest level, so a missing clock is not the obvious explanation (guess).
- Hypotheses for the stuck shader transition, none tested: (1) core stacks need `STACK_PWRON` (0xE20) first although kbase `MALI_CORESTACK` defaults to n in Mconfig (Pixel config unknown); (2) the shader cores need a clock/ACPM request that only DVFS issues; (3) EL3/TZPC state (`need_smc` / `exynos_pd_tz_restore`, research 17 section 5 unknown 1) or S2MPU; (4) the powered-up L2/tiler is not enough and `L2_CONFIG`/`PBHA` writes that kbase does before power-on matter. Next experiments (each a new writer, Opus review first): a single-core mask (`SHADER_PWRON=1`) and a `STACK_PWRON` variant.

## Addendum 2: `--gpu-power=stack` source table (2026-10-23, *read from source*, not run on hardware)

Variant of the sequence above for the stuck `SHADER_PWRTRANS` of the first run; kbase `corestack_driver_control = true` path (`bool corestack_driver_control = true` only if `CONFIG_MALI_CORESTACK`, pm_driver.c:62-66; Pixel setting unknown).

| Step | Register (LO word) | Offset (PA) | Value | kbase file:line |
|---|---|---|---|---|
| (pre) | STACK_PRESENT 0xE00, STACK_READY 0xE10, STACK_PWRTRANS 0xE40 | | present = soc.json `stack_present` 0x7F (*observed* G0), READY 0, PWRTRANS 0 | pm_driver.c:2593-2597 (`stacks_avail` = present cores) |
| ON 2 (new) | STACK_PWRON | 0xE20 (0x1C500E20) | `STACK_PRESENT` = 0x7F | pm_driver.c:2646-2649 (after the L2 is on, before the shaders); reg: enums.h:729, pm_driver.c:408-424 |
| (wait) | STACK_READY, STACK_PWRTRANS | | READY = 0x7F, PWRTRANS 0 | pm_driver.c:2656-2657 |
| ON 3 | SHADER_PWRON | 0x180 | `SHADER_PRESENT` | pm_driver.c:2658-2660 (as before) |
| OFF 1 | SHADER_PWROFF | 0x1C0 | `SHADER_PRESENT` | pm_driver.c:2846-2847 (as before) |
| OFF 2 (new) | STACK_PWROFF | 0xE30 (0x1C500E30) | 0x7F | pm_driver.c:2855-2859 (after `!shaders_trans && !shaders_ready`) |
| (wait) | STACK_READY, STACK_PWRTRANS | | both 0 | pm_driver.c:2866 |
| OFF 3 | L2_PWROFF | 0x1E0 | `L2_PRESENT` | as before |

Deviation from kbase: kbase writes `STACK_PWROFF = stacks_avail` unconditionally from `PEND_OFF_CORESTACK_ON`; the shim writes it only if `STACK_READY` != 0 (same rule as the other blocks) and drains `STACK_PWRTRANS` first. The shim also requires `STACK_READY` == 0 and `STACK_PRESENT` == 0x7F (HI 0) before any write.

## G2 stack variant run (2026-10-23, `--gpu-power=stack`, run `g2stack`, `logs/probe/20261022T232734Z-g2stack_`)
*Observed on hardware.* Precheck passed (`STACK_PRESENT` 0x7F, `STACK_READY` 0, `STACK_PWRTRANS` 0). `TILER_PWRON` worked as before. `STACK_PWRON` (LO word, 0x7F) was accepted (no fault, no refusal) but `STACK_READY` and `STACK_PWRTRANS` stayed 0 for the whole 1 s wait: `GPU POWER up=timeout:stack down=ok ticks=015D2501/04F12FF8 freq=01770000`. `SHADER_PWRON` was therefore never written; OFF powered the L2 and tiler off normally (`down=ok`, all `OFF_*_READY` 0).
Reading: the core stacks are not driver-controlled on this GPU/firmware (consistent with kbase `MALI_CORESTACK` `default n`, *guess* that Pixel's build keeps it off) or a LO-only write does not trigger them (*guess*, not distinguished). Either way the stack hypothesis for the stuck `SHADER_PWRTRANS` (hypothesis 1 above) is **dead**; the shaders must be unblocked by something else (clock/ACPM, EL3/TZPC/S2MPU, or the pre-power-on steps; hypotheses 2-4). The variant stays in the tree as an opt-in experiment; do not use it for further runs unless a new reason appears.

## Addendum 3: extra read-only dumps in `--gpu-probe` (2026-10-23, built, not yet run on hardware)
`shim/gpuprobe.c` now also prints (all through `try_read32`, no writes; host test `shim/gpuprobe_dump_host_test.c`, which does not even define `try_write32`; the existing gpuprobe object audit still shows stack-only stores). Source for the CMU table: `~/pixelios-artifacts/refs/kbase-bluejay/flexpmu_local.h` (copy of soc-gs `flexpmu_cal_local_gs101.h`, *read from source*; line numbers are that copy's). Printed only when the existing gate sees the top domain on (same rule as the old CMU reads):

| Output name | CMU_G3D offset (PA 0x1C400000 +) | Register (flexpmu_local.h name) | Source line |
|---|---|---|---|
| CMU_PLL_LOCKTIME | 0x004 | PLL_LOCKTIME_PLL_G3D / _L2 (same offset in both rows) | 494-495 |
| CMU_PLL_G3D (old name) / _CON1.._CON4 | 0x140 / 0x144 / 0x148 / 0x14C / 0x150 | PLL_CON0..CON4_PLL_G3D | 449 / 448 / 496 / 500 / 498 |
| CMU_PLL_G3D_L2 (old name) / _L2_CON1.._CON4 | 0x180 / 0x184 / 0x188 / 0x18C / 0x190 | PLL_CON0..CON4_PLL_G3D_L2 | 451 / 450 / 497 / 501 / 499 |
| CMU_MUX_CORE / _STACK / _TOP (old) | 0x600 / 0x610 / 0x620 | PLL_CON0_MUX_CLKCMU_EMBEDDED_G3D_{COREGROUP,STACKS,TOP}_USER | 452-454 |
| CMU_MUX_BUSD / _GLB / _SWITCH | 0x630 / 0x640 / 0x650 | PLL_CON0_MUX_CLKCMU_G3D_{BUSD,GLB,SWITCH}_USER | 493 / 492 / 491 |
| CMU_DIV_ADD_CH / _BUSP / _L2_GLB / _STACKS / _TOP | 0x1800 / 0x1804 / 0x1808 / 0x180C / 0x1810 | CLK_CON_DIV_CLK_G3D_ADD_CH_CLK, DIV_CLK_G3D_BUSP, _L2_GLB, _STACKS, _TOP | 490 / 489 / 488 / 487 / 486 |

Bit meanings are NOT assumed here (the sequences write PLL_CON0 bit 4 and PLL_CON1 = 0x30000003 at power-on, :448-451; PLL_CON3 bit 29 is polled as lock and bit 31 as bypass/skip, :502-505; all *guess* until compared with the dump).

ACPM fvmap (APM SRAM, not gated; read-only): soc.json `gpu` keys `fvmap_sram` = 0x0206A000 (= `acpm.sram_base` 0x02039000 + 0x31000, checked by `tools/boards.py`; observed valid in run `fvmap1`, docs/research/25: header type == index for all 14 domains), `fvmap_idx_g3d` = 5, `fvmap_idx_g3dl2` = 6 (observed in fvmap1: init_lv 11 of 12 levels / 6 of 7). Layout *read from source* in `xnu/patches/0097-sweet-fvmap-dump.patch` and soc-gs `cal-if/fvmap.h`: 24-byte header per index (`type`@0, `num_of_lv`@1, `num_of_members`@2, `init_lv`@8, `o_ratevolt` u16 @20), level table at fvmap base + `o_ratevolt`, 8 bytes per level (u32 kHz, u32 uV). Output: `GPU FVMAP5 type=.. nlv=.. members=.. init_lv=.. o_ratevolt=0x....` then one `GPU FVMAP<idx> lv=<n> kHz=<dec> uV=<dec>` per level with ` <- init_lv` on the init level; a header failing the patch's range check (0 or > 32 levels, table beyond 0x2000, unaligned) prints `REJECTED` and no levels; an unreadable word prints `unreadable` and the dump continues.

## Probe g0b result and the `--gpu-clk` experiment (2026-10-23; CORRECTED after Opus review, see below)
*Observed on hardware* (`logs/probe/20261022T233534Z-g0b_`, `--gpu-probe` with the extra dumps of addendum 3): `PLL_CON0` of `PLL_G3D` (0x140) and `PLL_G3D_L2` (0x180) = 0x10, which is the *PLL-output select* of the mux (CON0 bit 4), NOT an enable bit (the first reading, "both PLLs OFF", was wrong, see the CON3 decode below) (the user muxes 0x600/610/620/630 are set, `MUX_GLB`/`MUX_SWITCH` 0x640/0x650 = 0). `PLL_CON1` 0x10000001, `CON2` 0x8000000F, `CON3` 0xA0620402 (**bit 31 ENABLE and bit 29 LOCK both set**), `CON4` 0xF340, `PLL_LOCKTIME` 0x258; dividers `DIV_BUSP` (0x1804) = 2, `ADD_CH`/`L2_GLB`/`STACKS`/`TOP` = 0.
ACPM fvmap (soc.json now holds the rows): G3D (idx 5, 12 levels, `members` 3): 848000 762000 701000 572000 510000 471000 400000 351000 302000 251000 202000 **151000** kHz, uV 756250 ... 575000 / 562500 at init_lv 11 (151000 kHz). G3DL2 (idx 6, 7 levels): 996000 885000 750000 603000 434000 302000 **151000** kHz, init_lv 6 (151000 kHz, 600000 uV). Full tables in that run's `usb.txt`.
**CON3 decode** (*read from source* for the layout, *guess* that CMU_G3D's PLLs use the same type as CMU_TOP's): Linux treats `PLL_CON3` as the PLL control word of the gs101 PLLs (`PLL(pll_0517x, ..., PLL_CON3_PLL_SHARED0)`, clk-gs101.c:646-660); for pll_0516x/0517x/0518x ENABLE = bit 31, LOCK = bit 29, MDIV = [25:16], PDIV = [13:8], SDIV = [2:0] (clk-pll.c:424-431, 1375-1383); `PLL_CON0` bit 4 selects oscclk or the PLL output (`MUX(..., PLL_CON0_PLL_SHARED0, 4, 1)`, parents {oscclk, fout}, clk-gs101.c:664, 960; upstream Linux v6.12). 0xA0620402: ENABLE 1, LOCK 1, MDIV 0x62 = 98, PDIV 4, SDIV 2 => 24.576 MHz x 98 / (4 << 2) = 150.5 MHz = the firmware's 151000 kHz init level (an exact match, so the layout guess is very likely right); CON0 = 0x10 means the mux already selects the PLL. **Both GPU PLLs are probably already running at the init level**, so the ACPM clock request hypothesis is *refuted* (a same-rate FREQ_REQ is expected to do nothing) and the stuck `SHADER_PWRTRANS` of runs g2power/g2stack needs another explanation (next: the rail, `--gpu-rails`).

`--gpu-clk` (docs/AUTOMATION.md; **built, unrun; its premise is refuted by the CON3 decode above**) was written to test it with the existing, audited ACPM DVFS client. Source table, *read from source* (local copies in `~/pixelios-artifacts/refs/kbase-bluejay/`: `pixel_gpu_dvfs.c`, `gs101-gpu.dtsi`; soc-gs `scratch/dvfs-src/acpm_dvfs.c`, `cal-if.c`):

| What | Where | Detail |
|---|---|---|
| clock set | pixel_gpu_dvfs.c:52-70 `gpu_dvfs_set_freq` | `cal_dfs_set_rate(clks[domain].cal_id, table[level].clk[domain])` |
| -> ACPM | cal-if.c:85-102, acpm_dvfs.c:97-135 | `exynos_acpm_set_rate(GET_IDX(id), kHz)`: cmd = {idx, kHz, FREQ_REQ (0), ms}; `response = true` for G3D/G3DL2 (false only for MIF, INT, CPUCL0-2); return = reply word 3 |
| domain mapping | gs101-gpu.dtsi:46-47 | `gpu0_cmu_cal_id = ACPM_DVFS_G3DL2` (= `GPU_DVFS_CLK_TOP_LEVEL`), `gpu1_cmu_cal_id = ACPM_DVFS_G3D` (= `GPU_DVFS_CLK_SHADERS`); indices 6 / 5 observed (fvmap header type == index) |
| boot order | pixel_gpu_dvfs.c:859-896 `gpu_dvfs_set_initial_level` | loop `c = 0..COUNT`: G3DL2 first, then G3D, both at `level_min` (lowest throughput), called from `gpu_dvfs_init` (:983) at probe |
| runtime order | pixel_gpu_dvfs.c:105-114 `gpu_dvfs_set_new_level` | keeps `CLK_G3DL2 >= CLK_G3D`: clocking down G3D first, otherwise (up / equal) G3DL2 first. Ours (equal rates): G3DL2 then G3D |
| pre-condition | pixel_gpu_dvfs.c:95 | `set_new_level` returns early if the GPU power state is off; `set_initial_level` does not check. ABL leaves pd_g3d/pd_embedded_g3d on (G0), and the gpuprobe gate checks it |
| rate | soc.json `gpu.clk_khz` | 151000 for both = the fvmap `init_lv`. **Correction (review C2):** kbase's boot request is NOT 151000/151000: `level_min` is the last row of the DT table, `302000 151000` (gs101-gpu.dtsi:65 / :87; gpu0 = G3DL2, gpu1 = G3D; local copy `~/pixelios-artifacts/refs/kbase-bluejay/gs101-gpu.dtsi`), i.e. G3DL2 302000, G3D 151000; the "kbase does exactly that" claims in gpuclk.c and above were wrong |

ACPM gates for this client (soc.json `acpm`): `layout_verified` = **1** and `elem_unit_verified` = **1** (flipped 2026-10-22 after run `acpm3`: descriptor fields at the derived offsets, q_len 5, 16-byte slots, queues empty; the DVFS channel 1 GET was then observed on all three CPU clusters in run `dvfsr2`: `rep=000X0000 <kHz> 00000001 00000000`, 35 us). None of the ACPM flags is 0, so nothing blocks the run. What has never been observed: any GET/REQ for idx 5/6, and the FREQ_REQ *reply words* (a CPU FREQ_REQ through this client DID run: the runs `dvfs1-cl0..dvfs4-all` (HANDOFF session 13, commit c32fbd5) set the CPU rates with this shim client via `build-xnu-image --dvfs` and the CPUs then ran at about 1802/1835/1826 MHz, *observed* effect; patch 0096 only measured them; correction M4). If a gate were 0 the flag refuses with `flag: soc.json acpm.<name> = 0`.


Open findings of the Opus review of `--gpu-clk` (NOT fixed; the experiment is parked): C2 above; H1 (the `result: ok` check is meaningless: GET already returned 151000 before the request, a real result needs the before/after CON3 decode); M1 (`gpu_clk` does not read PMU_G3D_STAT/PMU_EMB_STAT before its request); M2 (shim.c runs the power cycle even after a failed/timed-out request); M3 (the write count: a run is 8 ACPM transactions, 6 GETs + 2 REQs, each writing the same 8 words: 4 request-slot words, the request front, the doorbell, the response rear, INTCR0); L1 (`acpm_dvfs_set` finds the GPU domains too; only the caller's loop keeps them out); L2 (two sequence-number streams on channel 1 if combined with `--touch-probe=dvfs`).

## Next hypothesis: the shader rail (2026-10-23, `--gpu-rails`, built, unrun)
With the PLLs already running (CON3 decode above) the stuck `SHADER_PWRTRANS` needs another cause. *Guess*: the shader cores sit on rail VDD_G3D = S2MPG11 BUCK2S (`S2S_VDD_G3D`; L2/tiler on BUCK8S `S8S_VDD_G3D_L2`, G3D_M on LDO1S `L1S_VDD_G3D_M`, doc 17 section 2.1, gs101-pmic.dtsi:760-773,850-860,936-946), all three `regulator-initial-mode = SEC_OPMODE_SUSPEND` ("ON in standby mode", s2mpg11-regulator.c:35-46), which may still be at a retention voltage while the L2/tiler rail came up. `--gpu-rails` (docs/AUTOMATION.md) reads the registers to show it; nothing is written.

| Item | Value | Source (GK = kernel/gs, android-gs-bluejay-5.10-android13-qpr3; local copies `~/pixelios-artifacts/scratch/acpm-src/`) |
|---|---|---|
| ACPM speedy channel / TYPE | 1 / 1 | s2mpg11-core.c:55,73,91,109,127 (`u8 channel = 1`), :29 (`I2C_ADDR_PMIC 0x01`); DT gs101-pmic.dtsi:710-718 (`acpm-speedy-channel = <1>`, `acpm-ipc-channel = <2>`); same derivation as S2MPG10 (0 / 1), doc 23 section 1 |
| BUCK2S | CTRL 0x15 (mode bits 7:6), OUT1 0x16, OUT2 0x17; uV = 200000 + 6250 * OUT1 (256 steps) | s2mpg11-register.h:48-50, :308-309, :335; s2mpg11-regulator.c:259-260 |
| BUCK8S | CTRL 0x25 (7:6), OUT1 0x26, OUT2 0x27; same formula | register.h:64-66; regulator.c:271-272 |
| LDO1S | enable = LDO_CTRL1 0x52 bits 5:4; vsel = L1S_CTRL1 0x41 & 0x7F (128 steps), CTRL2 0x42; uV = 300000 + 12500 * sel | register.h:79-80, :87 (LDO_CTRL1), :314-315, :334; regulator.c:227-228 |
| mode field | 0 off, 1 ON in standby (SUSPEND), 2 ON in PWREN_MIF mode, 3 ON normal | regulator.c:35-46 (`s2mpg11_of_map_mode`), `s2m_is_enabled` = (val & mask) != 0 |
| STATUS1 | 0x0C (raw, meaning unknown) | register.h:41 |
| *guess* | OUT1 is the live output (Linux reads/writes OUT1; OUT2 may be selected by a DVS pin or mode) | |

The first hardware run of `--gpu-rails` has not happened; the decode lines in its output are tagged accordingly.

## Run g1rails2 result: the shader rail is OFF; `--gpu-rail-on` (2026-10-23)
*Observed on hardware* (`logs/probe/20261023T001506Z-g1rails2_`, `--gpu-probe --gpu-rails`, read-only): S2MPG11 **BUCK2S** (S2S_VDD_G3D, the shader rail) `B2S_CTRL` (0x15) = 0x38 => mode bits 7:6 = 0 = **off**, `B2S_OUT1` (0x16) = 0x40 (600000 uV by the source formula), `B2S_OUT2` (0x17) = 0x30; **BUCK8S** (L2 rail) `B8S_CTRL` (0x25) = 0xD8 mode 3 on, OUT1 0x40 (600000 uV), OUT2 0x30; **LDO1S** (L1S_VDD_G3D_M) `L1S_CTRL1` 0x24 (750000 uV), `LDO_CTRL1` (0x52) = 0xF0 => mode 3 on; `STATUS1` (0x0C) = 0x01. Reading (mode field and formulas *read from source*, the OUT1-is-live part a *guess*): the rail is OFF. *Guess* (until a `--gpu-rail-on` run): the shader cores cannot power because their rail is off (ABL never needed the GPU); this fits tiler + L2 up and the shaders stuck in PWRTRANS.
The first reading "mode 1 = SUSPEND = standby/retention" (earlier section) was not evidence of anything (Opus review of --gpu-rails M1): the DT gives that mode to the CPU rails too; the relabelled output says so.

`--gpu-rail-on` (built, unrun, awaiting the Opus review of this writer) turns BUCK2S on for the duration of the power cycle and restores it. **Every write the option can make** (besides the 8 IPC words of every ACPM transaction, which are acpm.c's audited writes, doc 23; a failed OUT1 read is only a read: it is not counted as a published update and triggers no restore alarm, review L1):

| # | When | Target (ACPM request) | Command word 1 | Meaning | Source |
|---|---|---|---|---|---|
| W1 | enable, only if BUCK2S_CTRL reads mode 0 and OUT1 in 56..80 (550000..700000 uV) | S2MPG11 (speedy channel 1, TYPE 1) register **0x15** `B2S_CTRL`, FUNC_UPDATE, request word 0 = 0x1115 \| seq << 16 | `0x00C04002` = FUNC_UPDATE (2) \| val 0x40 << 8 \| mask 0xC0 << 16 | mode field bits 7:6 := 1 (SUSPEND = the value Linux writes for this rail); the APM does the read-modify-write, bits 5:0 untouched | reg: s2mpg11-register.h:48; mask: S2MPG11_REG_ENABLE_MASK_7_6, register.h:335, BUCK2S descriptor regulator.c:259-260; mode 1: DT `regulator-initial-mode = SEC_OPMODE_SUSPEND` + `regulator-always-on` (gs101-pmic.dtsi:766-779) -> `s2mpg11_of_map_mode` 1 (regulator.c:35-46) -> `s2m_set_mode` stores opmode[id] = 1 << 6 (:48-73; probe default opmode[id] = enable_mask, :718) -> `s2m_enable` writes opmode[id] under the mask (:75-91); framing: acpm_mfd.c update (doc 23 section 1.1, *observed* for S2MPG10 in rails2) |
| W2 | restore, always after the power cycle, only if the mode is not already 0 | same register, FUNC_UPDATE | `0x00C00002` = 2 \| val 0x00 << 8 \| mask 0xC0 << 16 | mode field := 0 (off) | `s2m_disable` writes 0 under the enable mask, regulator.c:95-111 |
No other PMIC register is written: not 0x16/0x17 (voltage), not any other rail. The pre-conditions are checked inside `acpm_s11_rail_set` itself (pre-read must show mode 0, OUT1 in the window), so a wrong caller cannot widen it. Reads: 0x15, 0x16 (pre-read/read-back), plus the ten of `--gpu-rails`.
Soft start: `S2MPG11_ENABLE_TIME_BUCK` = 130 us (register.h:339, the regulator core's `enable_time`); the shim waits 8x that, then reads back once (the margin is a *guess*). Voltage window: fvmap G3D init level 562500 uV (run g0b), observed 600000 uV, Linux DT window of the rail 450000..1300000 uV (gs101-pmic.dtsi:766-768).
*Decision (review M1)*: the enable value is mode 1 = 0x40, *read from source* as what Linux writes for this rail (cites in W1); the first version wrote mode 3 (0xC0), which Linux never writes to it. BUCK8S sits at mode 3 (ABL's choice, observed 0xD8). A leftover 0x40 equals stock Android; a leftover 0xC0 would persist into Android (Linux does not rewrite an already-enabled rail).
A rail left on (hang mid-cycle or `restore=FAILED`) cannot be cleared by the shim (the next enable refuses: mode != 0, and does no restore); a PSCI reset is a SoC reset, not a PMIC reset, so S2MPG11 probably keeps the value (*guess*); at 0x40 that is the stock state.
Run order: first `--gpu-rail-on` alone (enable, read back, restore: `GPU RAILON enable=ok readback=ok restore=ok power=not-requested`), then with `--gpu-power`; not with `--gpu-clk`. Before the restore the shader state is logged (`GPU RESTORE_SHADER_READY/PWRTRANS`, TILER, L2 READY). Review I2: the Linux GPU driver holds no regulator handle, `embedded_g3d_on` has no PMIC step (flexpmu_local.h:447-458), so no domain re-sequence is needed by source (*guess* on module order).
Sequence safety: a failed enable (refused, error reply, mismatch) skips the power cycle (same for a failed `--gpu-clk`, review M2); the restore runs after the power cycle in every case except "refused, nothing written", and says `!!! RESTORE FAILED ... may be LEFT ON` if it could not put the rail back.


## RESULT: GPU fully powered (2026-10-23, runs railon1 and railon2, `logs/probe/20261023T004818Z-railon1_`, `...T005128Z-railon2_`)
*Observed on hardware.* Root cause of the stuck shader transition: the shader rail S2MPG11 BUCK2S (VDD_G3D) was OFF after ABL (CTRL 0x38, mode 0; run g1rails2). `--gpu-rail-on` (ACPM PMIC update of B2S_CTRL mask 0xC0 value 0x40 = what Linux writes, voltage regs untouched) turned it on (CTRL 0x78), voltage unchanged (OUT1 0x40 = 600 mV), restored to 0x38 afterwards (both runs `restore=ok`). With the rail on, `--gpu-power` gave `POWER up=ok down=ok ticks=00000B00/0069047E`: TILER_PWRON then SHADER_PWRON 0x779FFF -> SHADER_READY=0x00779FFF, TILER_READY=1, L2_READY=1, PWRTRANS 0, GPU_FAULTSTATUS 0, and the OFF sequence returned everything to 0. All 20 shader cores of the G78 MP20 power up (ticks ON = 0xB00 = 2816 ticks = 0.11 ms at 24.576 MHz).
Refuted hypotheses: core stacks need STACK_PWRON (they do not), GPU PLLs off (PLL_CON3 decode: running at ~150.5 MHz), missing clock request (never needed for this step).
**Consequence for the architecture (guess):** the shim can enable the rail (ACPM, which XNU has no client for) and power the cores before handing over to XNU and leave them on; XNU then only needs GPU registers (stage 2: MMU/address space + job submit, open unknowns: G3D S2MPU DMA permission, research 17 section 5 item 3).
