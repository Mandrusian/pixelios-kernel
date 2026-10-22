/* Host test for the --touch-probe=dvfsr / dvfs steps of touchprobe.c (touch_probe_dvfs 0/1/2) on top of acpm.c and the fake APM of
 * dvfs_host_test.c: the printed lines, the cluster order (CL0, CL1, CL2), that dvfs sends only the non-zero rates, that a refusal
 * stops the rest, the read-back after the requests, and that no line is longer than the 96-byte RAM log line allows. Run by build.sh:
 *   gcc -O1 -Wall -Wextra -Werror -DDP_WRITE=1 ... dvfs_probe_host_test.c   (the dvfs image: TOUCH_DVFS_WRITE, rates 1803000,0,1826000)
 *   gcc -O1 -Wall -Wextra -Werror -DDP_WRITE=0 ... dvfs_probe_host_test.c   (the dvfsr image: step 2 must refuse, nothing sent) */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef DP_WRITE
#error "define DP_WRITE=0|1"
#endif
#define TOUCH_DVFS
#if DP_WRITE
#define TOUCH_DVFS_WRITE
#define DVFS_KHZ0 1803000
#define DVFS_KHZ1 0
#define DVFS_KHZ2 1826000
#endif
#define main dvfs_host_main
#define w32 acpm_w32
#define w32_allowed acpm_w32_allowed
#define r32 acpm_r32
#include "dvfs_host_test.c"
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
	int a, b, c;

	acpm_cfg_init(&cfg);
	setup(ESZ);
	assert(touch_probe_dvfs(0) == ACPM_OK);			/* describe: reads only, channel 1 */
	assert(find("ACPM cfg ch=1 ", 0) >= 0 && find("ACPM descriptor validated; queues empty", 0) >= 0 && nw == 0);
	assert(find("ACPM q len=5 elem=16 words=4 bytes=16", 0) >= 0);
	assert(touch_probe_dvfs(1) == ACPM_OK);
	a = find("DVFS CL0 rate=1197000 kHz", 0);
	b = find("DVFS CL1 rate=1197000 kHz", 0);
	c = find("DVFS CL2 rate=1106000 kHz", 0);
	assert(a >= 0 && b > a && c > b);
	assert(find("dvfs read: 3/3 domains ok", 0) >= 0 && nreq == 0 && nget == 3 && nbell == 3);
	assert(find("DVFS get CL0 seq=1 req=00010002 00000000 00000001", 0) >= 0);
	assert(find("DVFS get CL2 rep=00030000 0010e050 00000001 00000000", 0) >= 0);	/* 1106000 = 0x10e050; word 0 = seq << 16 as on the phone */
	nout = 0;
	setup(ESZ);
#if DP_WRITE
	assert(touch_probe_dvfs(2) == ACPM_OK);
	for (unsigned k = 0; getenv("DP_DUMP") && k < nout; k++)
		fprintf(stderr, "%s\n", out[k]);
	a = find("DVFS request CL0 seq=2 req=00020002 001b82f8 00000000", 0);	/* 1803000 = 0x1b82f8 */
	c = find("DVFS request CL2 ", 0);
	assert(a >= 0 && c > a && find("DVFS request CL1 ", 0) < 0 && find("DVFS pre-read CL1 ", 0) < 0);	/* CL1 asked 0 = left alone */
	assert(cur[2] == 1803000 && cur[3] == 1197000 && cur[4] == 1826000 && nreq == 2);
	assert(find("DVFS CL0 readback rate=1803000 kHz (asked 1803000)", c) >= 0);
	assert(find("DVFS CL1 readback rate=1197000 kHz (asked 0)", c) >= 0);
	assert(find("DVFS CL2 readback rate=1826000 kHz (asked 1826000)", c) >= 0);
	assert(find("dvfs set: ok 2/2 read back", 0) >= 0 && stray == 0);
	only_channel_writes();
	/* the first request fails (APM never takes it): the second cluster is NOT touched, the read-back still prints */
	nout = 0;
	setup(ESZ);
	never_at = 2;
	assert(touch_probe_dvfs(2) != ACPM_OK);
	assert(find("DVFS request CL2 ", 0) < 0 && find("DVFS readback CL0 err=5", 0) >= 0 && find("dvfs set: UNVERIFIED 0/2 read back (differ 0, unread 2)", 0) >= 0);	/* the read-back is refused: the request is still queued (queue not empty) */
	assert(find("the request stays queued", 0) >= 0 && nreq == 0);
	/* review H1: the firmware answers the requests but keeps the rates: the RESULT must say MISMATCH (it used to say ok) */
	nout = 0;
	setup(ESZ);
	ignore_req = 1;
	assert(touch_probe_dvfs(2) != ACPM_OK);
	assert(find("DVFS CL0 readback rate=1197000 kHz (asked 1803000)", 0) >= 0 && find("dvfs set: MISMATCH 0/2 read back (differ 2, unread 0)", 0) >= 0);
	assert(find("dvfs set: ok", 0) < 0);
	/* a slow reply (6 ms) is waited for: ok */
	nout = 0;
	setup(ESZ);
	delay_ticks = 150000;
	delay_from = 2;
	assert(touch_probe_dvfs(2) == ACPM_OK && find("dvfs set: ok 2/2 read back", 0) >= 0);
#else
	assert(touch_probe_dvfs(2) == ACPM_E_UNSURE && find("dvfs set: not in this image", 0) >= 0 && nw == 0 && nbell == 0);
#endif
	printf("PASS dvfs_probe_host_test DP_WRITE=%d\n", DP_WRITE);
	return 0;
}
