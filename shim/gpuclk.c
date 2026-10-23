/*
 * gpuclk.c - opt-in GPU clock request (see gpuclk.h). Freestanding, no static state, no MMIO of its own.
 *
 * Order and domains = Google's Pixel kbase platform code, ~/pixelios-artifacts/refs/kbase-bluejay/pixel_gpu_dvfs.c [read from source]:
 *  - gpu_dvfs_set_freq() (:52-70) = cal_dfs_set_rate(clks[domain].cal_id, table[level].clk[domain]) -> exynos_acpm_set_rate(GET_IDX(id), rate)
 *    (soc-gs cal-if/cal-if.c:85-102, acpm_dvfs.c:97-135: cmd = {idx, kHz, FREQ_REQ, ms}, response = TRUE for every domain but MIF/INT/CPUCL*,
 *    i.e. for G3D / G3DL2; return = reply word 3).
 *  - gpu_dvfs_set_initial_level() (:859-896) loops c = 0..GPU_DVFS_CLK_COUNT: GPU_DVFS_CLK_TOP_LEVEL (= G3DL2, DT gpu0_cmu_cal_id, gs101-gpu.dtsi:46)
 *    first, then GPU_DVFS_CLK_SHADERS (= G3D, gpu1_cmu_cal_id, :47), both at the lowest throughput level (level_min), under the
 *    power-domain access lock. gpu_dvfs_set_new_level() (:82-130) enforces CLK_G3DL2 >= CLK_G3D: when the level index goes up (clocking down)
 *    G3D first, otherwise G3DL2 first; for "same rate" (our case, both 151000 kHz) the else branch applies: G3DL2 then G3D.
 *  - Pre-conditions in kbase: the GPU power domain is on (gpu_pm_get_power_state_nolock, :95); for the initial level kbase does not check it.
 *    Here the ABL leaves pd_g3d/pd_embedded_g3d on (doc 27 G0); NOT checked by this file (open finding M1).
 * STATUS: built, unrun; premise refuted (PLL_CON3 decode, doc 27). OPEN review findings, not fixed: C2 (Linux asks G3DL2 302000 / G3D 151000, gs101-gpu.dtsi:65/:87),
 * H1 (result check is meaningless: GET already returns 151000), M1 (no PMU_G3D/EMB_STAT read before the request), M2 (shim.c runs the power cycle after a failed request).
 * What is NOT known (guess): that the firmware starts the PLLs on a FREQ_REQ for a rate its table already reports as current; kbase's
 * set_initial_level does exactly that, so Linux relies on it; and that level_min of the Pixel DVFS table is the 151000 kHz row.
 *
 * Writes: none here. acpm_dvfs_reassert() (acpm.c, GPU_CLK images) refuses any domain but G3D / G3DL2 and any rate but a row of the firmware table at
 * or below the ceiling (= 151000, soc.json), then uses the same audited xfer() as the CPU DVFS client (channel gates, empty queues, one
 * published request, bounded reply wait, nothing rolled back).
 */
#include <stdint.h>
#include "console.h"
#include "ramlog.h"
#include "acpm.h"
#include "gpuprobe.h"
#include "gpuclk.h"

#if SOC_GS101 && defined(ACPM_DVFS_CH) && defined(GPU_CLK_KHZ) && defined(GPU_FVMAP_IDX_G3D)

#define IDX_G3DL2	GPU_FVMAP_IDX_G3DL2	/* ACPM_DVFS_G3DL2 = 6 (gs101-gpu.dtsi:46) */
#define IDX_G3D		GPU_FVMAP_IDX_G3D	/* ACPM_DVFS_G3D = 5 (:47) */

__attribute__((noipa)) static unsigned put_txt(char *dst, unsigned cap, unsigned pos, const char *s)
{
	while (*s && pos + 1 < cap)
		dst[pos++] = *s++;
	return pos;
}

__attribute__((noipa)) static unsigned put_dec(char *dst, unsigned cap, unsigned pos, uint32_t v)
{
	char t[10];
	unsigned n = 0;

	t[n++] = (char)('0' + v % 10u);
	v /= 10u;
	for (unsigned i = 1; i < sizeof(t) && v; i++) {
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	}
	while (n && pos + 1 < cap)
		dst[pos++] = t[--n];
	return pos;
}

__attribute__((noipa)) static unsigned put_hex(char *dst, unsigned cap, unsigned pos, uint32_t v)
{
	for (unsigned i = 0; i < 8 && pos + 1 < cap; i++) {
		unsigned d = (v >> (28 - 4 * i)) & 0xfu;

		dst[pos++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
	}
	return pos;
}

static void emit(char *buf, unsigned pos)
{
	if (pos >= RAMLOG_LINE_MAX)
		pos = RAMLOG_LINE_MAX - 1;
	buf[pos] = 0;
	con_str("GPU", buf);
}

/* "CLK <a> <b>=<n> ..." : text pieces and one decimal each, up to three */
static void say3(const char *a, const char *k1, uint32_t v1, const char *k2, uint32_t v2, const char *k3, uint32_t v3)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, "CLK ");

	p = put_txt(line, sizeof(line), p, a);
	if (k1) {
		p = put_txt(line, sizeof(line), p, " ");
		p = put_txt(line, sizeof(line), p, k1);
		p = put_txt(line, sizeof(line), p, "=");
		p = put_dec(line, sizeof(line), p, v1);
	}
	if (k2) {
		p = put_txt(line, sizeof(line), p, " ");
		p = put_txt(line, sizeof(line), p, k2);
		p = put_txt(line, sizeof(line), p, "=");
		p = put_dec(line, sizeof(line), p, v2);
	}
	if (k3) {
		p = put_txt(line, sizeof(line), p, " ");
		p = put_txt(line, sizeof(line), p, k3);
		p = put_txt(line, sizeof(line), p, "=");
		p = put_dec(line, sizeof(line), p, v3);
	}
	emit(line, p);
}

static void say_txt(const char *a, const char *b, const char *c)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, "CLK ");

	p = put_txt(line, sizeof(line), p, a);
	p = put_txt(line, sizeof(line), p, b);
	p = put_txt(line, sizeof(line), p, c);
	emit(line, p);
}

/* one transaction, three lines like the touch probe's DVFS lines: command words, reply words, status */
static void xline(const char *what, const char *dom, const struct acpm_xfer *x)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p, i;

	if (!x->seq && !x->err)
		return;
	for (unsigned k = 0; k < 2; k++) {
		p = put_txt(line, sizeof(line), 0, "CLK ");
		p = put_txt(line, sizeof(line), p, what);
		p = put_txt(line, sizeof(line), p, " ");
		p = put_txt(line, sizeof(line), p, dom);
		p = put_txt(line, sizeof(line), p, k ? " rep=" : " req=");
		for (i = 0; i < 4; i++) {
			p = put_hex(line, sizeof(line), p, k ? x->rep[i] : x->req[i]);
			p = put_txt(line, sizeof(line), p, i < 3 ? " " : "");
		}
		emit(line, p);
	}
	p = put_txt(line, sizeof(line), 0, "CLK ");
	p = put_txt(line, sizeof(line), p, what);
	p = put_txt(line, sizeof(line), p, " ");
	p = put_txt(line, sizeof(line), p, dom);
	p = put_txt(line, sizeof(line), p, " seq=");
	p = put_dec(line, sizeof(line), p, x->seq);
	p = put_txt(line, sizeof(line), p, " err=");
	p = put_dec(line, sizeof(line), p, (uint32_t)x->err);
	p = put_txt(line, sizeof(line), p, " ret=");
	p = put_dec(line, sizeof(line), p, x->ret);
	p = put_txt(line, sizeof(line), p, " val=");
	p = put_dec(line, sizeof(line), p, x->value);
	p = put_txt(line, sizeof(line), p, " writes=");
	p = put_dec(line, sizeof(line), p, x->n);
	p = put_txt(line, sizeof(line), p, " pub=");
	p = put_dec(line, sizeof(line), p, x->published);
	emit(line, p);
	if (x->err)
		say_txt("refused/failed: ", acpm_why(x->why), x->n ? " (writes were made)" : " (nothing written)");
	if (x->err && x->published) {	/* nothing is rolled back: this is the queue as the failure left it */
		say3("STATE", "req_rear", x->state[0], "req_front", x->state[1], "rsp_rear", x->state[2]);
		say_txt("STATE: ", "the request stays queued; channel left as is, rerun only after reading this", "");
	}
}

static const char *dname(uint32_t dom)
{
	return dom == IDX_G3DL2 ? "G3DL2" : "G3D";
}

/* GET one domain; prints the transaction */
static int get_one(const struct acpm_cfg *c, uint32_t dom, uint32_t *khz, struct acpm_trace *t)
{
	int e;

	t->n = 0;
	e = acpm_dvfs_get(c, dom, khz, t);
	xline("get", dname(dom), &t->a);
	if (!e)
		say3(dname(dom) , "kHz", *khz, 0, 0, 0, 0);
	return e;
}

int gpu_clk(void)
{
	struct acpm_cfg c;
	struct acpm_trace t;
	struct acpm_chan h;
	uint32_t cur[2], back[2], dom, khz;
	int e, k, ok = 0, bad = 0;

	acpm_cfg_init(&c);
	say3("cfg", "dvfs_ch", c.dvfs_ch, "layout_verified", c.layout_verified, "elem_unit_verified", c.elem_unit_verified);
	say3("target", "kHz", GPU_CLK_KHZ, "g3dl2_idx", IDX_G3DL2, "g3d_idx", IDX_G3D);
	if (!c.dvfs_ch) {
		say_txt("refused: ", "no ACPM DVFS channel configured", "");
		return 1;
	}
	if (!c.layout_verified) {
		say_txt("refused: ", acpm_why(ACPM_W_LAYOUT), "");
		say_txt("flag: ", "soc.json acpm.layout_verified = 0, nothing written", "");
		return 1;
	}
	if (!c.elem_unit_verified) {
		say_txt("refused: ", acpm_why(ACPM_W_UNIT), "");
		say_txt("flag: ", "soc.json acpm.elem_unit_verified = 0, nothing written", "");
		return 1;
	}
	if (!counter_freq()) {
		say_txt("refused: ", acpm_why(ACPM_W_TIMER), "");
		return 1;
	}
	c.ch_id = c.dvfs_ch;		/* describe the DVFS channel, not the PMIC one (as touchprobe.c does) */
	e = acpm_describe(&c, &h);
	if (e) {
		say_txt("refused: ", acpm_why(h.why), "");
		say_txt("describe failed, ", "nothing written", "");
		return 1;
	}
	say3("desc", "qlen", h.qlen, "elem_bytes", h.esz_b, "ap_poll", h.ap_poll);
	say3("queues", "req_rear", h.rr, "req_front", h.rf, "rsp_rear", h.sr);
	if (h.rr != h.rf || h.sr != h.sf) {
		say_txt("refused: ", acpm_why(ACPM_W_BUSY), "");
		say_txt("queues not empty, ", "nothing written", "");
		return 1;
	}

	acpm_xfer_zero(&t.a);
	acpm_xfer_zero(&t.b);
	t.seq = 1;
	t.n = 0;
	/* 1. GET both, G3DL2 then G3D: a failure here means the domains are not understood: no request */
	for (k = 0; k < 2; k++) {
		dom = k ? IDX_G3D : IDX_G3DL2;
		cur[k] = 0;
		e = get_one(&c, dom, &cur[k], &t);
		if (e) {
			say_txt("result: ", "REFUSED-or-FAILED at the first GET, nothing written", "");
			return 1;
		}
	}
	/* 2. FREQ_REQ the init rate: G3DL2 first, then G3D (kbase gpu_dvfs_set_initial_level order); a failure stops the rest, no retry */
	for (k = 0; k < 2 && !bad; k++) {
		dom = k ? IDX_G3D : IDX_G3DL2;
		t.n = 0;
		e = acpm_dvfs_reassert(&c, dom, GPU_CLK_KHZ, &t);
		xline("pre-read", dname(dom), &t.a);
		if (t.n > 1)
			xline("request", dname(dom), &t.b);
		if (e)
			bad = 1;
	}
	wait_ticks(counter_freq() / 100);	/* ~10 ms for the firmware to act (guess; the read-back shows whether it did) */
	/* 3. read both back (also after a failure: it shows the state the firmware is in) */
	for (k = 0; k < 2; k++) {
		dom = k ? IDX_G3D : IDX_G3DL2;
		back[k] = 0;
		khz = 0;
		t.n = 0;
		if (!get_one(&c, dom, &khz, &t)) {
			back[k] = khz;
			if (khz == GPU_CLK_KHZ)
				ok++;
		}
	}
	say3("readback", "g3dl2", back[0], "g3d", back[1], "asked", GPU_CLK_KHZ);
	say_txt("after: ", "CMU_G3D words follow (compare with the probe dump above)", "");
	gpu_probe_cmu();
	if (!bad && ok == 2) {
		say_txt("result: ", "ok, both clocks read back the requested rate", "");
		return 0;
	}
	say_txt("result: ", bad ? "REFUSED-or-FAILED" : "MISMATCH or unread", " (see the lines above)");
	return 1;
}

#else	/* not gs101 / no DVFS block: refuse */

int gpu_clk(void) { return 1; }

#endif
