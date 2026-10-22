/* Host unit test for the sweettouchpoke script parser (xnu patch 0093, bsd/dev/sweet_touch_poke.h, extracted by
 * shim/build.sh so the kernel and this test see the same bytes):
 *   gcc -O1 -Wall -Wextra -Werror -Iout -o out/touchpoke_host_test touchpoke_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sweet_touch_poke.h"

/* Real layout via the shared poke_build: SPI11 0x10D40000 size 0x1000 (soc/google/gs101/soc.json usi11_spi_size),
 * sysreg mode register 0x10C2100C, CMU_PERIC1 0x10C00000 +0x10000 (offsets as in soc.json cmu_peric1), CMU_TOP
 * 0x1E080000 +0x10000, pin banks 0x10C40060 / 0x10C400C0 / 0x174D00C0 (example values; only the banks matter). */
static struct poke_range RG[POKE_MAX_RANGES];
static uint64_t WIN[POKE_MAX_WINDOWS];
static unsigned NRG, NWIN;

static int
P(const char *s, struct poke_op *o)
{
	unsigned bad;
	return poke_parse(s, o, RG, NRG, WIN, NWIN, &bad);
}

int
main(void)
{
	struct poke_hw hw = {
		.spi_pa = 0x10d40000, .spi_size = 0x1000, .usi_mode_pa = 0x10c2100c,
		.cmu_ok = 1, .top_ok = 1, .cmu_base = 0x10c00000, .cmu_size = 0x10000, .top_base = 0x1e080000, .top_size = 0x10000,
		.cmu_mux_usi = 0x1000, .cmu_mux_bus = 0x1004, .cmu_mux_bus_auto = 0x1008, .cmu_mux_usi_auto = 0x100c,
		.cmu_gate_ipclk = 0x2000, .cmu_gate_pclk = 0x2004, .cmu_gate_rstn = 0x2008, .cmu_qch = 0x3000, .cmu_div = 0x180c,
		.top_mux = 0x1020, .top_div = 0x1840, .top_gate = 0x2060,
		.pin_bank = { 0x10c40060, 0x10c40060, 0x10c40060, 0x10c40060, 0x10c400c0, 0x10c400c0, 0x174d00c0 },
	};
	struct poke_op o[POKE_MAX_OPS + 2];
	unsigned bad = 99;
	char big[400], many[400];
	int i;

	NRG = poke_build(&hw, RG, WIN, &NWIN);
	assert(NRG == 11 && NWIN == 12);

	/* valid; every op ends with ';' */
	assert(P("", o) == 0);
	assert(P("r:10d40000;", o) == 1 && o[0].kind == 'r' && o[0].addr == 0x10d40000);
	assert(P("r:0x10D40004;", o) == 1 && o[0].addr == 0x10d40004);          /* 0x prefix, case */
	assert(P("r:0X10d40004;", o) == 1 && o[0].addr == 0x10d40004);          /* 0X prefix */
	assert(P("w:10c21000=ABCD1234;", o) == 1 && o[0].kind == 'w' && o[0].val == 0xabcd1234u);
	assert(P("m:10d40008=ff00=1200;", o) == 1 && o[0].kind == 'm' && o[0].mask == 0xff00 && o[0].val == 0x1200);
	assert(P("d:f4240;", o) == 1 && o[0].kind == 'd' && o[0].val == 1000000u);
	assert(P("r:10d40000;w:10d40004=1;m:10d40008=f=3;d:64;", o) == 4 && o[3].val == 100);
	assert(P("r:10d40ffc;", o) == 1);                                       /* last word of the real 0x1000 SPI11 range */
	assert(P("w:10c40060=1;w:174d00dc=1;", o) == 2);                        /* pin banks are writable */

	/* final ';' is mandatory (a script cut by the boot-args limit must not run) */
	assert(P("r:10d40000", o) == -2);
	assert(P("r:10d40000;r:10d400", o) == -2);
	assert(P("w:10d40000=1", o) == -2);
	assert(P("m:10d40000=1=", o) == -2);

	/* syntax */
	assert(P(";", o) == -2);
	assert(P("r:10d40000;;r:10d40000;", o) == -2);                          /* empty op */
	assert(P("x:10d40000;", o) == -2);
	assert(P("r10d40000;", o) == -2);
	assert(P("r:;", o) == -2);
	assert(P("r:zz;", o) == -2);
	assert(P("r:10d40000=1;", o) == -2);                                    /* too many fields */
	assert(P("w:10d40000;", o) == -2);                                      /* too few */
	assert(P("m:10d40000=1;", o) == -2);
	assert(P("m:10d40000=1=2=3;", o) == -2);
	assert(P("w:10d40000=100000000;", o) == -2);                            /* value above 32 bits */
	assert(P("m:10d40000=100000000=1;", o) == -2);                          /* mask overflow */
	assert(P("m:10d40000=1=100000000;", o) == -2);                          /* value overflow */
	assert(P("r:10d40000 ;", o) == -2);                                     /* stray space */
	assert(P("r:00000000000010d40000;", o) == -2);                          /* 18 digits */
	assert(P("r:00010d4000000000000;", o) == -2);                           /* 17 digits */
	assert(P("r:0x;", o) == -2);
	assert(poke_parse("r:10d40000;r:xyz;", o, RG, NRG, WIN, NWIN, &bad) == -2 && bad == 1);

	/* alignment (-4), range (-5), delay (-6) */
	assert(P("r:10d40002;", o) == -4);
	assert(P("r:10d40001;", o) == -4);
	assert(P("r:10d40ffe;", o) == -4);
	assert(P("r:ffffffffffffffff;", o) == -4);
	assert(P("r:10d3fffc;", o) == -5);                                      /* just below SPI11 */
	assert(P("r:10d41000;", o) == -5);                                      /* just above the real SPI11 size */
	assert(P("r:0;", o) == -5);
	assert(P("r:10c22000;", o) == -5);                                      /* next page after the sysreg page */
	assert(P("r:fffffffffffffffc;", o) == -5);
	assert(P("d:f4241;", o) == -6);
	assert(poke_parse("r:10d40000;d:f4241;", o, RG, NRG, WIN, NWIN, &bad) == -6 && bad == 1);

	/* CMU_PERIC1 / CMU_TOP: reads anywhere, writes only at the ADT windows */
	assert(P("r:10c00000;r:10c0fffc;r:1e080000;r:1e08fffc;", o) == 4);
	assert(P("w:10c00000=1;", o) == -7);                                    /* OPTION-like offset 0 */
	assert(P("m:10c00800=1=1;", o) == -7);
	assert(P("w:10c01000=1;w:10c01004=1;w:10c01008=1;w:10c0100c=1;", o) == 4);   /* the four muxes */
	assert(P("m:10c0180c=ff=9;", o) == 1);                                  /* DIV: writable (docs/research/21) */
	assert(P("w:10c02000=1;w:10c02004=1;w:10c02008=1;w:10c03000=1;", o) == 4);   /* gates, QCH */
	assert(P("w:10c01010=1;", o) == -7);                                    /* neighbour of a window */
	assert(P("w:10c01001=1;", o) == -4);
	assert(P("w:1e081020=1;w:1e081840=1;w:1e082060=1;", o) == 3);           /* TOP mux, div, gate */
	assert(P("w:1e081024=1;", o) == -7);
	assert(P("m:1e080000=1=1;", o) == -7);
	assert(P("w:10c01000=1;w:10c01010=1;", o) == -7);                       /* second op is the bad one */
	assert(poke_parse("w:10c01000=1;w:10c01010=1;", o, RG, NRG, WIN, NWIN, &bad) == -7 && bad == 1);

	/* absent CMU/TOP blocks: no ranges, no windows */
	{
		struct poke_hw h2 = hw;
		struct poke_range r2[POKE_MAX_RANGES];
		uint64_t w2[POKE_MAX_WINDOWS];
		unsigned nw2, bad2, nr2;
		h2.cmu_ok = h2.top_ok = 0;
		nr2 = poke_build(&h2, r2, w2, &nw2);
		assert(nw2 == 0);
		assert(poke_parse("r:10c00000;", o, r2, nr2, w2, nw2, &bad2) == -5);
		assert(poke_parse("r:10d40000;", o, r2, nr2, w2, nw2, &bad2) == 1);
	}

	/* limits: 32 ops ok, 33 rejected, 241 chars rejected */
	many[0] = 0;
	for (i = 0; i < 32; i++) {
		strcat(many, "d:1;");
	}
	assert(P(many, o) == 32);
	strcat(many, "d:1;");
	assert(poke_parse(many, o, RG, NRG, WIN, NWIN, &bad) == -3 && bad == 32);
	memset(big, 'r', sizeof(big));
	big[POKE_MAX_LEN] = 0;
	assert(P(big, o) == -2);                                                /* 240 chars: allowed length, bad syntax */
	big[POKE_MAX_LEN] = 'r';
	big[POKE_MAX_LEN + 1] = 0;
	assert(P(big, o) == -1);                                                /* 241 chars */

	puts("touchpoke_host_test: PASS");
	return 0;
}
