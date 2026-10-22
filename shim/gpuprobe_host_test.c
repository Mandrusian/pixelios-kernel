/* Host test for the pure decoders and the safety gate in gpuprobe.h. Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -I. -o out/gpuprobe_host_test gpuprobe_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gpuprobe.h"

int try_read32(uintptr_t address, uint32_t *value)
{
	(void)address; (void)value;
	return 0;
}

int main(void)
{
	/* Mali-G78 (kbase TBEX): arch_major 9, product_major 2; arch_minor and version bits are free. */
	uint32_t g78 = 0x92021000u;
	assert(gpu_id_arch_major(g78) == 9);
	assert(gpu_id_arch_minor(g78) == 2);
	assert(gpu_id_product(g78) == 2);
	assert(gpu_id_ver_major(g78) == 1);
	assert(gpu_id_ver_minor(g78) == 0);
	assert(gpu_id_ver_status(g78) == 0);
	assert(!strcmp(gpu_model_name(g78), "Mali-G78"));
	assert(!strcmp(gpu_model_name(0x90020000u), "Mali-G78"));
	assert(!strcmp(gpu_model_name(0x90010000u), "Mali-G57"));
	assert(!strcmp(gpu_model_name(0xA0020000u), "Mali-G710"));
	assert(!strcmp(gpu_model_name(0x12345678u), "?"));
	assert(gpu_is_jm(g78));
	assert(!gpu_is_jm(0xA0020000u));

	assert(gpu_popcount32(0) == 0);
	assert(gpu_popcount32(0x000FFFFFu) == 20);	/* MP20 */
	assert(gpu_popcount32(0xFFFFFFFFu) == 32);

	/* The gate only opens when everything is on and readable. */
	assert(gpu_gate(1, 1, 1, 1, 1, 0x10, 0x10, 0x10) == GPU_GATE_OPEN);
	assert(gpu_gate(0, 1, 1, 1, 1, 0x10, 0x10, 0x10) == GPU_GATE_PMU_UNREADABLE);
	assert(gpu_gate(1, 1, 0, 1, 1, 0x10, 0x10, 0x10) == GPU_GATE_PMU_UNREADABLE);
	assert(gpu_gate(1, 0, 1, 1, 1, 0x10, 0x10, 0x10) == GPU_GATE_TOP_OFF);
	assert(gpu_gate(1, 0, 1, 0, 0, 0, 0, 0) == GPU_GATE_TOP_OFF);	/* top off wins: CMU not touched */
	assert(gpu_gate(1, 1, 1, 0, 1, 0x10, 0x10, 0x10) == GPU_GATE_CORES_OFF);
	assert(gpu_gate(1, 3, 1, 3, 0, 0, 0, 0) == GPU_GATE_CMU_UNREADABLE);	/* only bit 0 counts in STATUS */
	assert(gpu_gate(1, 1, 1, 1, 1, 0x10, 0x00, 0x10) == GPU_GATE_MUX_OFF);
	assert(gpu_gate(1, 1, 1, 1, 1, 0x10, 0x10, 0x00) == GPU_GATE_MUX_OFF);
	assert(gpu_gate(1, 1, 1, 1, 1, 0x1, 0x1, 0x1) == GPU_GATE_MUX_OFF);	/* other bits do not count */
	assert(!strcmp(gpu_gate_name(GPU_GATE_TOP_OFF), "pd_g3d-off"));
	puts("PASS gpuprobe decoders and gate");
	return 0;
}
