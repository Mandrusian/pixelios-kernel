#pragma once

/*
 * gpurails.h - READ-ONLY dump of the S2MPG11 registers of the GPU rails (build.sh --gpu-rails, implies --gpu-probe, gs101 only).
 *
 * gpu_rails(): refuse (zero ACPM traffic, prints why) unless the soc.json acpm.s2mpg11 block is fully known, the ACPM gates
 * layout_verified / elem_unit_verified are set, the PMIC channel descriptor validates and both queues are empty; then READ the ten registers
 * of soc.json acpm.s2mpg11.regs (BUCK2S = S2S_VDD_G3D shaders, BUCK8S = S8S_VDD_G3D_L2, LDO1S = L1S_VDD_G3D_M) through acpm_s11_read() and print them
 * as `GPU RAIL <name> reg=0x.. v=0x..` plus a decoded line per rail. The first failed read stops the rest (no retry). Returns 0 if all ten were read.
 * The only ACPM traffic is that many FUNC_READ transactions (each makes the audited IPC writes of acpm.c: 4 request words, the request front, the
 * doorbell, the reply rear, INTCR0); no PMIC register is ever written. gpurails.c has no MMIO access of its own (audited).
 */
int gpu_rails(void);
