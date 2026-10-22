/* Host test for the --touch-probe=railsr / rails steps of touchprobe.c (touch_probe_rails 0/1/2) on top of acpm.c and the fake APM of
 * acpm_host_test.c: the printed lines, the step order (LDO25M before LDO26M), that nothing is written while a [guess] is not verified
 * (the shipped soc.json), and that no line is longer than the 96-byte RAM log line allows. Two builds, run by build.sh:
 *   gcc ... -DRT_VERIFIED=1 rails_host_test.c   (assumptions overridden to "verified": the full read + update flow)
 *   gcc ... -DRT_VERIFIED=0 rails_host_test.c   (flags forced to 0: unsure, zero writes) */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef RT_VERIFIED
#error "define RT_VERIFIED=0|1"
#endif
#define TOUCH_RAILS
#define TOUCH_RAILS_WRITE
#if RT_VERIFIED
#undef ACPM_LAYOUT_VERIFIED
#define ACPM_LAYOUT_VERIFIED 1
#undef ACPM_ELEM_UNIT_VERIFIED
#define ACPM_ELEM_UNIT_VERIFIED 1
#undef ACPM_RAIL0_VSEL_KNOWN
#define ACPM_RAIL0_VSEL_KNOWN 1
#undef ACPM_RAIL0_VSEL_MIN
#define ACPM_RAIL0_VSEL_MIN 0x10
#undef ACPM_RAIL0_VSEL_MAX
#define ACPM_RAIL0_VSEL_MAX 0x30
#undef ACPM_RAIL1_VSEL_KNOWN
#define ACPM_RAIL1_VSEL_KNOWN 1
#undef ACPM_RAIL1_VSEL_MIN
#define ACPM_RAIL1_VSEL_MIN 0x20
#undef ACPM_RAIL1_VSEL_MAX
#define ACPM_RAIL1_VSEL_MAX 0x3f
#else	/* the shipped flags are 1 since run acpm3: force them off to keep testing the refusal path */
#undef ACPM_LAYOUT_VERIFIED
#define ACPM_LAYOUT_VERIFIED 0
#undef ACPM_ELEM_UNIT_VERIFIED
#define ACPM_ELEM_UNIT_VERIFIED 0
#endif
/* both .c files are included into this one unit: give acpm.c's (and its test's) statics that clash with touchprobe.c's another name */
#define main acpm_host_main
#define w32 acpm_w32
#define w32_allowed acpm_w32_allowed
#define r32 acpm_r32
#include "acpm_host_test.c"
#undef main
#undef w32
#undef w32_allowed
#undef r32
#include "touchprobe.c"

static char out[1024][128];
static unsigned nout;

void wait_ticks(uint64_t t) { now += t; }
void con_str(const char *label, const char *s)
{
	assert(nout < 1024 && strlen(s) <= 82 && strlen(label) + strlen(s) < 96);
	snprintf(out[nout++], sizeof(out[0]), "%s %s", label, s);
}

static int find(const char *needle, unsigned from)
{
	for (unsigned i = from; i < nout; i++)
		if (strstr(out[i], needle))
			return (int)i;
	return -1;
}

int main(void)
{
	int e, i, j;
	char line[128];

	/* describe prints the raw descriptor, the pointers and the buffer words WHATEVER the validation said, then the exact reason */
	setup(16);
	assert(touch_probe_rails(0) == ACPM_OK);
	assert(find("ACPM desc[12..17]=", 0) >= 0 && find("ACPM req buf+000", 0) >= 0 && find("ACPM rsp buf+040", 0) >= 0);
	snprintf(line, sizeof(line), "ACPM req buf+010 %08x %08x %08x %08x", GARB(REQ_BASE_O / 4 + 4), GARB(REQ_BASE_O / 4 + 5),
		 GARB(REQ_BASE_O / 4 + 6), GARB(REQ_BASE_O / 4 + 7));
	assert(find(line, 0) >= 0 && find("ACPM req buf+050", 0) < 0 && nw == 0);	/* q_len 5 * 16 bytes = 5 lines, one slot each */
	nout = 0;
	setup(64);					/* wrong element size: refused, but the words are all there */
	assert(touch_probe_rails(0) == ACPM_E_DESC);
	assert(find("ACPM desc[0..5]=", 0) >= 0 && find("ACPM desc[12..17]=", 0) >= 0 && find("ACPM q len=5 elem=64", 0) >= 0);
	assert(find("ACPM req buf+000", 0) >= 0 && find("ACPM describe refused: q_elem_size * elem_unit is not 16 bytes", 0) >= 0);
	assert(find("ACPM descriptor validated", 0) < 0 && nw == 0);
	nout = 0;
	setup(16);					/* no such channel: the ids that are in the table */
	setS(desc_off(), 9);
	assert(touch_probe_rails(0) == ACPM_E_DESC);
	assert(find("ACPM ids[0..]=0 1 9 deadbeef deadbeef deadbeef", 0) >= 0 && find("ACPM describe refused: no descriptor", 0) >= 0 && nw == 0);
	nout = 0;
	setup(16);					/* descriptor type wrong: refused with the words */
	setS(desc_off() + 0x0c, 2);
	assert(touch_probe_rails(0) == ACPM_E_DESC);
	assert(find("ACPM desc[0..5]=", 0) >= 0 && find("ACPM describe refused: descriptor type", 0) >= 0);
	nout = 0;

	setup(16);
#if RT_VERIFIED
	assert(touch_probe_rails(0) == ACPM_OK);
	assert(find("ACPM cfg mbox=0x17610000 sram=0x2039000 init=+0xa000", 0) >= 0);
	assert(find("ACPM cfg layout_verified=1 elem_unit=1 elem_unit_verified=1", 0) >= 0);
	assert(find("ACPM descriptor validated; queues empty", 0) >= 0 && find("ACPM unsure", 0) < 0 && nw == 0);
	assert(touch_probe_rails(1) == ACPM_OK);
	assert(find("ACPM read L25M seq=1 req=0001015c 00000000 00000000 00000000", 0) >= 0);
	assert(find("ACPM read L25M rep=0001015c 00002300", 0) >= 0 && find("ACPM read L25M ret=0 val=0x23 err=0", 0) >= 0);
	assert(find("ACPM read L26M seq=2 req=0002015d 00000000 00000000 00000000", 0) >= 0);
	assert(find("ACPM read L26M rep=0002015d 00003100", 0) >= 0 && find("ACPM read L26M writes=8 published=2 ticks=", 0) >= 0);
	assert(find("SWEET TOUCH RESULT: rails read: L25M=off L26M=off ok", 0) >= 0);
	assert(pmic[0x5C] == 0x23 && pmic[0x5D] == 0x31 && nbell == 2);
	nout = 0;
	assert(touch_probe_rails(2) == ACPM_OK);
	i = find("ACPM update L25M seq=2 req=0002015c 00808002 00000000 00000000", 0);
	j = find("ACPM update L26M seq=4 req=0004015d 00808002 00000000 00000000", 0);
	assert(find("ACPM pre-read L25M seq=1 req=0001015c 00000000", 0) >= 0 && find("ACPM pre-read L26M seq=3 req=0003015d", 0) >= 0);
	assert(i >= 0 && j > i);			/* DVDD before AVDD */
	assert(find("ACPM readback L25M", 0) > j && find("ACPM readback L26M", 0) > j);
	assert(find("SWEET TOUCH RESULT: rails update: L25M 0x23->0xa3 L26M 0x31->0xb1 on=2/2 ok", 0) >= 0);
	assert(pmic[0x5C] == 0xA3 && pmic[0x5D] == 0xB1 && stray == 0);
	/* a failing update stops before the second rail and still reads back */
	setup(16);
	nout = 0;
	mode = M_RET;
	e = touch_probe_rails(2);
	assert(e != ACPM_OK && find("ACPM update L26M", 0) < 0 && find("REFUSED-or-FAILED", 0) >= 0 && pmic[0x5D] == 0x31);
	/* review 2 M1: L25M already on (ABL left it) is not a failure and L26M still gets its update */
	setup(16);
	nout = 0;
	pmic[0x5C] = 0xA8;
	assert(touch_probe_rails(2) == ACPM_OK);
	assert(pmic[0x5C] == 0xA8 && pmic[0x5D] == 0xB1 && find("ACPM update L26M", 0) >= 0);
	/* review 2 M2: a refused update prints zeros, never stack garbage and never "stays queued" */
	assert(find("ACPM update L25M seq=0 req=00000000 00000000 00000000 00000000", 0) >= 0 && find("L25M writes=0 published=0", 0) >= 0 && find("stays queued", 0) < 0);
#else
	assert(touch_probe_rails(0) == ACPM_OK);		/* describing is always allowed: reads only */
	assert(find("ACPM unsure: descriptor layout", 0) >= 0 && find("ACPM unsure: queue element unit", 0) >= 0 && nw == 0);
	e = touch_probe_rails(1);
	assert(e == ACPM_E_UNSURE && nw == 0 && nbell == 0 && stray == 0);
	assert(find("ACPM unsure: descriptor layout", 0) >= 0 && find("rails read: L25M=? L26M=? REFUSED-or-FAILED", 0) >= 0);
	e = touch_probe_rails(2);			/* never reached by shim.c after a refusal; must refuse anyway */
	assert(e == ACPM_E_UNSURE && nw == 0 && nbell == 0 && stray == 0);
	(void)i; (void)j;
#endif
	printf("PASS rails host test (verified=%d)\n", RT_VERIFIED);
	return 0;
}
