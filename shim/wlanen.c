/*
 * wlanen.c - opt-in WLAN_EN (gpp14-2) high for 200 ms and back, see wlanen.h. Freestanding, no static state.
 *
 * [src] = Google android-gs-bluejay-6.1-android16 and Linux v6.12, local copies in ~/pixelios-artifacts/refs/wlan-stage1/; docs/research/29 sections 2 (S02-S04) and 6.
 *  - Pin: DT `wl_reg_on = <&gpp14 2 0x1>` (gs101-common-wlan.dtsi:147), `pcie,wlan-gpio = <&gpp14 2 0x1>` (gs101-common.dtsi:375); PERST `gpios = <&gph2 0 0x1>` (gs101-pcie.dtsi:56).
 *  - Bank gpp14 = PERIC0 pinctrl 0x10840000 + 0x1C0 = 0x108401C0, gph2 = HSI2 pinctrl 0x14440000 + 0x0 (pinctrl-exynos-arm64.c:988,1016); exynos850_bank_type_off: CON +0x00 4 bit/pin,
 *    DAT +0x04 1 bit/pin (pinctrl-exynos-arm64.c:47-50). Function codes INPUT 0, OUTPUT 1 (samsung.h:77-78; pinctrl-samsung.h:61-62).
 *  - Linux order for "output high": samsung_gpio_direction_output = samsung_gpio_set_value (DAT read-modify-write, pinctrl-samsung.c:555-570) then samsung_gpio_set_direction
 *    (CON read-modify-write of the 4-bit field, :624-653), under one spinlock (:693-696). "input" = the CON field cleared (:656-676). dhd turns the module on with gpio_direction_output(wl_reg_on, 1)
 *    (dhd_custom_google.c:601) and off with gpio_direction_output(wl_reg_on, 0) (:616); WIFI_TURNON_DELAY = 200 ms (dhd.h:3137-3140, only WIFI_TURNOFF_DELAY is overridden in bcm4389-Kbuild:522).
 *  - POLARITY (review B2): the DT flag `0x1` of wl_reg_on / pcie,wlan-gpio is GPIO_ACTIVE_LOW, but both drivers use the legacy raw API (rc.c:1989-1995 of_get_named_gpio +
 *    gpio_direction_output(.., 0); dhd_custom_google.c:529-566,601 gpio_direction_output(wl_reg_on, 1)), which does not apply the flag (guess: Linux gpiolib legacy behaviour, gpiolib not
 *    fetched): raw HIGH = module ON. Do not "fix" the polarity here.
 *  - observed (run w1, docs/research/20 section 3a): GPP14.CON = 0x33 (pin 2 field 0 = input), DAT = 0x3 (bit 2 = 0), PERST input low, CLKREQ lvl 1, DEVWAKE 0, HOSTWAKE 0.
 */
#include <stdint.h>
#include "console.h"
#include "ramlog.h"
#include "wlanen.h"

#if SOC_GS101 && defined(WLAN_EN_RUN) && defined(WLAN_GPP14) && defined(WLAN_GPH2) && defined(WLAN_GPA7) && defined(WLAN_PMU_PCIE1_ISO) && \
	defined(WLAN_GPP14_EN_PIN) && defined(WLAN_GPH2_PERST_PIN)

_Static_assert(WLAN_GPP14_EN_PIN < 8 && WLAN_GPH2_PERST_PIN < 8, "a pinctrl bank has 8 pins");

#define EN_CON		(WLAN_GPP14)
#define EN_DAT		(WLAN_GPP14 + 4u)
#define PERST_CON	(WLAN_GPH2)
#define PERST_DAT	(WLAN_GPH2 + 4u)
#define EN_SHIFT	(4u * (unsigned)WLAN_GPP14_EN_PIN)
#define EN_CON_MASK	(0xfu << EN_SHIFT)
#define EN_DAT_MASK	(1u << (unsigned)WLAN_GPP14_EN_PIN)
#define PERST_SHIFT	(4u * (unsigned)WLAN_GPH2_PERST_PIN)
#define PERST_DAT_MASK	(1u << (unsigned)WLAN_GPH2_PERST_PIN)
#define FUNC_INPUT	0u
#define FUNC_OUTPUT	1u
#define WAIT_MS		200u

__attribute__((noipa)) static unsigned put_txt(char *dst, unsigned cap, unsigned pos, const char *s)
{
	while (*s && pos + 1 < cap)
		dst[pos++] = *s++;
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
	con_str("WLAN", buf);
}

static void say(const char *a, const char *b)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, a);

	p = put_txt(line, sizeof(line), p, b);
	emit(line, p);
}

/* "EN <what> <n1>=XXXXXXXX <n2>=XXXXXXXX" */
static void two(const char *what, const char *n1, uint32_t v1, const char *n2, uint32_t v2)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, "EN ");

	p = put_txt(line, sizeof(line), p, what);
	p = put_txt(line, sizeof(line), p, " ");
	p = put_txt(line, sizeof(line), p, n1);
	p = put_txt(line, sizeof(line), p, "=");
	p = put_hex(line, sizeof(line), p, v1, 8);
	p = put_txt(line, sizeof(line), p, " ");
	p = put_txt(line, sizeof(line), p, n2);
	p = put_txt(line, sizeof(line), p, "=");
	p = put_hex(line, sizeof(line), p, v2, 8);
	emit(line, p);
}

/* Read-only dump of the GPP14 pad registers PUD / DRV / CONPDN / PUDPDN at +0x08 / +0x0C / +0x10 / +0x14 (exynos850_bank_type_off reg_offset {0x00,0x04,0x08,0x0c,0x10,0x14},
 * pinctrl-exynos-arm64.c:47-50): what the pin floats to after the restore and the drive used for the high level (review B3). Reads only, never written. */
static void pads(const char *what);

/* "EN <what> <n>=XXXXXXXX" */
static void one(const char *what, const char *n, uint32_t v)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, "EN ");

	p = put_txt(line, sizeof(line), p, what);
	p = put_txt(line, sizeof(line), p, " ");
	p = put_txt(line, sizeof(line), p, n);
	p = put_txt(line, sizeof(line), p, "=");
	p = put_hex(line, sizeof(line), p, v, 8);
	emit(line, p);
}

static int r32(uintptr_t pa, uint32_t *v)
{
	return try_read32(pa, v);
}

static void pads(const char *what)
{
	uint32_t a = 0, b = 0, c = 0, d = 0;
	int ok = r32(WLAN_GPP14 + 0x08u, &a) && r32(WLAN_GPP14 + 0x0cu, &b) && r32(WLAN_GPP14 + 0x10u, &c) && r32(WLAN_GPP14 + 0x14u, &d);

	if (!ok) {
		say("EN pads unreadable: ", what);
		return;
	}
	two(what, "PUD", a, "DRV", b);
	two(what, "CONPDN", c, "PUDPDN", d);
}

/* The bits of the register at pa that a write may change; 0 = not writable. Exactly the WLAN_EN field of GPP14.CON and the WLAN_EN bit of GPP14.DAT. */
static uint32_t wmask(uintptr_t pa)
{
	if (pa == EN_CON)
		return EN_CON_MASK;
	if (pa == EN_DAT)
		return EN_DAT_MASK;
	return 0u;
}

/* v may differ from old only inside the mask; the CON field may only become INPUT or OUTPUT (never a pad function). */
static int w32_allowed(uintptr_t pa, uint32_t old, uint32_t v)
{
	uint32_t m = wmask(pa);

	if (m == 0u || ((old ^ v) & ~m) != 0u)
		return 0;
	if (pa == EN_CON && ((v & m) >> EN_SHIFT) > FUNC_OUTPUT)
		return 0;
	return 1;
}

/* The ONLY writer: read-modify-write of the masked bits (bits outside the mask of pa are ignored), through the allowlist. noinline keeps the one
 * try_write32 call site inside this symbol (the object audit checks it). 0 = written, -1 = fault, -2 = refused by the allowlist (nothing written). */
__attribute__((noipa)) static int w32(uintptr_t pa, uint32_t bits)
{
	uint32_t old = 0, v;

	if (wmask(pa) == 0u)
		return -2;			/* not one of the two registers: not even read */
	if (!try_read32(pa, &old))
		return -1;
	v = (old & ~wmask(pa)) | (bits & wmask(pa));
	if (!w32_allowed(pa, old, v))
		return -2;
	return try_write32(pa, v) ? 0 : -1;
}

/* read one DAT word into *out and return the '0' / '1' / '?' of the bit under mask */
static char pinc(uintptr_t pa, uint32_t mask, uint32_t *out)
{
	uint32_t v = 0;
	int ok = r32(pa, &v);

	*out = v;
	return !ok ? '?' : (v & mask) ? '1' : '0';
}

static const char *precondition(uint32_t con, uint32_t dat, uint32_t pcon, uint32_t pdat)
{
	if (((con & EN_CON_MASK) >> EN_SHIFT) != FUNC_INPUT)
		return "WLAN_EN gpp14-2 is not a plain input (CON field != 0)";
	if (dat & EN_DAT_MASK)
		return "WLAN_EN gpp14-2 reads high already";
	if (((pcon >> PERST_SHIFT) & 0xfu) > FUNC_OUTPUT)
		return "PERST gph2-0 is in a pad function (CON field > 1)";
	if (pdat & PERST_DAT_MASK)
		return "PERST gph2-0 reads high";
	return 0;
}

/* Put the pin back, unconditionally (guess, Samsung GPIO semantics, not verified on this SoC: the DAT latch of an input pin is not observable, reading DAT shows the pin; the write is made regardless): DAT first (drive low while still an output; the
 * read-back is the latch itself only while the pin is an output), then the CON field; then read both back. 1 = restored (both writes accepted AND the words read back). */
static int restore(uint32_t con0, uint32_t dat0)
{
	uint32_t con = 0, dat = 0;
	int e1 = w32(EN_DAT, dat0), e2, ok;

	ok = r32(EN_DAT, &dat) && (dat & EN_DAT_MASK) == (dat0 & EN_DAT_MASK);
	e2 = w32(EN_CON, con0);
	if (!r32(EN_DAT, &dat) || !r32(EN_CON, &con))
		return 0;
	two("restore", "CON", con, "DAT", dat);
	return ok && e1 == 0 && e2 == 0 && con == con0 && (dat & EN_DAT_MASK) == (dat0 & EN_DAT_MASK);
}

static void summary(const char *enable, const char *sample, const char *restore_s)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, "EN enable=");

	p = put_txt(line, sizeof(line), p, enable);
	p = put_txt(line, sizeof(line), p, " sample=");
	p = put_txt(line, sizeof(line), p, sample);
	p = put_txt(line, sizeof(line), p, " restore=");
	p = put_txt(line, sizeof(line), p, restore_s);
	emit(line, p);
}

int wlan_en_run(void)
{
	uint32_t con0 = 0, dat0 = 0, pcon = 0, pdat = 0, con1 = 0, dat1 = 0, iso = 0, hdat = 0, cdat = 0, ddat = 0;
	char sample[6];
	const char *why;
	int e1, e2 = -9, enable_ok, restored;

	sample[0] = 'n'; sample[1] = '/'; sample[2] = 'a'; sample[3] = 0;
	if (!counter_freq()) {
		say("EN refused: ", "no timer, nothing written");
		summary("refused", sample, "n/a");
		return 1;
	}
	if (!r32(EN_CON, &con0) || !r32(EN_DAT, &dat0) || !r32(PERST_CON, &pcon) || !r32(PERST_DAT, &pdat)) {
		say("EN refused: ", "GPP14 / GPH2 words unreadable, nothing written");
		summary("refused", sample, "n/a");
		return 1;
	}
	two("before GPP14", "CON", con0, "DAT", dat0);
	two("before GPH2", "CON", pcon, "DAT", pdat);
	pads("before pads GPP14");
	if (r32(WLAN_PMU_PCIE1_ISO, &iso))
		one("before PMU", "ISO1", iso);
	why = precondition(con0, dat0, pcon, pdat);
	if (why) {
		say("EN refused: ", why);
		say("EN refused: ", "nothing written");
		summary("refused", sample, "n/a");
		return 1;
	}

	/* enable, in Linux's order: DAT first, then the CON field (pinctrl-samsung.c:693-696) */
	e1 = w32(EN_DAT, EN_DAT_MASK);
	if (e1 == 0)
		e2 = w32(EN_CON, FUNC_OUTPUT << EN_SHIFT);
	enable_ok = e1 == 0 && e2 == 0;
	if (r32(EN_CON, &con1) && r32(EN_DAT, &dat1)) {
		two("after enable", "CON", con1, "DAT", dat1);
		enable_ok = enable_ok && ((con1 & EN_CON_MASK) >> EN_SHIFT) == FUNC_OUTPUT && (con1 & ~EN_CON_MASK) == (con0 & ~EN_CON_MASK) &&
			    (dat1 & EN_DAT_MASK) != 0u;
	} else {
		enable_ok = 0;
	}
	if (enable_ok) {
		wait_ticks(counter_freq() / 1000u * WAIT_MS);	/* 200 ms on the counter */
		sample[0] = pinc(EN_DAT, EN_DAT_MASK, &dat1);
		sample[1] = pinc(PERST_DAT, PERST_DAT_MASK, &pdat);
		sample[2] = pinc(PERST_DAT, 1u << 1, &cdat);		/* CLKREQ gph2-1 */
		sample[3] = pinc(PERST_DAT, 1u << 5, &ddat);		/* DEVWAKE gph2-5 */
		sample[4] = pinc(WLAN_GPA7 + 4u, 1u, &hdat);		/* HOSTWAKE gpa7-0 */
		sample[5] = 0;
		(void)r32(EN_CON, &con1);
		two("sample GPP14", "CON", con1, "DAT", dat1);
		two("sample GPH2", "DAT", pdat, "GPA7.DAT", hdat);
		if (r32(WLAN_PMU_PCIE1_ISO, &iso))
			one("sample PMU", "ISO1", iso);
	}

	/* ALWAYS put the pin back, whatever the enable did (a failed first write may still have latched DAT) */
	restored = restore(con0, dat0);
	pads("after pads GPP14");
	if (!restored) {
		say("!!! RESTORE FAILED: ", "WLAN_EN gpp14-2 may be LEFT as an output / HIGH (see the restore line above)");
		say("!!! RESTORE FAILED: ", "module may stay powered; reset the phone to clear (guess: the pin state is lost on a reset)");
	}
	summary(enable_ok ? "ok" : "fault", sample, restored ? "ok" : "FAILED");
	return enable_ok && restored ? 0 : 1;
}

#else	/* not gs101 / --wlan-en not built / values missing: refuse, nothing written */

int wlan_en_run(void)
{
	con_str("WLAN", "EN refused: --wlan-en not built or soc.json wlan pins missing, nothing written");
	return 1;
}

#endif
