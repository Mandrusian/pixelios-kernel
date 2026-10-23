/* Host test for gpuclk.c (--gpu-clk) against the FAKE APM of dvfs_host_test.c (#included, its main renamed): G3DL2 / G3D GET, the two
 * FREQ_REQ for the init rate in kbase's order (G3DL2 first), the read-back, the second CMU dump, and every refusal with ZERO writes
 * (ACPM gates, non-empty queue, failing first GET); a timeout of the first request stops the second; acpm_dvfs_reassert refuses CPU
 * domains and every rate but the ceiling. Built twice by build.sh: with the real board macros, and (-DGATE_TEST) with
 * ACPM_LAYOUT_VERIFIED / ACPM_ELEM_UNIT_VERIFIED forced to 0 by a sed'ed copy of the header. Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -Wno-unused-function -DGPU_CLK -I. -Iout -include out/board_config.h -o out/gpuclk_host_test gpuclk_host_test.c */
#define main dvfs_host_main
#include "dvfs_host_test.c"
#undef main
#include "gpuclk.c"

#ifndef GPU_CLK
#error "build with -DGPU_CLK"
#endif

static char lines[512][RAMLOG_LINE_MAX + 8];
static unsigned nl, cmu_calls;

void con_str(const char *label, const char *s)
{
	assert(nl < 512);
	snprintf(lines[nl++], sizeof(lines[0]), "%s %s", label, s);
}
void wait_ticks(uint64_t t) { now += t; }
void gpu_probe_cmu(void) { cmu_calls++; con_str("GPU", "CMU_FAKE_DUMP"); }

static int has(const char *s)
{
	for (unsigned i = 0; i < nl; i++)
		if (strstr(lines[i], s))
			return 1;
	return 0;
}

static void dump_lines(void)
{
	for (unsigned i = 0; i < nl; i++)
		fprintf(stderr, "  %s\n", lines[i]);
}

#define EXPECT(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); dump_lines(); assert(0); } } while (0)

/* the (domain, word1, word2) of every request slot written, in order */
static unsigned reqs(uint32_t dom[], uint32_t w1[], uint32_t w2[])
{
	unsigned n = 0, i;

	for (i = 0; i + 3 < nw; i++)
		if (wlog[i].pa >= SRAM + REQ_BASE_O && wlog[i].pa < SRAM + REQ_BASE_O + QLEN * ESZ && (wlog[i].pa - SRAM - REQ_BASE_O) % ESZ == 0) {
			dom[n] = wlog[i].v & 0xffffu;
			w1[n] = wlog[i + 1].v;
			w2[n] = wlog[i + 2].v;
			n++;
		}
	return n;
}

int main(void)
{
#ifndef GATE_TEST
	uint32_t dom[16], w1[16], w2[16];
	unsigned n;
#endif

#ifdef GATE_TEST
	/* the board macros say layout_verified = 0 / elem_unit_verified = 0 (sed'ed header): refuse, say why, write nothing */
	setup(ESZ);
	nl = cmu_calls = 0;
	EXPECT(gpu_clk() == 1 && nw == 0 && nbell == 0 && cmu_calls == 0);
#ifdef GATE_UNIT
	EXPECT(has("refused: ") && has("element unit") && has("acpm.elem_unit_verified = 0") && has("elem_unit_verified=0"));
#else
	EXPECT(has("refused: ") && has("layout") && has("acpm.layout_verified = 0") && has("layout_verified=0"));
#endif
	puts("PASS gpuclk gate host test");
	return 0;
#else
	/* happy path */
	setup(ESZ);
	nl = cmu_calls = 0;
	EXPECT(gpu_clk() == 0);
	n = reqs(dom, w1, w2);
	EXPECT(n == 8);		/* GET6 GET5 | pre-GET6 REQ6 | pre-GET5 REQ5 | GET6 GET5 */
	EXPECT(dom[0] == 6 && w2[0] == 1 && dom[1] == 5 && w2[1] == 1);
	EXPECT(dom[2] == 6 && w2[2] == 1 && dom[3] == 6 && w2[3] == 0 && w1[3] == 151000);	/* FREQ_REQ = 0 */
	EXPECT(dom[4] == 5 && w2[4] == 1 && dom[5] == 5 && w2[5] == 0 && w1[5] == 151000);
	EXPECT(dom[6] == 6 && w2[6] == 1 && dom[7] == 5 && w2[7] == 1);
	EXPECT(nreq == 2 && nget == 6 && cmu_calls == 1);
	EXPECT(has("GPU CLK G3DL2 kHz=151000") && has("GPU CLK G3D kHz=151000") && has("GPU CLK readback g3dl2=151000 g3d=151000 asked=151000"));
	EXPECT(has("GPU CLK result: ok") && has("GPU CLK cfg dvfs_ch=1 layout_verified=1 elem_unit_verified=1"));
	EXPECT(has("GPU CLK request G3DL2 req=") && has("GPU CLK request G3D seq="));
	only_channel_writes();
	{ unsigned a = 0, b = 0, i; for (i = 0; i < nl; i++) { if (strstr(lines[i], "request G3DL2 seq=")) a = i; if (strstr(lines[i], "request G3D seq=")) b = i; } EXPECT(a && b && a < b); }

	/* the firmware ignores the request and sits at another rate: the read-back says MISMATCH, both requests were still made */
	setup(ESZ); cur[5] = cur[6] = 202000; ignore_req = 1;
	nl = cmu_calls = 0;
	EXPECT(gpu_clk() == 1 && nreq == 2 && has("GPU CLK readback g3dl2=202000 g3d=202000") && has("MISMATCH") && cmu_calls == 1);

	/* a failing first GET (error code in reply word 3): nothing is requested */
	setup(ESZ); get_err = 1;
	nl = cmu_calls = 0;
	EXPECT(gpu_clk() == 1 && nreq == 0 && has("REFUSED-or-FAILED at the first GET") && cmu_calls == 0);
	setup(ESZ); never = 1;
	nl = cmu_calls = 0;
	EXPECT(gpu_clk() == 1 && nreq == 0 && has("REFUSED-or-FAILED at the first GET"));

	/* the first request is never taken: published, nothing rolled back, the second domain is not asked, no further doorbell */
	setup(ESZ); never_at = 4;		/* doorbells: GET6 1, GET5 2, pre-GET6 3, REQ6 4 */
	nl = cmu_calls = 0;
	EXPECT(gpu_clk() == 1 && nbell == 4 && nreq == 0 && has("REFUSED-or-FAILED") && has("the request stays queued"));
	n = reqs(dom, w1, w2);
	EXPECT(!(n >= 6 && dom[5] == 5 && w2[5] == 0));

	/* queue not empty / wrong element size / a layout the descriptor check rejects: zero writes */
	setup(ESZ); setS(REQ_FRONT_O, 0);
	nl = cmu_calls = 0;
	EXPECT(gpu_clk() == 1 && nw == 0 && nbell == 0 && has("refused: "));
	setup(8);
	nl = cmu_calls = 0;
	EXPECT(gpu_clk() == 1 && nw == 0 && nbell == 0);

	/* acpm_dvfs_reassert: only G3D / G3DL2, only the ceiling row; the CPU domains and every other rate are refused before anything is sent */
	{
		struct acpm_trace t;

		setup(ESZ);
		t = trace();
		EXPECT(acpm_dvfs_reassert(&cfg, 2, 1197000, &t) == ACPM_E_ARG && nw == 0);
		EXPECT(acpm_dvfs_reassert(&cfg, 3, 1197000, &t) == ACPM_E_ARG && nw == 0);
		EXPECT(acpm_dvfs_reassert(&cfg, 4, 1106000, &t) == ACPM_E_ARG && nw == 0);
		EXPECT(acpm_dvfs_reassert(&cfg, 7, 151000, &t) == ACPM_E_ARG && nw == 0);
		EXPECT(acpm_dvfs_reassert(&cfg, 5, 202000, &t) == ACPM_E_ARG && nw == 0);	/* a firmware row, above the ceiling */
		EXPECT(acpm_dvfs_reassert(&cfg, 6, 996000, &t) == ACPM_E_ARG && nw == 0);
		EXPECT(acpm_dvfs_reassert(&cfg, 5, 151001, &t) == ACPM_E_ARG && nw == 0);	/* not a row */
		EXPECT(acpm_dvfs_reassert(&cfg, 5, 0, &t) == ACPM_E_ARG && nw == 0);
		EXPECT(acpm_dvfs_reassert(&cfg, 5, 151000, &t) == ACPM_OK && nreq == 1);	/* same rate as the current one: still sent */
		t = trace();
		EXPECT(acpm_dvfs_reassert(&cfg, 6, 151000, &t) == ACPM_OK && nreq == 2);
		only_channel_writes();
	}
	/* a CPU --dvfs table is untouched by the GPU rows: the CPU client still sees exactly three domains */
	EXPECT(acpm_dvfs_ndom() == 3 && ACPM_DVFS_NALL == 5);
	puts("PASS gpuclk host test");
	return 0;
#endif
}
