#pragma once
#include <stdint.h>

/*
 * gpujob.h - opt-in G2 stage 2 (build.sh --gpu-job): program address space 0, run ONE Valhall WRITE_VALUE job that makes the GPU write a magic 64-bit value
 * to a RAM word through its MMU, and report. Runs INSIDE the powered window of gpu_power_cycle() (gpupower.c calls gpu_job_run() once the shaders are up);
 * everything is switched off again by the OFF sequence that follows. Design, every register / field / value with its cite: docs/research/28-gpu-stage2-job.md.
 *
 * gpu_job_run(): read-only preconditions first (zero writes unless all hold), then the S2MPU dump, the page tables + job descriptor in the shim-owned scratch
 * (5 pages, see gpu_job_scratch), AS0 UPDATE, JS0 submit, bounded wait (100 ms of CNTPCT), a result report, and ALWAYS: IRQ raw bits cleared, AS0 set back to
 * UNMAPPED. Returns 0 only if the target word holds the magic value afterwards.
 * Writes: exactly the pairs of w32_allowed() in gpujob.c (docs/research/28 section 7) and the scratch window; no GPU_COMMAND, no reset.
 */
int gpu_job_run(void);

/* Provided by shim.c (outside the audited writer): 5 pages (20 KiB), 4 KiB aligned, inside the shim image's own RAM, used for nothing else. */
uintptr_t gpu_job_scratch(void);
/* Provided by shim.c: clean+invalidate the data cache lines of [pa, pa+len) to the point of coherency (DC CIVAC; the shim runs MMU-off, so VA == PA). */
void gpu_job_cache_flush(uintptr_t pa, unsigned len);

/* Fault-guarded 32-bit MMIO / RAM access and the counter (shim.c, console.c; host tests substitute fakes). */
int try_read32(uintptr_t address, uint32_t *value);
int try_write32(uintptr_t address, uint32_t value);
uint64_t touch_ticks(void);
uint64_t counter_freq(void);
