#pragma once
#include <stdint.h>

/*
 * gpus2mpu.h - STRICTLY read-only dump of the four G3D S2MPU blocks (build.sh --gpu-s2mpu, implies --gpu-probe, gs101 only; also run by --gpu-job).
 *
 * The S2MPU (stage-2 memory protection unit, "s2mpu_g3d0..3") sits between the GPU's AXI masters and DRAM; whether GPU DMA may reach shim RAM is open unknown #3
 * of docs/research/17 section 5. This prints the control/fault registers and, per VID 0 and 1, which of the 64 one-gigabyte granules are open for read+write
 * and which use a 2nd-level table, plus the exact L1 entry of the granule that contains `probe_pa` (the PA the caller cares about). Nothing is ever written.
 * Register map: soc-gs drivers/soc/google/s2mpu/s2mpu-regs.h (docs/research/28). Printed only if the PMU says pd_g3d (the S2MPU's power domain,
 * power-domain-id 0x1C410204) is on; every read is fault-guarded.
 */
void gpu_s2mpu_dump(uintptr_t probe_pa);

/* Fault-guarded 32-bit MMIO read (shim.c; host tests substitute a fake window). */
int try_read32(uintptr_t address, uint32_t *value);
