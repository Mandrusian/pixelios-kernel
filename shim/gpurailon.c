/*
 * gpurailon.c - opt-in S2MPG11 BUCK2S (shader rail) on / restore, see gpurailon.h. Freestanding, no static state, no MMIO of its own.
 *
 * The write: acpm_s11_rail_set() in acpm.c (GPU_RAIL_ON images). Fields, all *read from source* (GK kernel/gs android-gs-bluejay-5.10-android13-qpr3, local copies in
 * ~/pixelios-artifacts/scratch/acpm-src/): BUCK2S enable register = S2MPG11_PM_B2S_CTRL 0x15 (s2mpg11-register.h:48, regulator.c:259-260), enable mask = bits 7:6
 * (S2MPG11_REG_ENABLE_MASK_7_6 = 0xC0, register.h:335); the mode value is `mode << shift & mask` (s2m_set_mode, regulator.c:48-73) with mode 3 = SEC_OPMODE_ON "ON in
 * normal mode" (s2mpg11_of_map_mode, :35-46); disable writes 0 under the mask (s2m_disable, :95-111). The enable value is MODE 1 (SUSPEND, 0x40) because that is what LINUX
 * writes for this rail [read from source]: the DT gives BUCK2S regulator-initial-mode = SEC_OPMODE_SUSPEND and regulator-always-on (gs101-pmic.dtsi:766-779), s2m_set_mode
 * stores opmode[id] = mode << shift & mask (:48-73; the probe starts with opmode[id] = enable_mask, :718) and s2m_enable writes opmode[id] under the mask (:75-91); a rail
 * left behind with 0x40 equals stock Android (mode 3 would not). The observed BUCK8S (L2 rail) sits at mode 3 (run g1rails2: 0xD8), ABL's choice. Soft start: S2MPG11_ENABLE_TIME_BUCK = 130 us (register.h:339, the regulator core's enable_time);
 * we wait 8x that (guess for the margin), then read back once.
 * Voltage: never written. The enable is refused unless OUT1 (0x16) is inside soc.json's window (550000..700000 uV; DT range 450-1300 mV, gs101-pmic.dtsi:766-768;
 * observed 600000 uV, fvmap G3D init level 562500 uV).
 */
#include <stdint.h>
#include "console.h"
#include "ramlog.h"
#include "acpm.h"
#include "gpurailon.h"

#if SOC_GS101 && defined(GPU_RAIL_ON) && defined(ACPM_S11_ON_KNOWN) && ACPM_S11_ON_KNOWN && defined(ACPM_S11_KNOWN) && ACPM_S11_KNOWN

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

__attribute__((noipa)) static unsigned put_hex(char *dst, unsigned cap, unsigned pos, uint32_t v, unsigned digits)
{
	for (unsigned i = 0; i < digits && pos + 1 < cap; i++) {
		unsigned d = (v >> (4 * (digits - 1 - i))) & 0xfu;

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

static void say(const char *a, const char *b, const char *c)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, "RAIL ");

	p = put_txt(line, sizeof(line), p, a);
	p = put_txt(line, sizeof(line), p, b);
	p = put_txt(line, sizeof(line), p, c);
	emit(line, p);
}

/* "RAIL <what> B2S_CTRL=0x.. mode=n B2S_OUT1=0x.. uV=<n> OUT2=0x.." (uV from OUT1 with the source formula; OUT1 being the live output is a guess; OUT2 printed
 * because the unknown bits 5:0 of CTRL might select it, review I1) */
static void state_line(const char *what, uint32_t ctrl, uint32_t out1, uint32_t out2)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, "RAIL ");

	p = put_txt(line, sizeof(line), p, what);
	p = put_txt(line, sizeof(line), p, " B2S_CTRL=0x");
	p = put_hex(line, sizeof(line), p, ctrl, 2);
	p = put_txt(line, sizeof(line), p, " mode=");
	p = put_dec(line, sizeof(line), p, (ctrl & ACPM_S11_ON_MASK) >> 6);
	p = put_txt(line, sizeof(line), p, " B2S_OUT1=0x");
	p = put_hex(line, sizeof(line), p, out1, 2);
	p = put_txt(line, sizeof(line), p, " uV=");
	p = put_dec(line, sizeof(line), p, ACPM_S11_BUCK_MIN_UV + ACPM_S11_BUCK_STEP_UV * out1);
	p = put_txt(line, sizeof(line), p, " OUT2=0x");
	p = put_hex(line, sizeof(line), p, out2, 2);
	emit(line, p);
}

/* the one summary line: `GPU RAILON enable=.. readback=.. restore=.. power=..` */
static void summary(int state, const char *restore, int power)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, "RAILON enable=");

	p = put_txt(line, sizeof(line), p, state == RAIL_ON_OK ? "ok" : state == RAIL_REFUSED ? "refused" : "unverified");
	p = put_txt(line, sizeof(line), p, " readback=");
	p = put_txt(line, sizeof(line), p, state == RAIL_ON_OK ? "ok" : state == RAIL_MISMATCH ? "mismatch" : "n/a");
	p = put_txt(line, sizeof(line), p, " restore=");
	p = put_txt(line, sizeof(line), p, restore);
	p = put_txt(line, sizeof(line), p, " power=");
	p = put_txt(line, sizeof(line), p, power == RAIL_POWER_RAN ? "ran" : power == RAIL_POWER_NONE ? "not-requested" : "skipped");
	emit(line, p);
}

/* the transaction like the touch probe's lines: command words, reply words, status */
static void xline(const char *what, const struct acpm_xfer *x)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p, i;

	if (!x->seq && !x->err)
		return;
	for (unsigned k = 0; k < 2; k++) {
		p = put_txt(line, sizeof(line), 0, "RAIL ");
		p = put_txt(line, sizeof(line), p, what);
		p = put_txt(line, sizeof(line), p, k ? " rep=" : " req=");
		for (i = 0; i < 4; i++) {
			p = put_hex(line, sizeof(line), p, k ? x->rep[i] : x->req[i], 8);
			p = put_txt(line, sizeof(line), p, i < 3 ? " " : "");
		}
		emit(line, p);
	}
	p = put_txt(line, sizeof(line), 0, "RAIL ");
	p = put_txt(line, sizeof(line), p, what);
	p = put_txt(line, sizeof(line), p, " seq=");
	p = put_dec(line, sizeof(line), p, x->seq);
	p = put_txt(line, sizeof(line), p, " err=");
	p = put_dec(line, sizeof(line), p, (uint32_t)x->err);
	p = put_txt(line, sizeof(line), p, " ret=");
	p = put_dec(line, sizeof(line), p, x->ret);
	p = put_txt(line, sizeof(line), p, " writes=");
	p = put_dec(line, sizeof(line), p, x->n);
	p = put_txt(line, sizeof(line), p, " pub=");
	p = put_dec(line, sizeof(line), p, x->published);
	emit(line, p);
	if (x->err)
		say("refused/failed: ", acpm_why(x->why), x->n ? " (writes were made)" : " (nothing written)");
	if (x->err && x->published) {
		say("STATE: ", "the request stays queued; channel left as is, rerun only after reading this", "");
	}
}

/* the ACPM-side preconditions shared by enable and restore; 0 = fine, else the refusal was printed */
static int preflight(struct acpm_cfg *c)
{
	struct acpm_chan h;

	acpm_cfg_init(c);
	if (!c->layout_verified) {
		say("on refused: ", acpm_why(ACPM_W_LAYOUT), "");
		return 1;
	}
	if (!c->elem_unit_verified) {
		say("on refused: ", acpm_why(ACPM_W_UNIT), "");
		return 1;
	}
	if (!counter_freq()) {
		say("on refused: ", acpm_why(ACPM_W_TIMER), "");
		return 1;
	}
	if (acpm_describe(c, &h)) {
		say("on refused: ", acpm_why(h.why), " (describe, nothing written)");
		return 1;
	}
	if (h.rr != h.rf || h.sr != h.sf) {
		say("on refused: ", acpm_why(ACPM_W_BUSY), " (queues not empty, nothing written)");
		return 1;
	}
	return 0;
}

/* one read-back of BUCK2S_CTRL and OUT1 */
static int readback(const struct acpm_cfg *c, struct acpm_trace *t, uint32_t *ctrl, uint32_t *out1, uint32_t *out2)
{
	t->n = 0;
	if (acpm_s11_read(c, ACPM_S11_ON_REG, ctrl, t))
		return 1;
	if (acpm_s11_read(c, ACPM_S11_ON_OUT1, out1, t))
		return 1;
	if (acpm_s11_read(c, ACPM_S11_R_B2S_OUT2, out2, t))
		return 1;
	return 0;
}

int gpu_rail_enable(void)
{
	struct acpm_cfg c;
	struct acpm_trace t;
	uint32_t out1 = 0, out2 = 0, ctrl = 0, out1b = 0, out2b = 0;
	int e, state;

	if (preflight(&c))
		return RAIL_REFUSED;
	acpm_xfer_zero(&t.a);
	acpm_xfer_zero(&t.b);
	t.seq = 1;
	t.n = 0;
	if (acpm_s11_read(&c, ACPM_S11_R_B2S_OUT2, &out2, &t)) {		/* OUT2 for the before line: a read, nothing written */
		xline("on OUT2 read", &t.a);
		say("on result: ", "REFUSED, OUT2 read failed, nothing written to the PMIC", "");
		return RAIL_REFUSED;
	}
	e = acpm_s11_rail_set(&c, 1, &t, &out1);		/* pre-read CTRL, read OUT1, then at most ONE update (t.n == 3 once it is built) */
	xline("on pre-read", &t.a);
	state_line("on before", t.a.value, out1, out2);
	xline("on update", &t.b);
	if (e) {
		if (t.n == 3 && t.b.published) {		/* the UPDATE was published: the effect is unknown */
			say("on result: ", "update published but failed, effect unknown, restore follows", "");
			return RAIL_UNVERIFIED;
		}
		say("on result: ", "REFUSED, nothing written to the PMIC", "");	/* a refusal, or a failed READ: no PMIC write was made */
		return RAIL_REFUSED;
	}
	wait_ticks(counter_freq() / 1000000u * ACPM_S11_ON_SOFT_US * 8u + 1u);	/* 8x the driver's enable_time (130 us, register.h:339) */
	if (readback(&c, &t, &ctrl, &out1b, &out2b)) {
		xline("on readback", &t.a);
		say("on result: ", "read-back failed, effect unknown, restore follows", "");
		return RAIL_UNVERIFIED;
	}
	state_line("on after", ctrl, out1b, out2b);
	state = ((ctrl & ACPM_S11_ON_MASK) == ACPM_S11_ON_VAL_ON && out1b == out1) ? RAIL_ON_OK : RAIL_MISMATCH;
	say("on result: ", state == RAIL_ON_OK ? "rail is on, voltage unchanged" : "MISMATCH (mode not on or OUT1 changed), no power cycle, restore follows", "");
	return state;
}

void gpu_rail_restore(int state, int power)
{
	struct acpm_cfg c;
	struct acpm_trace t;
	uint32_t ctrl = 0, out1 = 0, out2 = 0;
	int e, ok = 0;

	if (state == RAIL_REFUSED) {
		summary(state, "none", power);
		return;
	}
	if (preflight(&c)) {
		say("!!! RESTORE FAILED: ", "BUCK2S_CTRL may be LEFT ON, preconditions failed", "");
		summary(state, "FAILED", power);
		return;
	}
	acpm_xfer_zero(&t.a);
	acpm_xfer_zero(&t.b);
	t.seq = 1;
	t.n = 0;
	e = acpm_s11_rail_set(&c, 0, &t, 0);			/* pre-read CTRL; one update to mode 0 unless it is already off */
	xline("restore pre-read", &t.a);
	xline("restore update", &t.b);
	if (!e || e == ACPM_E_ON) {				/* ACPM_E_ON = already off */
		wait_ticks(counter_freq() / 1000u);
		if (!readback(&c, &t, &ctrl, &out1, &out2)) {
			state_line("restore after", ctrl, out1, out2);
			ok = (ctrl & ACPM_S11_ON_MASK) == ACPM_S11_ON_VAL_OFF;
		}
	}
	if (!ok)
		say("!!! RESTORE FAILED: ", "S2MPG11 BUCK2S_CTRL may be LEFT ON (see the lines above)", "");
	summary(state, ok ? "ok" : "FAILED", power);
}

#else	/* not gs101 / --gpu-rail-on not built / config unknown: refuse, no ACPM traffic */

int gpu_rail_enable(void)
{
	con_str("GPU", "RAIL on refused: acpm.s2mpg11.rail_on null/unknown, no ACPM request sent");
	return RAIL_REFUSED;
}

void gpu_rail_restore(int state, int power)
{
	(void)state; (void)power;
}

#endif
