/* Host test for touchprobe.c: sequence order, timeouts, register masks, write allowlist, RMW bit preservation, the
 * docs/research/21 experiments. It #includes the .c so the statics are reachable and uses the REAL generated board
 * macros. Neighbouring bits of every register start as garbage. Build (also run by build.sh):
 *   gcc -O1 -Wall -Wextra -Werror -I. -Iout -include out/board_config.h -o out/touch_host_test touch_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "touchprobe.c"

#if !(SOC_GS101 && defined(TOUCH_SPI_BASE))
#error "needs the bluejay board_config.h (shim/build.sh --board bluejay writes out/board_config.h)"
#endif

/* ---- fake hardware ---- */
#define NREG 512
static struct { uintptr_t pa; uint32_t v; } reg[NREG];
static unsigned nreg;
static uintptr_t bad_pa;		/* reads of this address fault */
static uintptr_t bad_wpa;		/* writes to this address fault (nothing stored) */
static uintptr_t ignore_wpa;		/* writes to this address are accepted but not stored */
static int hw_ok;			/* master: the SPI engine can move data at all */
static int need_cs, need_osc, need_c;	/* ... but only with: CS_REG bit0 clear / USI mux = osc and div 5 / USI_OPTION CLKREQ */
static int loop_ok;			/* self-loopback works */
static uint8_t rx_pat[16];
static unsigned rx_idx;
static uint8_t tx_log[64];
static unsigned ntx;
static uint64_t fake_now;
static int time_frozen;
static unsigned long accesses, spi_reads;
static uintptr_t wlog[65536];
static uint32_t wval[65536];
static unsigned nw;
static char lines[1024][128];
static unsigned nlines;

static uint32_t garbage(uintptr_t pa)
{
	uint64_t x = pa * 0x9e3779b97f4a7c15ull + 12345;

	x ^= x >> 29;
	x *= 0xbf58476d1ce4e5b9ull;
	x ^= x >> 32;
	return (uint32_t)x;
}

static int in_spi(uintptr_t pa)
{
	return pa >= SPI && pa < SPI + 0x1000;
}

static uint32_t *slot(uintptr_t pa)
{
	for (unsigned i = 0; i < nreg; i++)
		if (reg[i].pa == pa)
			return &reg[i].v;
	assert(nreg < NREG);
	reg[nreg].pa = pa;
	reg[nreg].v = in_spi(pa) ? 0 : garbage(pa);
	return &reg[nreg++].v;
}

static int works(void)
{
	if (!hw_ok)
		return 0;
	if (need_cs && (*slot(SPI + TP_SPI_CS_REG) & 1))
		return 0;
	if (need_osc && ((*slot(CMU + TOUCH_CMU_MUX_USI) & (1u << TOUCH_CMU_BIT_MUX_SEL)) ||
			 (*slot(CMU + TOUCH_CMU_DIV) & 0xf) != 5))
		return 0;
	if (need_c && !(*slot(USI + TP_USI_OPTION) & TP_USI_OPT_CLKREQ))
		return 0;
	return 1;
}

static int loopmode(void)
{
	return !!(*slot(SPI + TP_SPI_MODE_CFG) & TP_MODE_LOOPBACK);
}

int try_read32(uintptr_t pa, uint32_t *v)
{
	assert(++accesses < 80000000ul);	/* a runaway loop fails the test instead of hanging it */
	if (pa == bad_pa)
		return 0;
	if (in_spi(pa))
		spi_reads++;
	if (pa == SPI + TP_SPI_STATUS) {
		*v = (works() || (loopmode() && loop_ok)) ? (0x1ffu << 15) : 0;
		return 1;
	}
	if (pa == SPI + TP_SPI_RX_DATA) {
		unsigned i = rx_idx++;

		*v = loopmode() ? (i < ntx ? tx_log[i] : 0) : rx_pat[i % sizeof(rx_pat)];
		return 1;
	}
	*v = *slot(pa);
	return 1;
}

int try_write32(uintptr_t pa, uint32_t v)
{
	assert(nw < 65536);
	if (pa == bad_wpa)
		return 0;
	wlog[nw] = pa;
	wval[nw++] = v;
	if (pa == SPI + TP_SPI_PACKET_CNT)
		rx_idx = ntx = 0;
	if (pa == SPI + TP_SPI_TX_DATA && ntx < sizeof(tx_log))
		tx_log[ntx++] = (uint8_t)v;
	if (pa != ignore_wpa)
		*slot(pa) = v;
	return 1;
}

uint64_t counter_freq(void) { return 1000000; }	/* 1 tick = 1 us */
uint64_t touch_ticks(void) { fake_now += time_frozen ? 0 : 7; return fake_now; }
void wait_ticks(uint64_t t) { fake_now += time_frozen ? 0 : t; }
void con_str(const char *label, const char *s)
{
	assert(strlen(label) + 1 + strlen(s) < RAMLOG_LINE_MAX);
	assert(nlines < 1024);
	snprintf(lines[nlines++], sizeof(lines[0]), "%s %s", label, s);
}

#define MUXB	TOUCH_PIN_MUX_BANK
#define MUXN	TOUCH_PIN_MUX_NR
#define BUSY	(1u << TOUCH_CMU_BIT_MUX_BUSY)
#define SEL	(1u << TOUCH_CMU_BIT_MUX_SEL)

/* ABL handoff state as observed on hardware: mux pad is an INPUT with dat 0 (func 0, pud 1), USI mode 0, CS_REG 1,
 * USI user mux = TOP (sel set in the real log; here clear so the sequence has something to write), divider 0. */
static void reset(void)
{
	nreg = nw = nlines = 0;
	bad_pa = bad_wpa = ignore_wpa = 0;
	hw_ok = need_cs = need_osc = need_c = loop_ok = 0;
	memset(rx_pat, 0, sizeof(rx_pat));
	rx_pat[1] = TOUCH_CHIP_ID0;
	rx_pat[2] = TOUCH_CHIP_ID1;
	rx_idx = ntx = 0;
	time_frozen = 0;
	accesses = spi_reads = 0;
	fake_now = 1000;
	*slot(MUXB) &= ~(0xfu << (MUXN * 4));
	*slot(MUXB + 4) &= ~(1u << MUXN);
	*slot(MUXB + 8) = (*slot(MUXB + 8) & ~(0xfu << (MUXN * 4))) | (1u << (MUXN * 4));
	*slot(TOUCH_USI_MODE_REG) &= ~(uint32_t)TOUCH_USI_MODE_MASK;
	*slot(CMU + TOUCH_CMU_MUX_USI) &= ~(BUSY | SEL);
	*slot(CMU + TOUCH_CMU_DIV) &= ~BUSY;
	*slot(CMU + TOUCH_CMU_GATE_IPCLK) |= 1u << TOUCH_CMU_BIT_GATE_AUTO;
	*slot(CMU + TOUCH_CMU_GATE_PCLK) |= 1u << TOUCH_CMU_BIT_GATE_AUTO;
	*slot(CMU + TOUCH_CMU_GATE_RSTN) |= 1u << TOUCH_CMU_BIT_GATE_AUTO;
	*slot(TOP + TOUCH_CMUTOP_GATE) &= ~(1u << TOUCH_CMUTOP_GATE_BIT);
	*slot(SPI + TP_SPI_CS_REG) = 1;
	*slot(USI + TP_USI_CON) = 1;
}

static int has(const char *needle)
{
	if (getenv("TOUCH_V") && !strcmp(getenv("TOUCH_V"), needle))
		for (unsigned i = 0; i < nlines; i++)
			fprintf(stderr, "%s\n", lines[i]);
	for (unsigned i = 0; i < nlines; i++)
		if (strstr(lines[i], needle))
			return 1;
	return 0;
}

static int line_of(const char *needle)
{
	for (unsigned i = 0; i < nlines; i++)
		if (strstr(lines[i], needle))
			return (int)i;
	return -1;
}

static int write_idx(uintptr_t pa)
{
	for (unsigned i = 0; i < nw; i++)
		if (wlog[i] == pa)
			return (int)i;
	return -1;
}

static int write_idx_val(uintptr_t pa, uint32_t v, unsigned from)
{
	for (unsigned i = from; i < nw; i++)
		if (wlog[i] == pa && wval[i] == v)
			return (int)i;
	return -1;
}

/* Independent of wmask: which registers a run with these options may write at all. */
static int expected_write(uintptr_t pa, unsigned o)
{
	unsigned off;

	if (pa >= SPI && pa < SPI + 0x30) {
		off = (unsigned)(pa - SPI);
		return off != 0x14 && off != 0x1c;
	}
	if (pa == USI + 4 || pa == USI + 8 || pa == TOUCH_USI_MODE_REG)
		return 1;
	if (pa == CMU + TOUCH_CMU_MUX_USI || pa == CMU + TOUCH_CMU_GATE_IPCLK || pa == CMU + TOUCH_CMU_GATE_PCLK ||
	    pa == CMU + TOUCH_CMU_GATE_RSTN || pa == TOP + TOUCH_CMUTOP_GATE)
		return 1;
	if (pa == CMU + TOUCH_CMU_QCH)
		return !(o & TP_O_EXP);
	if (pa == CMU + TOUCH_CMU_DIV)
		return !!(o & (TP_O_OSC | TP_O_TOP));
	if (pa == TOUCH_PIN_CS_BANK || pa == TOUCH_PIN_CS_BANK + 4)	/* the gpp23 bank: clk, mosi, miso, cs */
		return 1;
	if (pa == TOUCH_PIN_CS_BANK + 8)
		return !!(o & (TP_O_RST | TP_O_MISOPU));
	if (pa == MUXB || pa == MUXB + 4)	/* the gpp26 bank: mux, reset */
		return !!(o & (TP_O_MUX | TP_O_RST));
	if (pa == MUXB + 8)
		return !!(o & TP_O_RST);
	return 0;
}

static void check_run_writes(unsigned o)
{
	for (unsigned i = 0; i < nw; i++)
		assert(expected_write(wlog[i], o));
}

static void preserved(uintptr_t pa, uint32_t init, uint32_t allowed)
{
	assert(((*slot(pa) ^ init) & ~allowed) == 0);
}

/* direct w32 call: toggle `xorbits` of the current value; returns w32's result; a refusal must not write */
static int try_w(uintptr_t pa, uint32_t xorbits, unsigned o)
{
	uint32_t cur = *slot(pa);
	unsigned before = nw;
	int r = w32(pa, cur ^ xorbits, o);

	if (r)
		assert(nw == before && *slot(pa) == cur);
	return r;
}

#define NIB(n)	(0xfu << ((n) * 4u))

static void test_formatter(void)
{
	reset();
	nlines = 0;
	say("pin %-5s bank %#llx[%u] func=%#x dat=%d x=%#x %02x", "clk", 0x10c40060ull, 3u, 0u, 1, 0xabu, 6u);
	assert(!strcmp(lines[0], "SWEET TOUCH: pin clk   bank 0x10c40060[3] func=0 dat=1 x=0xab 06"));
	nlines = 0;
	say("a%sb %s", "", "the quick brown fox jumps over the lazy dog the quick brown fox jumps over the lazy dog "
	    "and then some more words to overflow one line");
	assert(nlines == 2);
	nlines = 0;
	say_result("x=%u", 3u);
	assert(!strcmp(lines[0], "SWEET TOUCH RESULT: x=3"));
}

static void test_pure(void)
{
	uint32_t g = tp_gate_open(1u << TOUCH_CMU_BIT_GATE_AUTO);
	uint8_t a[2] = { 0x36, 0x48 }, sw[2] = { 0x48, 0x36 }, no[2] = { 0, 0 };

	assert(g & (1u << TOUCH_CMU_BIT_GATE_MANUAL));
	assert(g & (1u << TOUCH_CMU_BIT_GATE_VAL));
	assert(!(g & (1u << TOUCH_CMU_BIT_GATE_AUTO)));
	assert(tp_gate_open(0xffffffffu) == (0xffffffffu & ~(1u << TOUCH_CMU_BIT_GATE_AUTO)));
	assert(TP_ST_RX_LVL(5u << 15) == 5 && TP_ST_TX_LVL(7u << 6) == 7);
	assert(tp_chip_id_match(a, a) == 1 && tp_chip_id_match(sw, a) == 2 && tp_chip_id_match(no, a) == 0);
}

/* The allowlist, bit-exact, against garbage neighbours: exactly the union of the XNU writes and the built options. */
static void test_allowlist(void)
{
	unsigned o;

	reset();
	/* SPI engine and USI wrapper */
	assert(try_w(SPI + TP_SPI_CH_CFG, 0x3, 0) == 0 && try_w(SPI + TP_SPI_TX_DATA, 0x55, 0) == 0);
	assert(try_w(SPI + TP_SPI_CS_REG, 1, 0) == 0 && try_w(SPI + TP_SPI_MODE_CFG, TP_MODE_LOOPBACK, 0) == 0);
	assert(try_w(SPI + TP_SPI_STATUS, 1, 0) == -2 && try_w(SPI + TP_SPI_RX_DATA, 1, 0) == -2);
	assert(try_w(SPI + 0x30, 1, 0) == -2 && try_w(SPI + 0x3, 1, 0) == -2 && try_w(SPI + 0xdc, 1, 0) == -2);
	assert(try_w(USI + 0, 1, 0) == -2 && try_w(USI + 0xc, 1, 0) == -2);
	assert(try_w(USI + TP_USI_CON, 1, 0) == 0 && try_w(USI + TP_USI_OPTION, 2, 0) == 0);
	/* USI sysreg: the mode field only */
	assert(try_w(TOUCH_USI_MODE_REG, 0x2, 0) == 0 && try_w(TOUCH_USI_MODE_REG, 0x8, 0) == -2);
	assert(try_w(TOUCH_USI_MODE_REG + 4, 1, 0) == -2);
	/* CMU_PERIC1: user mux sel bit, gate bits, QCH only without experiments, divider only with osc/top; never OPTION */
	assert(try_w(CMU + TOUCH_CMU_MUX_USI, SEL, 0) == 0 && try_w(CMU + TOUCH_CMU_MUX_USI, 1, 0) == -2);
	assert(try_w(CMU + TOUCH_CMU_MUX_USI, BUSY, 0) == -2);
	assert(try_w(CMU + TOUCH_CMU_GATE_IPCLK, TP_GATE_CHECK_MASK, 0) == 0 && try_w(CMU + TOUCH_CMU_GATE_IPCLK, 1, 0) == -2);
	assert(try_w(CMU + TOUCH_CMU_GATE_PCLK, 1, 0) == -2 && try_w(CMU + TOUCH_CMU_GATE_RSTN, 1, 0) == -2);
	assert(try_w(CMU + TOUCH_CMU_QCH, 3, 0) == 0 && try_w(CMU + TOUCH_CMU_QCH, 4, 0) == -2);
	assert(try_w(CMU + TOUCH_CMU_QCH, 3, TP_O_CS) == -2 && try_w(CMU + TOUCH_CMU_QCH, 3, TP_O_LOOP) == -2);
	assert(try_w(CMU + TOUCH_CMU_DIV, 5, 0) == -2 && try_w(CMU + TOUCH_CMU_DIV, 5, TP_O_CS | TP_O_RST) == -2);
	assert(try_w(CMU + TOUCH_CMU_DIV, 5, TP_O_OSC) == 0 && try_w(CMU + TOUCH_CMU_DIV, 5, TP_O_TOP) == 0);
	assert(try_w(CMU + TOUCH_CMU_DIV, 0x10, TP_O_OSC) == -2 && try_w(CMU + TOUCH_CMU_DIV, BUSY, TP_O_TOP) == -2);
	assert(try_w(CMU + TOUCH_CMU_OPTION, 1, ~0u) == -2 && try_w(CMU + TOUCH_CMU_MUX_BUS, 1, ~0u) == -2);
	assert(try_w(CMU + TOUCH_CMU_MUX_BUS_AUTO, 1, ~0u) == -2 && try_w(CMU + TOUCH_CMU_MUX_USI_AUTO, 1, ~0u) == -2);
	/* CMU_TOP: the gate bit only */
	assert(try_w(TOP + TOUCH_CMUTOP_GATE, 1u << TOUCH_CMUTOP_GATE_BIT, 0) == 0);
	assert(try_w(TOP + TOUCH_CMUTOP_GATE, 1, 0) == -2);
	assert(try_w(TOP + TOUCH_CMUTOP_MUX, 1, ~0u) == -2 && try_w(TOP + TOUCH_CMUTOP_DIV, 1, ~0u) == -2);
	/* pads: gpp23 CON/DAT for the four SPI pins always, bits of other pins never; PUD only with rst / misopu */
	assert(try_w(TOUCH_PIN_CS_BANK, 0xffff, 0) == 0 && try_w(TOUCH_PIN_CS_BANK, 1u << 16, 0) == -2);
	assert(try_w(TOUCH_PIN_CS_BANK + 4, 0xf, 0) == 0 && try_w(TOUCH_PIN_CS_BANK + 4, 1u << 4, 0) == -2);
	assert(try_w(TOUCH_PIN_CS_BANK + 8, NIB(2), 0) == -2);
	assert(try_w(TOUCH_PIN_MISO_BANK + 8, NIB(TOUCH_PIN_MISO_NR), TP_O_MISOPU) == 0);	/* MISO nibble only */
	assert(try_w(TOUCH_PIN_MISO_BANK + 8, NIB(TOUCH_PIN_CLK_NR), TP_O_MISOPU) == -2);
	assert(try_w(TOUCH_PIN_MISO_BANK + 8, NIB(TOUCH_PIN_MISO_NR) | NIB(TOUCH_PIN_CLK_NR), TP_O_MISOPU) == -2);
	assert(try_w(TOUCH_PIN_MISO_BANK + 8, NIB(TOUCH_PIN_MISO_NR), TP_O_RST) == 0);
	assert(try_w(TOUCH_PIN_CLK_BANK + 8, NIB(TOUCH_PIN_CLK_NR), TP_O_RST) == 0);
	assert(try_w(TOUCH_PIN_CLK_BANK + 8, NIB(4), TP_O_RST) == -2);	/* a pin that is not a touch pad */
	assert(try_w(TOUCH_PIN_RESET_BANK + 8, NIB(TOUCH_PIN_RESET_NR), TP_O_RST) == 0);
	/* mux pad (gpp26 pin 1): only with mux, only its own nibble / bit */
	assert(try_w(MUXB, NIB(MUXN), 0) == -2 && try_w(MUXB + 4, 1u << MUXN, 0) == -2);
	assert(try_w(MUXB, NIB(MUXN), TP_O_MUX) == 0 && try_w(MUXB + 4, 1u << MUXN, TP_O_MUX) == 0);
	assert(try_w(MUXB, NIB(MUXN) | 1u, TP_O_MUX) == -2);	/* a second bit changing */
	assert(try_w(MUXB + 4, (1u << MUXN) | (1u << 5), TP_O_MUX) == -2);
	assert(try_w(MUXB, 0xffffffffu, TP_O_MUX) == -2 && try_w(MUXB + 4, 0xffffffffu, TP_O_MUX) == -2);
	assert(try_w(MUXB + 8, NIB(MUXN), TP_O_MUX) == -2 && try_w(MUXB + 8, NIB(MUXN), TP_O_MISOPU | TP_O_CS) == -2);
	assert(try_w(MUXB + 8, NIB(MUXN), TP_O_RST) == -2);	/* PUD of the mux pad: never */
	/* reset pad only with rst, irq pad never, other banks never */
	assert(try_w(TOUCH_PIN_RESET_BANK, NIB(TOUCH_PIN_RESET_NR), 0) == -2);
	assert(try_w(TOUCH_PIN_RESET_BANK, NIB(TOUCH_PIN_RESET_NR), TP_O_RST) == 0);
	assert(try_w(TOUCH_PIN_RESET_BANK + 4, 1u << TOUCH_PIN_RESET_NR, TP_O_RST) == 0);
	assert(try_w(TOUCH_PIN_IRQ_BANK, 1, ~0u) == -2 && try_w(TOUCH_PIN_IRQ_BANK + 4, 1, ~0u) == -2);
	assert(try_w(TOUCH_PIN_IRQ_BANK + 8, 1, ~0u) == -2);
	assert(try_w(TOUCH_PIN_CS_BANK - 0x20, 1, ~0u) == -2 && try_w(TOUCH_PIN_CS_BANK + 0x20, 1, ~0u) == -2);
	/* a refused-or-faulting write is reported once and stops the sequence */
	for (o = 0; o < 128; o++)
		assert(try_w(CMU + TOUCH_CMU_OPTION, 1, o) == -2);
}

static void test_dump(void)
{
	reset();
	touch_probe_dump();	/* USI11 mode 0 at handoff */
	assert(nw == 0 && spi_reads == 0);	/* no USI/SPI register is read while the mode is not SPI */
	assert(has("diag before: skipped: usi mode="));
	assert(has("SWEET TOUCH: cmu base 0x10c00000: OPTION[0x800]="));
	assert(has("SWEET TOUCH: cmu_top 0x1e080000: PERIC1_IP mux[0x10e8]="));
	assert(has("SWEET TOUCH: cmu gates: ipclk_4[0x202c]="));
	assert(has("SWEET TOUCH: sysreg USI11 mode (0x10c2100c) = ") && has("(SPI = 0x2, so not SPI yet)"));
	assert(has("SWEET TOUCH: pin mux   bank 0x10c400c0[1] func=0 dat=0 pud=0x1"));	/* the real handoff state */
	assert(!has("unreadable"));

	reset();	/* the mode already SPI: the USI/SPI registers are read */
	*slot(TOUCH_USI_MODE_REG) |= TOUCH_USI_MODE_SPI;
	touch_probe_dump();
	assert(nw == 0 && spi_reads > 0 && has("SWEET TOUCH: diag before: ch="));

	reset();	/* an unreadable mode register: nothing in the USI/SPI window is read */
	bad_pa = TOUCH_USI_MODE_REG;
	touch_probe_dump();
	assert(nw == 0 && spi_reads == 0 && has("unreadable pa=0x10c2100c") && has("skipped"));

	reset();	/* an unreadable CMU register is reported and the dump goes on */
	bad_pa = TOP + TOUCH_CMUTOP_GATE;
	touch_probe_dump();
	assert(nw == 0 && has("unreadable pa=0x1e082104") && has("diag before: skipped"));
}

static void test_mux(void)
{
	uint32_t c0, d0;

	/* real state (func 0, dat 0) without the mux option: the bus is left alone, nothing written, no SPI read */
	reset();
	touch_probe_run(0);
	assert(nw == 0 && spi_reads == 0 && has("leaving the bus alone") && has("mux=refused"));
	assert(!has("AP drives it low"));

	/* dat high is also "not ours" */
	reset();
	*slot(MUXB) |= 1u << (MUXN * 4);
	*slot(MUXB + 4) |= 1u << MUXN;
	touch_probe_run(TP_O_MUX | TP_O_CS);
	assert(has("AP is NOT driving it low (func=0x1 dat=1)") || has("AP is NOT driving it low"));

	/* the faulting read must not look like "AP drives it low" (finding: fault read returned 0) */
	reset();
	bad_pa = MUXB;
	touch_probe_run(0);
	assert(nw == 0 && has("unreadable pa=0x10c400c0") && has("AP/AOC mux: unreadable") && !has("AP drives it low"));
	reset();
	bad_pa = MUXB + 4;
	touch_probe_run(TP_O_MUX);
	assert(nw == 0 && !has("AP drives it low") && !has("mux driven low"));

	/* with the mux option: DAT low first, then the pad becomes an output (0088), bits of other pins untouched */
	reset();
	c0 = *slot(MUXB);
	d0 = *slot(MUXB + 4);
	touch_probe_run(TP_O_MUX);
	assert(has("mux option: mux driven low, now func=0x1 dat=0"));
	assert(write_idx(MUXB + 4) >= 0 && write_idx(MUXB) > write_idx(MUXB + 4) - 1);
	assert(write_idx(MUXB + 4) < write_idx(MUXB));
	preserved(MUXB, c0, NIB(MUXN));
	preserved(MUXB + 4, d0, 1u << MUXN);
	assert(((*slot(MUXB) >> (MUXN * 4)) & 0xf) == 1 && !((*slot(MUXB + 4) >> MUXN) & 1));
	assert(write_idx(MUXB + 8) < 0);	/* its pull is never written */
	assert(write_idx(MUXB) < write_idx(SPI + TP_SPI_CH_CFG));	/* before any SPI write */
	check_run_writes(TP_O_MUX);

	/* read-back mismatch (the pad ignores the write): stop, nothing else touched */
	reset();
	ignore_wpa = MUXB;
	touch_probe_run(TP_O_MUX);
	assert(has("mux read-back mismatch") && write_idx(SPI + TP_SPI_CH_CFG) < 0 && write_idx(CMU + TOUCH_CMU_MUX_USI) < 0);
	assert(has("mux=failed"));

	/* a faulting write stops everything after it */
	reset();
	bad_wpa = MUXB;
	touch_probe_run(TP_O_MUX);
	assert(has("write FAULTED pa=0x10c400c0") && write_idx(CMU + TOUCH_CMU_MUX_USI) < 0);
}

static void test_plain_run(void)
{
	uint32_t ipc0, pcl0, rst0, usi0, mux0, top0, qch0, c23, d23, c26, d26;
	int top, mux, ip, pc, rs, q, spi;

	/* AP already owns the mux (a hypothetical handoff): the XNU sweettouchclk=3 sweettouchvar=1 sequence, chip silent */
	reset();
	*slot(MUXB) |= 1u << (MUXN * 4);
	ipc0 = *slot(CMU + TOUCH_CMU_GATE_IPCLK);
	pcl0 = *slot(CMU + TOUCH_CMU_GATE_PCLK);
	rst0 = *slot(CMU + TOUCH_CMU_GATE_RSTN);
	usi0 = *slot(TOUCH_USI_MODE_REG);
	mux0 = *slot(CMU + TOUCH_CMU_MUX_USI);
	top0 = *slot(TOP + TOUCH_CMUTOP_GATE);
	qch0 = *slot(CMU + TOUCH_CMU_QCH);
	c23 = *slot(TOUCH_PIN_CS_BANK);
	d23 = *slot(TOUCH_PIN_CS_BANK + 4);
	c26 = *slot(MUXB);
	d26 = *slot(MUXB + 4);
	touch_probe_run(0);
	check_run_writes(0);
	top = write_idx(TOP + TOUCH_CMUTOP_GATE);
	mux = write_idx(CMU + TOUCH_CMU_MUX_USI);
	ip = write_idx(CMU + TOUCH_CMU_GATE_IPCLK);
	pc = write_idx(CMU + TOUCH_CMU_GATE_PCLK);
	rs = write_idx(CMU + TOUCH_CMU_GATE_RSTN);
	q = write_idx(CMU + TOUCH_CMU_QCH);
	spi = write_idx(SPI + TP_SPI_CH_CFG);
	assert(top == 0 && top < mux && mux < ip && ip < pc && pc < rs && rs < q && q < spi);	/* CMU_TOP gate first (0092) */
	assert(wval[top] == (*slot(TOP + TOUCH_CMUTOP_GATE)) && wval[top] == (top0 | (1u << TOUCH_CMUTOP_GATE_BIT)));
	assert(wval[mux] == (mux0 | SEL));
	assert(wval[ip] == tp_gate_open(ipc0) && wval[pc] == tp_gate_open(pcl0) && wval[rs] == tp_gate_open(rst0));
	assert(wval[q] == (qch0 | (1u << TOUCH_CMU_BIT_QCH_ENABLE) | (1u << TOUCH_CMU_BIT_QCH_REQ)));
	assert(write_idx(TOUCH_USI_MODE_REG) > q);	/* the sysreg mode before any SPI/USI register is read */
	assert(write_idx(TOUCH_USI_MODE_REG) < write_idx(SPI + TP_SPI_MODE_CFG));
	/* RMW bit preservation against garbage neighbours */
	preserved(TOUCH_USI_MODE_REG, usi0, TOUCH_USI_MODE_MASK);
	assert((*slot(TOUCH_USI_MODE_REG) & TOUCH_USI_MODE_MASK) == TOUCH_USI_MODE_SPI);
	preserved(CMU + TOUCH_CMU_GATE_IPCLK, ipc0, TP_GATE_CHECK_MASK);
	preserved(CMU + TOUCH_CMU_GATE_PCLK, pcl0, TP_GATE_CHECK_MASK);
	preserved(CMU + TOUCH_CMU_GATE_RSTN, rst0, TP_GATE_CHECK_MASK);
	preserved(CMU + TOUCH_CMU_MUX_USI, mux0, SEL);
	preserved(TOP + TOUCH_CMUTOP_GATE, top0, 1u << TOUCH_CMUTOP_GATE_BIT);
	preserved(CMU + TOUCH_CMU_QCH, qch0, 3);
	preserved(TOUCH_PIN_CS_BANK, c23, 0xffffu);
	preserved(TOUCH_PIN_CS_BANK + 4, d23, 0xfu);
	preserved(MUXB, c26, 0);	/* nothing in gpp26 is written without mux / rst */
	preserved(MUXB + 4, d26, 0);
	assert(write_idx(CMU + TOUCH_CMU_OPTION) < 0 && write_idx(CMU + TOUCH_CMU_DIV) < 0);
	/* spi pads: SPI function on clk/mosi/miso, cs output */
	assert(((*slot(TOUCH_PIN_CLK_BANK) >> (TOUCH_PIN_CLK_NR * 4)) & 0xf) == TOUCH_SPI_FUNC);
	assert(((*slot(TOUCH_PIN_CS_BANK) >> (TOUCH_PIN_CS_NR * 4)) & 0xf) == 1);
	/* variants A..E once each, in order, as sweettouchvar=1 */
	{
		int prev = -1;
		for (char c = 'A'; c <= 'E'; c++) {
			char key[32], joined[256];
			int l;

			snprintf(key, sizeof(key), "TOUCH: VARIANT %c: timeout", c);
			l = line_of(key);
			assert(l > prev);
			prev = l;
			snprintf(joined, sizeof(joined), "%s %s", lines[l], lines[l + 1]);	/* long XNU lines are split at spaces */
			assert(strstr(joined, "status=") && strstr(joined, "ch=") && strstr(joined, "clk="));
		}
	}
	assert(has("phase 1 result: chip did not answer") && has("identify: SPI transfer timed out"));
	assert(*slot(SPI + TP_SPI_CLK_CFG) & TP_CLK_ENCLK && (*slot(SPI + TP_SPI_CLK_CFG) & TP_CLK_PSR_MASK) == 3);	/* A */
	assert(write_idx(SPI + TP_SPI_CS_REG) >= 0 && *slot(SPI + TP_SPI_CS_REG) == (TP_CS_AUTO | TP_CS_NSC_CNT_2));	/* E */
	assert(has("TOUCH RESULT: opts=0 mux=ap-owned") && has("id=stall"));

	/* the engine moves data only once USI_OPTION has CLKREQ (variant C): A, B timeout, C completes, D/E never run */
	reset();
	*slot(MUXB) |= 1u << (MUXN * 4);
	hw_ok = need_c = 1;
	touch_probe_run(0);
	check_run_writes(0);
	assert(has("VARIANT A: timeout") && has("VARIANT B: timeout") && has("VARIANT C: completed"));
	assert(!has("VARIANT D") && !has("VARIANT E") && has("VARIANT C completes, identifying again"));
	assert(has("identify: reg 0x20000000 = 36 48") && has("FTM5 answers") && has("phase 1 result: chip answered"));
	assert(has("chip-id-ok"));

	/* the first identify already works: no variant */
	reset();
	*slot(MUXB) |= 1u << (MUXN * 4);
	hw_ok = 1;
	touch_probe_run(0);
	assert(!has("VARIANT") && has("FTM5 answers"));
}

static void test_faults_and_bounds(void)
{
	/* a faulting CMU register stops the sequence before any SPI/pad write */
	reset();
	*slot(MUXB) |= 1u << (MUXN * 4);
	bad_pa = CMU + TOUCH_CMU_MUX_USI;
	touch_probe_run(0);
	assert(write_idx(SPI + TP_SPI_CH_CFG) < 0 && write_idx(TOUCH_USI_MODE_REG) < 0);
	assert(has("unreadable") && has("sequence stopped"));
	check_run_writes(0);

	/* a faulting WRITE (TOP gate) stops everything after it */
	reset();
	*slot(MUXB) |= 1u << (MUXN * 4);
	bad_wpa = TOP + TOUCH_CMUTOP_GATE;
	touch_probe_run(0);
	assert(has("write FAULTED pa=0x1e082104") && nw == 0);

	/* a faulting SPI register after the mode write stops the setup */
	reset();
	*slot(MUXB) |= 1u << (MUXN * 4);
	bad_pa = SPI + TP_SPI_FB_CLK;
	touch_probe_run(0);
	assert(has("unreadable pa=") && has("sequence stopped"));
	check_run_writes(0);

	/* a dead timer and a never-ready engine still terminate (poll budget), with every option */
	reset();
	*slot(MUXB) |= 1u << (MUXN * 4);
	time_frozen = 1;
	touch_probe_run(TP_O_MUX | TP_O_CS | TP_O_OSC | TP_O_LOOP | TP_O_RST | TP_O_MISOPU);
	assert(has("TOUCH RESULT:") && accesses < 80000000ul);
	reset();
	*slot(MUXB) |= 1u << (MUXN * 4);
	time_frozen = 1;
	touch_probe_run(0);
	assert(has("phase 1 result: chip did not answer"));

	/* a stuck mux BUSY bit: the clock sequence gives up */
	reset();
	*slot(MUXB) |= 1u << (MUXN * 4);
	*slot(CMU + TOUCH_CMU_MUX_USI) = BUSY;
	touch_probe_run(0);
	assert(has("still BUSY after 1 ms, clock sequence aborted"));
}

static void test_experiments(void)
{
	uint32_t div0, pud_cs0, pud_rst0, pud_mux0;

	/* ---- X1 cs: ABL-exact manual chip select; works only with CS_REG bit0 clear ---- */
	reset();
	hw_ok = need_cs = 1;
	touch_probe_run(TP_O_MUX | TP_O_CS);
	check_run_writes(TP_O_MUX | TP_O_CS);
	assert(has("VARIANT F: completed") && has("FTM5 answers") && has("F=data") && has("chip-id-ok"));
	assert(has("moved=F,I"));
	assert(write_idx(CMU + TOUCH_CMU_QCH) < 0);	/* no QCH write with experiments */
	assert(!has("VARIANT A") && !has("VARIANT E"));	/* the A..E ladder is only for plain runs */
	assert(write_idx(SPI + TP_SPI_MODE_CFG) < 0 && write_idx(SPI + TP_SPI_INT_EN) < 0 && write_idx(SPI + TP_SPI_SWAP_CFG) < 0);
	assert(write_idx(SPI + TP_SPI_PENDING_CLR) < 0);
	{
		/* order inside variant F: CS_REG &= ~1, PACKET_CNT = 5 | EN, 5 TX bytes, CH_CFG |= 3, ... CH_CFG &= ~3, PACKET_CNT 0,
		 * SW_RST, CS_REG |= 1 */
		int c0 = write_idx_val(SPI + TP_SPI_CS_REG, 0, 0), p5 = write_idx_val(SPI + TP_SPI_PACKET_CNT, 5 | TP_PKT_CNT_EN, 0);
		int t0 = write_idx_val(SPI + TP_SPI_TX_DATA, TP_FTM5_CMD_HW_REG_R, 0);
		int on, off, p0, c1;

		assert(c0 >= 0 && p5 > c0 && t0 > p5);
		assert(wval[t0 + 1] == 0x20 && wval[t0 + 2] == 0 && wval[t0 + 3] == 0 && wval[t0 + 4] == 0);
		for (on = t0 + 5; wlog[on] != SPI + TP_SPI_CH_CFG; on++)
			assert(wlog[on] == SPI + TP_SPI_TX_DATA || on > t0 + 20);
		assert((wval[on] & 3) == 3);
		off = on + 1;
		while (wlog[off] != SPI + TP_SPI_CH_CFG)
			off++;
		assert((wval[off] & 3) == 0);
		p0 = write_idx_val(SPI + TP_SPI_PACKET_CNT, 0, (unsigned)off);
		assert(p0 > off);
		assert(write_idx_val(SPI + TP_SPI_CH_CFG, wval[p0 + 1], (unsigned)p0) == p0 + 1 && (wval[p0 + 1] & TP_CH_SW_RST));
		c1 = write_idx_val(SPI + TP_SPI_CS_REG, 1, (unsigned)p0);
		assert(c1 > p0);
	}
	/* the pin-level CS goes low before CS_REG is cleared and high before CS_REG is set */
	assert((*slot(SPI + TP_SPI_CS_REG) & 1) == 1);
	assert(((*slot(TOUCH_PIN_CS_BANK + 4) >> TOUCH_PIN_CS_NR) & 1) == 1);

	/* cs fails when the engine needs more than CS_REG: stall reported, nothing wider written */
	reset();
	touch_probe_run(TP_O_MUX | TP_O_CS);
	assert(has("VARIANT F: timeout") && has("F=stall") && has("moved=-"));
	check_run_writes(TP_O_MUX | TP_O_CS);

	/* ---- X2 osc: mux bit4 := 0, poll, div[3:0] := 5, poll; restored if the burst fails ---- */
	reset();
	hw_ok = need_osc = 1;
	div0 = *slot(CMU + TOUCH_CMU_DIV);
	touch_probe_run(TP_O_MUX | TP_O_OSC);
	check_run_writes(TP_O_MUX | TP_O_OSC);
	assert(has("clk=applied") && has("chip-id-ok") && !(*slot(CMU + TOUCH_CMU_MUX_USI) & SEL));
	assert((*slot(CMU + TOUCH_CMU_DIV) & 0xf) == 5);
	preserved(CMU + TOUCH_CMU_DIV, div0, 0xf);
	assert(line_of("clk source osc") > line_of("clock sequence done") && line_of("clk source osc") < line_of("USI_CON="));
	assert(write_idx(CMU + TOUCH_CMU_DIV) > write_idx_val(CMU + TOUCH_CMU_MUX_USI, *slot(CMU + TOUCH_CMU_MUX_USI), 0) - 1);

	reset();
	div0 = *slot(CMU + TOUCH_CMU_DIV);
	touch_probe_run(TP_O_MUX | TP_O_OSC);	/* nothing moves: both registers go back */
	assert(has("clk=applied") && has("id=stall"));
	assert((*slot(CMU + TOUCH_CMU_DIV) & 0xf) == (div0 & 0xf));
	assert(*slot(CMU + TOUCH_CMU_MUX_USI) & SEL);	/* the TOP selection the clock chain had set before */
	assert(has("usi mux restore") && has("usi div restore"));
	check_run_writes(TP_O_MUX | TP_O_OSC);

	reset();	/* a mux that stays BUSY: bounded, restored */
	hw_ok = 1;
	ignore_wpa = CMU + TOUCH_CMU_MUX_USI;
	*slot(CMU + TOUCH_CMU_MUX_USI) = SEL | BUSY;
	touch_probe_run(TP_O_MUX | TP_O_OSC);
	assert(has("read-back mismatch") || has("BUSY"));
	assert(write_idx(CMU + TOUCH_CMU_DIV) < 0 || has("restoring"));

	/* ---- X6 top: bit4 := 1, div := 9 ---- */
	reset();
	hw_ok = 1;
	touch_probe_run(TP_O_MUX | TP_O_TOP);
	assert(has("clk source top") && (*slot(CMU + TOUCH_CMU_MUX_USI) & SEL) && (*slot(CMU + TOUCH_CMU_DIV) & 0xf) == 9);
	check_run_writes(TP_O_MUX | TP_O_TOP);

	/* ---- X3 loop: MODE_CFG bit3, FB_CLK 0 then 1, TX/RX compare, restored ---- */
	reset();
	loop_ok = 1;
	touch_probe_run(TP_O_MUX | TP_O_LOOP);
	check_run_writes(TP_O_MUX | TP_O_LOOP);
	assert(has("loopback FB_CLK=0: rx == tx") && has("loopback FB_CLK=1: rx == tx") && has("loop=rx==tx"));
	assert(line_of("loopback FB_CLK=0") < line_of("loopback FB_CLK=1") && line_of("loopback FB_CLK=1") < line_of("identify:"));
	assert(write_idx_val(SPI + TP_SPI_FB_CLK, 0, 0) >= 0);
	assert(!(*slot(SPI + TP_SPI_MODE_CFG) & TP_MODE_LOOPBACK));
	assert(*slot(SPI + TP_SPI_FB_CLK) == 1);	/* the value spi_setup wrote */
	assert(has("moved=L"));
	reset();
	touch_probe_run(TP_O_MUX | TP_O_LOOP);
	assert(has("loopback FB_CLK=0: timeout") && has("loop=stall"));
	assert(!(*slot(SPI + TP_SPI_MODE_CFG) & TP_MODE_LOOPBACK));

	/* ---- X4 rst: pulls off, reset pad low 10 ms, high, FIFO polled for event 3 (bounded), pulls restored ---- */
	reset();
	hw_ok = 1;
	memset(rx_pat, 0, sizeof(rx_pat));
	rx_pat[1] = TP_FTM5_EVT_CONTROLLER_READY;
	pud_cs0 = *slot(TOUCH_PIN_CS_BANK + 8);
	pud_rst0 = *slot(TOUCH_PIN_RESET_BANK + 8);
	pud_mux0 = *slot(MUXB + 8);
	touch_probe_run(TP_O_MUX | TP_O_RST);
	check_run_writes(TP_O_MUX | TP_O_RST);
	assert(has("rst: PUD of clk/mosi/miso/cs/reset") && has("controller-ready event seen") && has("rst=ready"));
	assert(*slot(TOUCH_PIN_CS_BANK + 8) == pud_cs0 && *slot(TOUCH_PIN_RESET_BANK + 8) == pud_rst0);	/* restored */
	assert(*slot(MUXB + 8) == pud_mux0);
	{
		int lo = write_idx_val(TOUCH_PIN_RESET_BANK + 4, *slot(TOUCH_PIN_RESET_BANK + 4) & ~(1u << TOUCH_PIN_RESET_NR), 0);
		assert(lo >= 0);
		assert(((*slot(TOUCH_PIN_RESET_BANK + 4) >> TOUCH_PIN_RESET_NR) & 1) == 1);	/* ends high */
		assert(((*slot(TOUCH_PIN_RESET_BANK) >> (TOUCH_PIN_RESET_NR * 4)) & 0xf) == 1);	/* output */
	}
	reset();	/* no event ever: bounded, reported */
	hw_ok = 1;
	touch_probe_run(TP_O_MUX | TP_O_RST);
	assert(has("no controller-ready event") && has("rst=data"));
	reset();	/* the FIFO read itself fails */
	touch_probe_run(TP_O_MUX | TP_O_RST);
	assert(has("rst: FIFO read timed out") && has("rst=stall"));
	check_run_writes(TP_O_MUX | TP_O_RST);

	/* ---- X5 misopu: MISO PUD := 3 for the identify, then back ---- */
	reset();
	hw_ok = 1;
	pud_cs0 = *slot(TOUCH_PIN_MISO_BANK + 8);
	touch_probe_run(TP_O_MUX | TP_O_MISOPU);
	check_run_writes(TP_O_MUX | TP_O_MISOPU);
	assert(has("misopu: MISO PUD") && has("chip-id-ok"));
	{
		uint32_t up = (pud_cs0 & ~NIB(TOUCH_PIN_MISO_NR)) | (3u << (TOUCH_PIN_MISO_NR * 4));
		int a = write_idx_val(TOUCH_PIN_MISO_BANK + 8, up, 0);

		assert(a >= 0 && write_idx_val(TOUCH_PIN_MISO_BANK + 8, pud_cs0, (unsigned)a) > a);
	}
	assert(*slot(TOUCH_PIN_MISO_BANK + 8) == pud_cs0);
	assert(line_of("misopu: MISO PUD") < line_of("identify:"));

	/* ---- everything at once, in the documented order ---- */
	reset();
	hw_ok = need_cs = need_osc = loop_ok = 1;
	touch_probe_run(TP_O_MUX | TP_O_CS | TP_O_OSC | TP_O_LOOP | TP_O_RST | TP_O_MISOPU);
	check_run_writes(TP_O_MUX | TP_O_CS | TP_O_OSC | TP_O_LOOP | TP_O_RST | TP_O_MISOPU);
	assert(line_of("AP/AOC mux") < line_of("clk source osc") && line_of("clk source osc") < line_of("VARIANT F: cs_reg"));
	assert(line_of("VARIANT F: cs_reg") < line_of("loopback FB_CLK=0") && line_of("loopback FB_CLK=1") < line_of("rst: PUD"));
	assert(line_of("rst: PUD") < line_of("misopu:") && line_of("misopu:") < line_of("identify:"));
	assert(line_of("identify:") < line_of("TOUCH RESULT:"));
	assert(has("TOUCH RESULT: opts=0x77 mux=driven-low clk=applied F=data loop=rx==tx"));
	assert(has("moved=F,L,R,I"));
}

int main(void)
{
	test_formatter();
	test_pure();
	test_allowlist();
	test_dump();
	test_mux();
	test_plain_run();
	test_faults_and_bounds();
	test_experiments();
	puts("PASS touchprobe sequence, allowlist, timeouts, experiments");
	return 0;
}
