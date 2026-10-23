/* Host test for gpus2mpu.c (--gpu-s2mpu): fake PMU + four S2MPU register windows. try_write32 is deliberately NOT defined, so linking fails if the dump
 * ever references a writer. Uses the REAL generated board macros. Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -I. -Iout -include out/board_config.h -o out/gpus2mpu_host_test gpus2mpu_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gpus2mpu.c"

#if !(SOC_GS101 && defined(GPU_S2MPU_G3D0))
#error "needs the bluejay board_config.h"
#endif

static char lines[1024][RAMLOG_LINE_MAX + 8];
static unsigned nl;
static uint32_t top_stat = 1;
static uintptr_t bad_pa;
static unsigned reads;

void con_str(const char *label, const char *s)
{
	assert(nl < 1024);
	assert(strlen(s) < RAMLOG_LINE_MAX);
	snprintf(lines[nl++], sizeof(lines[0]), "%s %s", label, s);
}

/* register file: CTRL0 0x1 on instance 0, 0 elsewhere; L1 attr: VID0 granules 0 and 33 (0x8_0000_0000 / 1 GiB = 32 ... see below) open RW */
static uint32_t regval(uintptr_t pa)
{
	for (unsigned i = 0; i < 4; i++) {
		uintptr_t b = i == 0 ? GPU_S2MPU_G3D0 : i == 1 ? GPU_S2MPU_G3D1 : i == 2 ? GPU_S2MPU_G3D2 : GPU_S2MPU_G3D3;

		if (pa >= b && pa < b + 0x10000) {
			uintptr_t off = pa - b;

			if (off == 0)
				return i == 0 ? 1 : 0;
			if (off == 0x2000)
				return 0x80 + i;			/* FAULT_STATUS */
			if (off >= 0x4000 && off < 0x4000 + 2 * 0x200 && (off & 7) == 4) {
				unsigned vid = (off - 0x4000) / 0x200, m = ((off - 0x4000) % 0x200) / 8;

				if (vid == 0 && (m == 2 || m == 32))
					return 6;			/* RD | WR, no L2 table */
				if (vid == 0 && m == 33)
					return 1;			/* L2 table */
				return 0;
			}
			return 0x10000000u + (uint32_t)off;
		}
	}
	return 0;
}

int try_read32(uintptr_t pa, uint32_t *v)
{
	reads++;
	if (pa == bad_pa)
		return 0;
	if (pa == GPU_PMU_G3D_STAT) {
		*v = top_stat;
		return 1;
	}
	*v = regval(pa);
	return 1;
}

static int has(const char *s)
{
	for (unsigned i = 0; i < nl; i++)
		if (strstr(lines[i], s))
			return 1;
	return 0;
}

#define EXPECT(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); for (unsigned i_ = 0; i_ < nl; i_++) fprintf(stderr, "  %s\n", lines[i_]); assert(0); } } while (0)

int main(void)
{
	/* probe_pa 0x80080000 -> granule 2 (0x80000000 >> 30 = 2): open RW on VID 0 of every instance */
	gpu_s2mpu_dump(0x80080000ul);
	EXPECT(has("GPU S2MPU probe_pa=080080000 gb_index=02"));
	EXPECT(has("GPU S2MPU0 CTRL0 pa=020080000 v=00000001") && has("GPU S2MPU1 CTRL0 pa=020090000 v=00000000") && has("GPU S2MPU3 CTRL0 pa=0200B0000 v=00000000"));
	EXPECT(has("GPU S2MPU2 CFG pa=0200A0010 v=10000010") && has("GPU S2MPU0 FAULT_STATUS pa=020082000 v=00000080") && has("GPU S2MPU3 FAULT_STATUS pa=0200B2000 v=00000083"));
	/* granules 2 and 32 RD+WR; granule 33 has an L2 table: masks are bit 2 | bit 32 and bit 33 */
	EXPECT(has("GPU S2MPU0 vid0 rdwr=0000000100000004 l2=0000000200000000 nonzero=03 bad=00"));
	EXPECT(has("GPU S2MPU0 vid1 rdwr=0000000000000000 l2=0000000000000000 nonzero=00 bad=00"));
	EXPECT(has("GPU S2MPU2 vid0 rdwr=0000000100000004"));
	EXPECT(has("GPU S2MPU0 L1ATTR0 pa=020084014 v=00000006") && has("GPU S2MPU0 L1ATTR1 pa=020084214 v=00000000"));
	EXPECT(has("GPU S2MPU0 FAULT_INFO0 pa=020082010") && has("GPU S2MPU0 FAULT_PA_LOW1 pa=020082024"));

	/* a faulting read is printed and counted, the dump goes on */
	nl = 0; bad_pa = GPU_S2MPU_G3D1 + 0x4000 + 8 * 2 + 4;
	gpu_s2mpu_dump(0x880000000ull);			/* granule 0x22 = 34 */
	EXPECT(has("gb_index=22") && has("GPU S2MPU1 vid0 rdwr=0000000100000000 l2=0000000200000000 nonzero=02 bad=01"));
	EXPECT(has("GPU S2MPU3 vid1 rdwr=") && has("L1ATTR0 pa=020084044") == 0);

	/* pd_g3d off: nothing but the skip line, not one S2MPU read */
	nl = 0; bad_pa = 0; top_stat = 0; reads = 0;
	gpu_s2mpu_dump(0x80080000ul);
	EXPECT(has("S2MPU skipped: pd_g3d is not on") && !has("CTRL0") && reads == 1);
	puts("PASS gpus2mpu host test");
	return 0;
}
