/*
 * wlanprobe.c - read-only BCM4389 Wi-Fi path probe (see wlanprobe.h).
 *
 * Every register access is try_read32() (a bus fault prints "unreadable"
 * and the probe continues). No MMIO stores of any kind, no SMC, no GPIO
 * changes; the object is audited by tools/test-bluejay-boot.py with the
 * same stack-only-stores policy as usbprobe.o. No static state: the
 * summary re-reads the pins instead of keeping them.
 *
 * Register meaning (all [src], docs/research/12 section 1.2 and 4):
 *  - PMU +0x3EC4 bit0 = PCIe1 PHY isolation (1 = bypass/PHY powered)
 *  - ELBI +0x2C8 RDLH_LINKUP, &0x3F == 0x11 is L0; +0x54 APP_LTSSM_ENABLE;
 *    +0x2E8 PM_DSTATE; +0x3A4 SOFT_RESET (pcie-exynos-rc.h:44-62)
 *  - DBI +0x0 vendor/device, +0x8 class (live DWC RC answers)
 *  - gpp14-2 WLAN_EN, gph2-0 PERST, gph2-1 CLKREQ (func 2), gph2-5 dev_wake,
 *    gpa7-0 host_wake. Bank layout CON +0x0 / DAT +0x4 is an assumption
 *    for gs101 (TODO(verify)); no existing shim code reads gpa7, so the
 *    "sanity check against the volume key" has no code to reuse.
 */
#include <stdint.h>
#include "console.h"
#include "ramlog.h"
#include "wlanprobe.h"

#if SOC_GS101 && defined(WLAN_ELBI)

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
	con_str("WLAN", buf);
}

/* "<name> pa=XXXXXXXX v=XXXXXXXX" or "... unreadable"; returns 1 and *v when read. */
static int rd(const char *name, uintptr_t pa, uint32_t *v)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = 0;
	int ok = try_read32(pa, v);
	p = put_txt(line, sizeof(line), p, name);
	p = put_txt(line, sizeof(line), p, " pa=");
	p = put_hex(line, sizeof(line), p, pa, 8);
	if (ok) {
		p = put_txt(line, sizeof(line), p, " v=");
		p = put_hex(line, sizeof(line), p, *v, 8);
	} else {
		p = put_txt(line, sizeof(line), p, " unreadable");
	}
	emit(line, p);
	return ok;
}

/* rd() with the name "<pfx>+OOO" built on the stack. */
static int rd_off(const char *pfx, uintptr_t base, uint32_t off, uint32_t *v)
{
	char nm[24];
	unsigned p = put_txt(nm, sizeof(nm), 0, pfx);
	p = put_txt(nm, sizeof(nm), p, "+");
	p = put_hex(nm, sizeof(nm), p, off, 3);
	nm[p] = 0;
	return rd(nm, base + off, v);
}

static void rd_words(const char *pfx, uintptr_t base, uint32_t first, uint32_t last)
{
	uint32_t v;
	for (uint32_t off = first; off <= last; off += 4)
		rd_off(pfx, base, off, &v);
}

/* "<name> func=X lvl=Y" for one pin of a CON/DAT pair. */
static void pin_line(const char *name, int con_ok, uint32_t con, int dat_ok, uint32_t dat,
		     unsigned pin)
{
	char line[RAMLOG_LINE_MAX];
	unsigned p = put_txt(line, sizeof(line), 0, name);
	p = put_txt(line, sizeof(line), p, " func=");
	p = con_ok ? put_hex(line, sizeof(line), p, wlan_gpio_func(con, pin), 1)
		   : put_txt(line, sizeof(line), p, "?");
	p = put_txt(line, sizeof(line), p, " lvl=");
	p = dat_ok ? put_hex(line, sizeof(line), p, wlan_gpio_level(dat, pin), 1)
		   : put_txt(line, sizeof(line), p, "?");
	emit(line, p);
}

/* Read a bank's CON/DAT and print the raw words (the pins are decoded by the caller). */
static void bank(const char *con_name, const char *dat_name, uintptr_t base,
		 int *con_ok, uint32_t *con, int *dat_ok, uint32_t *dat)
{
	*con_ok = rd(con_name, base, con);
	*dat_ok = rd(dat_name, base + 4, dat);
}

void wlan_probe_l1(void)
{
	uint32_t v, con, dat;
	int con_ok, dat_ok;

	rd("PMU_PCIE1_ISO", WLAN_PMU_PCIE1_ISO, &v);
	rd("PD_HSI2_A", WLAN_PD_HSI2_A, &v);
	rd("PD_HSI2_A+4", WLAN_PD_HSI2_A + 4, &v);
	rd("PD_HSI2_B", WLAN_PD_HSI2_B, &v);
	rd("PD_HSI2_B+4", WLAN_PD_HSI2_B + 4, &v);

	bank("GPP14.CON", "GPP14.DAT", WLAN_GPP14, &con_ok, &con, &dat_ok, &dat);
	pin_line("WLAN_EN gpp14-2", con_ok, con, dat_ok, dat, 2);
	bank("GPH2.CON", "GPH2.DAT", WLAN_GPH2, &con_ok, &con, &dat_ok, &dat);
	pin_line("PERST gph2-0", con_ok, con, dat_ok, dat, 0);
	pin_line("CLKREQ gph2-1", con_ok, con, dat_ok, dat, 1);
	pin_line("DEVWAKE gph2-5", con_ok, con, dat_ok, dat, 5);
	bank("GPA7.CON", "GPA7.DAT", WLAN_GPA7, &con_ok, &con, &dat_ok, &dat);
	pin_line("HOSTWAKE gpa7-0", con_ok, con, dat_ok, dat, 0);
}

void wlan_probe_l2(void)
{
	char line[RAMLOG_LINE_MAX];
	uint32_t v, en = 0, perst = 0, link = 0, id = 0;
	int en_ok, perst_ok, link_ok, id_ok;
	unsigned p;

	rd_words("SYSREG", WLAN_SYSREG, 0x00, 0x1c);
	rd_words("ELBI", WLAN_ELBI, 0x00, 0x40);
	rd_off("ELBI", WLAN_ELBI, 0x54, &v);		/* APP_LTSSM_ENABLE */
	link_ok = rd_off("ELBI", WLAN_ELBI, 0x2c8, &link);	/* RDLH_LINKUP */
	rd_off("ELBI", WLAN_ELBI, 0x2e8, &v);		/* PM_DSTATE */
	rd_off("ELBI", WLAN_ELBI, 0x3a4, &v);		/* SOFT_RESET */
	rd_words("PMA", WLAN_PMA, 0x00, 0x1c);
	rd_words("PCS", WLAN_PCS, 0x00, 0x1c);
	id_ok = rd_off("DBI", WLAN_DBI, 0x00, &id);
	rd_off("DBI", WLAN_DBI, 0x04, &v);
	rd_off("DBI", WLAN_DBI, 0x08, &v);
	rd_off("DBI", WLAN_DBI, 0x0c, &v);

	p = put_txt(line, sizeof(line), 0, "SUMMARY en=");
	/* Re-read the pins (no static state). */
	en_ok = try_read32(WLAN_GPP14 + 4, &en);
	p = en_ok ? put_hex(line, sizeof(line), p, wlan_gpio_level(en, 2), 1)
		   : put_txt(line, sizeof(line), p, "?");
	p = put_txt(line, sizeof(line), p, " perst=");
	perst_ok = try_read32(WLAN_GPH2 + 4, &perst);
	p = perst_ok ? put_hex(line, sizeof(line), p, wlan_gpio_level(perst, 0), 1)
		   : put_txt(line, sizeof(line), p, "?");
	p = put_txt(line, sizeof(line), p, " dbi_id=");
	p = id_ok ? put_hex(line, sizeof(line), p, id, 8)
		  : put_txt(line, sizeof(line), p, "unreadable");
	p = put_txt(line, sizeof(line), p, " link=");
	p = link_ok ? put_hex(line, sizeof(line), p, link & 0x3fu, 2)
		    : put_txt(line, sizeof(line), p, "?");
	emit(line, p);
}

#else	/* not gs101 (or no WLAN addresses): empty stub */

void wlan_probe_l1(void) {}
void wlan_probe_l2(void) {}

#endif
