#pragma once

/*
 * gpuclk.h - opt-in GPU clock request through the audited ACPM DVFS client (build.sh --gpu-clk, implies --gpu-probe, gs101 only).
 *
 * gpu_clk(): refuse (writes nothing, prints why) unless the DVFS channel is configured, the ACPM gates layout_verified and
 * elem_unit_verified are set, the descriptor validates and both queues are empty; GET G3DL2 (ACPM idx 6) and G3D (idx 5); ask for the
 * firmware's own init rate (soc.json gpu.clk_khz = 151000 kHz) for G3DL2 and then G3D, even if GET already reports it (Linux sends
 * it at every probe; the point is to make the APM act on it); GET both back; print the CMU_G3D words again. Returns 0 only if both
 * read back the requested rate. The only writes are the ones acpm.c's audited writer makes for those two FREQ_REQ transactions;
 * this file has no MMIO writer of its own (audited: no try_write32 reference, stack-only stores). Output lines: GPU CLK ...
 * (docs/AUTOMATION.md).
 */
int gpu_clk(void);
