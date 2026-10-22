/*
 * gpuprobe.c - read-only Mali-G78 probe (see gpuprobe.h, docs/research/17).
 *
 * Every access is try_read32(); a bus fault prints "unreadable" and the probe
 * continues. No MMIO stores of any kind, no SMC, no clock/power change; the
 * object is audited by tools/test-bluejay-boot.py with the same
 * stack-only-stores policy as usbprobe.o and wlanprobe.o. No static state.
 *
 * Register sources ([src] = Android kernel, branch android-gs-bluejay-6.1-android16):
 *  - PMU (0x17460000) G3D_CONFIGURATION +0x1E00 / G3D_STATUS +0x1E04 and
 *    EMBEDDED_G3D_CONFIGURATION +0x2000 / _STATUS +0x2004: soc-gs
 *    drivers/soc/google/cal-if/gs101/flexpmu_cal_local_gs101.h (g3d_on, g3d_status,
 *    embedded_g3d_on, embedded_g3d_status); the DT also gives 0x2004 as
 *    gpu_pmu_status_reg_offset (kernel/devices/google/gs101 dts/gs101-gpu.dtsi).
 *    Linux reads these with readl (pmucal_rae.c:131,145), so AP reads are allowed.
 *  - CMU_G3D (0x1C400000) PLL_CON0_MUX_CLKCMU_EMBEDDED_G3D_{COREGROUP,STACKS,TOP}_USER at
 *    +0x600/+0x610/+0x620, PLL_CON0_PLL_G3D +0x140, PLL_CON0_PLL_G3D_L2 +0x180: same file.
 *  - GPU registers (0x1C500000, JM layout): gpu/mali_kbase hw_access/regmap/
 *    mali_kbase_regmap_jm_enums.h (GPU_CONTROL__* offsets in the comments),
 *    mali_kbase_regmap_jm.c:1814-1825 (MMU at 0x2000).
 */
#include <stdint.h>
#include "console.h"
#include "ramlog.h"
#include "gpuprobe.h"

#if SOC_GS101 && defined(GPU_MALI)

#define CMU_MUX_COREGROUP	0x600u
#define CMU_MUX_STACKS		0x610u
#define CMU_MUX_TOP		0x620u
#define CMU_PLL_G3D		0x140u
#define CMU_PLL_G3D_L2		0x180u

static unsigned put_txt(char *dst, unsigned cap, unsigned pos, const char *s)
{
	while (*s && pos + 1 < cap)
		dst[pos++] = *s++;
	return pos;
}

static unsigned put_hex(char *dst, unsigned cap, unsigned pos, uint64_t v, unsigned digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (unsigned i = 0; i < digits && pos + 1 < cap; i++)
		dst[pos++] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	return pos;
}

static void emit(char *buf, unsigned pos)
{
	if (pos >= RAMLOG_LINE_MAX)
		pos = RAMLOG_LINE_MAX - 1;
	buf[pos] = 0;
	con_str("GPU", buf);
}

/* "<name> pa=XXXXXXXXX v=XXXXXXXX" or "... unreadable"; returns 1 and *v when read. */
static int rd(const char *name, uintptr_t pa, uint32_t *v)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = 0;
	int ok = try_read32(pa, v);
	p = put_txt(line, sizeof(line), p, name);
	p = put_txt(line, sizeof(line), p, " pa=");
	p = put_hex(line, sizeof(line), p, pa, 9);
	if (ok) {
		p = put_txt(line, sizeof(line), p, " v=");
		p = put_hex(line, sizeof(line), p, *v, 8);
	} else {
		p = put_txt(line, sizeof(line), p, " unreadable");
	}
	emit(line, p);
	return ok;
}

static void summary(enum gpu_gate gate, int id_ok, uint32_t id, unsigned cores)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, "SUMMARY gate=");
	p = put_txt(line, sizeof(line), p, gpu_gate_name(gate));
	if (id_ok) {
		p = put_txt(line, sizeof(line), p, " id=");
		p = put_hex(line, sizeof(line), p, id, 8);
		p = put_txt(line, sizeof(line), p, " ");
		p = put_txt(line, sizeof(line), p, gpu_model_name(id));
		p = put_txt(line, sizeof(line), p, gpu_is_jm(id) ? " JM cores=" : " CSF cores=");
		p = put_hex(line, sizeof(line), p, cores / 10, 1);	/* decimal, cores < 100 */
		p = put_hex(line, sizeof(line), p, cores % 10, 1);
	}
	emit(line, p);
}

void gpu_probe(void)
{
	uint32_t v, top = 0, emb = 0, mt = 0, mc = 0, ms = 0, id = 0, sh = 0;
	int top_ok, emb_ok, mux_ok = 0, id_ok = 0;
	enum gpu_gate gate;

	/* Always-on PMU: the only thing read unconditionally. */
	rd("PMU_G3D_CFG", GPU_PMU_G3D_CFG, &v);
	top_ok = rd("PMU_G3D_STAT", GPU_PMU_G3D_STAT, &top);
	rd("PMU_EMB_CFG", GPU_PMU_EMB_CFG, &v);
	emb_ok = rd("PMU_EMB_STAT", GPU_PMU_EMB_STAT, &emb);

	/* CMU_G3D lives in the top domain: touch it only when that is powered. */
	if (top_ok && (top & 1u)) {
		int a = rd("CMU_MUX_TOP", GPU_CMU_G3D + CMU_MUX_TOP, &mt);
		int b = rd("CMU_MUX_CORE", GPU_CMU_G3D + CMU_MUX_COREGROUP, &mc);
		int c = rd("CMU_MUX_STACK", GPU_CMU_G3D + CMU_MUX_STACKS, &ms);
		mux_ok = a && b && c;
		rd("CMU_PLL_G3D", GPU_CMU_G3D + CMU_PLL_G3D, &v);
		rd("CMU_PLL_G3D_L2", GPU_CMU_G3D + CMU_PLL_G3D_L2, &v);
	}

	gate = gpu_gate(top_ok, top, emb_ok, emb, mux_ok, mt, mc, ms);
	if (gate != GPU_GATE_OPEN) {
		summary(gate, 0, 0, 0);
		return;
	}

	/* Gate open: GPU_CONTROL registers (all RO or RAWSTAT), then JOB/MMU RAWSTAT. */
	id_ok = rd("GPU_ID", GPU_MALI + 0x000, &id);
	rd("L2_FEATURES", GPU_MALI + 0x004, &v);
	rd("TILER_FEATURES", GPU_MALI + 0x00c, &v);
	rd("MEM_FEATURES", GPU_MALI + 0x010, &v);
	rd("MMU_FEATURES", GPU_MALI + 0x014, &v);
	rd("AS_PRESENT", GPU_MALI + 0x018, &v);
	rd("JS_PRESENT", GPU_MALI + 0x01c, &v);
	rd("GPU_IRQ_RAWSTAT", GPU_MALI + 0x020, &v);
	rd("GPU_STATUS", GPU_MALI + 0x034, &v);
	rd("GPU_FAULTSTATUS", GPU_MALI + 0x03c, &v);
	rd("REVIDR", GPU_MALI + 0x280, &v);
	rd("THREAD_MAX_THREADS", GPU_MALI + 0x0a0, &v);
	rd("THREAD_FEATURES", GPU_MALI + 0x0ac, &v);
	rd("SHADER_PRESENT_LO", GPU_MALI + 0x100, &sh);
	rd("SHADER_PRESENT_HI", GPU_MALI + 0x104, &v);
	rd("TILER_PRESENT", GPU_MALI + 0x110, &v);
	rd("L2_PRESENT", GPU_MALI + 0x120, &v);
	rd("STACK_PRESENT", GPU_MALI + 0xe00, &v);
	rd("SHADER_READY", GPU_MALI + 0x140, &v);
	rd("L2_READY", GPU_MALI + 0x160, &v);
	rd("COHERENCY_FEATURES", GPU_MALI + 0x300, &v);
	rd("JOB_IRQ_RAWSTAT", GPU_MALI + 0x1000, &v);
	rd("MMU_IRQ_RAWSTAT", GPU_MALI + 0x2000, &v);
	summary(gate, id_ok, id, gpu_popcount32(sh));
}

#else	/* not gs101 (or no GPU addresses): empty stub */

void gpu_probe(void) {}

#endif
