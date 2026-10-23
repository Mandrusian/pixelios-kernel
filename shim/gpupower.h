#pragma once
#include <stdint.h>

/*
 * gpupower.h - opt-in Mali-G78 power cycle (docs/research/17-gpu-kernel-gs101.md section 2.4, stage 1).
 * Linked only with build.sh --gpu-power (gs101, implies --gpu-probe: the gate lives in gpuprobe.h).
 *
 * gpu_power_cycle(): refuse with ZERO writes unless the preconditions hold (gate open, GPU_ID and the PRESENT masks
 * equal the soc.json values, every READY/PWRTRANS word is 0); then power the tiler+L2 and the shader cores ON,
 * report, and ALWAYS power them OFF again in kbase order, back to the state ABL left. Returns 0 when up and down
 * both succeeded, 1 otherwise (including a refusal). Output lines: see docs/AUTOMATION.md.
 */
int gpu_power_cycle(void);

/* Fault-guarded 32-bit MMIO write (shim.c). Called ONLY from w32 in gpupower.c (audited). */
int try_write32(uintptr_t address, uint32_t value);

uint64_t touch_ticks(void);	/* CNTPCT (shim.c, built with TOUCH_PROBE or GPU_POWER) */
uint64_t counter_freq(void);	/* console.c */
