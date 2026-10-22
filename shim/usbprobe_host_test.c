/* Host unit tests for the stage-1 read-only DWC3 probe: a fake
 * register window keyed on the exact addresses from usbprobe.h (any
 * read the probe makes to an address NOT in the window is counted and
 * must stay zero, so the test proves the probe touches only the
 * documented registers), report formatting, the unreadable path, and
 * the skip-when-GSNPSID-unreadable path. Build (also run by
 * shim/build.sh):
 *   gcc -O1 -Wall -Wextra -I. -o out/usbprobe_host_test \
 *       fdt.c usbprobe.c usbprobe_host_test.c */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fdt.h"
#include "fdt_test_builder.h"
#include "usbprobe.h"

/* Captured console output. */
static char con_log[16384];
static unsigned con_log_len;
static unsigned con_str_calls;

void con_str(const char *label, const char *s)
{
	con_str_calls++;
	if (con_log_len + strlen(label) + strlen(s) + 2 < sizeof(con_log))
		con_log_len += (unsigned)snprintf(con_log + con_log_len,
						  sizeof(con_log) - con_log_len,
						  "%s %s\n", label, s);
}

void con_hex(const char *label, uint64_t v)
{
	(void)label;
	(void)v;
}

/* ---- fake register window ---- */

struct fake_reg {
	uintptr_t addr;
	uint32_t value;
	int unreadable;
	unsigned reads;		/* times try_read32 hit this address */
};

static struct fake_reg fake[96];
static unsigned n_fake;
static unsigned unexpected_reads;

int try_read32(uintptr_t address, uint32_t *value)
{
	for (unsigned i = 0; i < n_fake; i++) {
		if (fake[i].addr == address) {
			fake[i].reads++;
			if (fake[i].unreadable)
				return 0;
			*value = fake[i].value;
			return 1;
		}
	}
	unexpected_reads++;	/* read outside the documented register set */
	return 0;
}

/* Fake pstore zone for the ramoops overlap flag: pstore.c is not
 * linked into this test, so usbprobe_report's pstore_zone_addr/size
 * calls resolve here. */
static uint64_t fake_zone_base, fake_zone_size;

uint64_t pstore_zone_addr(void)
{
	return fake_zone_base;
}

uint64_t pstore_zone_size(void)
{
	return fake_zone_size;
}

static void fake_set(uintptr_t addr, uint32_t value)
{
	for (unsigned i = 0; i < n_fake; i++)
		if (fake[i].addr == addr) {
			fake[i].value = value;
			fake[i].unreadable = 0;
			return;
		}
	assert(n_fake < sizeof(fake) / sizeof(fake[0]));
	fake[n_fake].addr = addr;
	fake[n_fake].value = value;
	fake[n_fake].unreadable = 0;
	n_fake++;
}

static void fake_unreadable(uintptr_t addr)
{
	fake_set(addr, 0);
	for (unsigned i = 0; i < n_fake; i++)
		if (fake[i].addr == addr)
			fake[i].unreadable = 1;
}

/* No register may be read twice within one usbprobe_report(): a
 * double read of a sensitive register (e.g. a counter with read
 * side effects added later) must fail the test, not ship. Asserts and
 * then clears the counters so the next report starts fresh. */
static void window_assert_single_reads(void)
{
	for (unsigned i = 0; i < n_fake; i++) {
		assert(fake[i].reads <= 1);
		fake[i].reads = 0;
	}
}

/* Host stand-ins for the linker symbols usbprobe.c gates the image
 * overlap check against. Declared in order; the assert below pins the
 * precondition the device linker script guarantees (_head < _end). */
char _head[64];
char _end[1];

static void con_log_reset(void)
{
	con_log_len = 0;
	con_log[0] = 0;
	con_str_calls = 0;
}

static void window_reset(void)
{
	n_fake = 0;
	unexpected_reads = 0;
	fake_zone_base = 0;
	fake_zone_size = 0;
}

/* Populate a plausible ABL-handoff window: dwc3 v3.20a in device mode,
 * running, enumerated at high speed... values chosen so every decoded
 * field has a distinct, hand-checkable value. */
static void window_typical(void)
{
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GSNPSID_OFF, 0x5533320Au);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GCTL_OFF, 0x00112000u);		/* prtcap=2 device */
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DCFG_OFF, 0x00000088u);		/* devaddr=0x11 */
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DCTL_OFF, 0x80000000u);		/* run_stop=1 */
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DSTS_OFF, 0x00400004u);		/* SS, halted */
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DALEPENA_OFF, 0x00000003u);	/* ep0out+ep0in */
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTADRLO_OFF, 0xFDFC8000u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTADRHI_OFF, 0x00000000u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTSIZ_OFF, 0x00001000u);	/* 4096 bytes */
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTCOUNT_OFF, 0x00000014u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GUSB2PHYCFG0_OFF, 0x00000040u);	/* susphy=1 */
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GUSB3PIPECTL0_OFF, 0x00000000u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DEPCMD_OFF(0), 0x00000409u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DEPCMDPAR0_OFF(0), 0x9F000000u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DEPCMDPAR1_OFF(0), 0x00000000u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DEPCMDPAR2_OFF(0), 0x00000000u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DEPCMD_OFF(1), 0x00000000u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DEPCMDPAR0_OFF(1), 0x00000000u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DEPCMDPAR1_OFF(1), 0x00000000u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_DEPCMDPAR2_OFF(1), 0x00000000u);
	for (unsigned i = 0; i < USBPROBE_PHY_DUMP_WORDS; i++)
		fake_set(USBPROBE_PHY_BASE + i * 4u, 0xA0000000u + i);
	fake_set(USBPROBE_PMU_PHY_USB20, 0x00000033u);
	fake_set(USBPROBE_PMU_PHY_USBDP, 0x00000011u);
}

/* FDT with one reserved-memory child covering [0xfdfc8000, +0x1000). */
static uintptr_t build_fdt_with_evbuf_reserved(void)
{
	static struct fdt_builder b;
	fb_init(&b);
	fb_begin_node(&b, "");
	fb_prop_u32(&b, "#address-cells", 2);
	fb_prop_u32(&b, "#size-cells", 1);
	fb_begin_node(&b, "reserved-memory");
	fb_prop_u32(&b, "#address-cells", 2);
	fb_prop_u32(&b, "#size-cells", 1);
	fb_begin_node(&b, "evbuf@fdfc8000");
	fb_reg21(&b, 0xfdfc8000ULL, 0x1000);
	fb_end_node(&b);
	fb_end_node(&b);
	fb_end_node(&b);
	return fb_finish(&b);
}

/* FDT whose only reserved range does NOT contain the event buffer. */
static uintptr_t build_fdt_without_evbuf_reserved(void)
{
	static struct fdt_builder b;
	fb_init(&b);
	fb_begin_node(&b, "");
	fb_prop_u32(&b, "#address-cells", 2);
	fb_prop_u32(&b, "#size-cells", 1);
	fb_begin_node(&b, "reserved-memory");
	fb_prop_u32(&b, "#address-cells", 2);
	fb_prop_u32(&b, "#size-cells", 1);
	fb_begin_node(&b, "unrelated@90000000");
	fb_reg21(&b, 0x90000000ULL, 0x100000);
	fb_end_node(&b);
	fb_end_node(&b);
	fb_end_node(&b);
	return fb_finish(&b);
}

/* FDT with FDT_MAX_RESERVED + 1 reserved-memory children: the walker
 * keeps the first 64 and counts the overflow (n_reserved_dropped), so
 * the probe must print the truncated-table warning. */
static uintptr_t build_fdt_truncated_reserved(void)
{
	static struct fdt_builder b;
	fb_init(&b);
	fb_begin_node(&b, "");
	fb_prop_u32(&b, "#address-cells", 2);
	fb_prop_u32(&b, "#size-cells", 1);
	fb_begin_node(&b, "reserved-memory");
	fb_prop_u32(&b, "#address-cells", 2);
	fb_prop_u32(&b, "#size-cells", 1);
	for (unsigned i = 0; i <= FDT_MAX_RESERVED; i++) {
		char name[20];
		snprintf(name, sizeof(name), "res%02u", i);
		fb_begin_node(&b, name);
		fb_reg21(&b, 0x90000000ULL + (uint64_t)i * 0x10000ULL, 0x1000);
		fb_end_node(&b);
	}
	fb_end_node(&b);
	fb_end_node(&b);
	return fb_finish(&b);
}

/* FDT whose single reserved range covers only the FIRST half of the
 * event buffer: "in reserved" requires full containment. */
static uintptr_t build_fdt_partial_reserved(void)
{
	static struct fdt_builder b;
	fb_init(&b);
	fb_begin_node(&b, "");
	fb_prop_u32(&b, "#address-cells", 2);
	fb_prop_u32(&b, "#size-cells", 1);
	fb_begin_node(&b, "reserved-memory");
	fb_prop_u32(&b, "#address-cells", 2);
	fb_prop_u32(&b, "#size-cells", 1);
	fb_begin_node(&b, "res@9000");
	fb_reg21(&b, 0x90000000ULL, 0x1000);
	fb_end_node(&b);
	fb_end_node(&b);
	fb_end_node(&b);
	return fb_finish(&b);
}

static void assert_line(const char *expected)
{
	char want[160];
	snprintf(want, sizeof(want), "%s\n", expected);
	const char *hit = strstr(con_log, want);
	if (!hit || (hit != con_log && hit[-1] != '\n'))
		fprintf(stderr, "MISSING: %s\nLOG:\n%s", expected, con_log);
	assert(hit != NULL);
	/* whole line: the match must start at a line boundary */
	assert(hit == con_log || hit[-1] == '\n');
}

/* ---- tests ---- */

static void test_report_formatting(void)
{
	assert((uintptr_t)_end > (uintptr_t)_head);
	window_reset();
	window_typical();
	/* Armed ramoops zone away from the event buffer: clear. */
	fake_zone_base = 0xfd500000ULL;
	fake_zone_size = 0x100000;
	con_log_reset();
	uintptr_t fdt = build_fdt_with_evbuf_reserved();
	usbprobe_report(fdt);

	assert_line("USB GSNPSID value=5533320A ip=5533 dwc3 0x5533 family");
	assert_line("USB GCTL raw=00112000 prtcap=2(device)");
	assert_line("USB DCFG raw=00000088 devaddr=11");
	assert_line("USB DCTL raw=80000000 run_stop=1 csftrst=0");
	assert_line("USB DSTS raw=00400004 speed=4(SS) halted=1");
	assert_line("USB DALEPENA raw=00000003 ep0out=1 ep0in=1");
	assert_line("USB GEVNTADR lo=FDFC8000 hi=00000000");
	assert_line("USB GEVNTSIZ raw=00001000 bytes=4096 intmask=0");
	assert_line("USB GEVNTCOUNT raw=00000014 count=20 ehb=0");
	assert_line("USB GUSB2PHYCFG0 raw=00000040 susphy=1 softrst=0");
	assert_line("USB GUSB3PIPECTL0 raw=00000000 susphy=0 softrst=0");
	assert_line("USB EP0OUT cmd=00000409 par0=9F000000");
	assert_line("USB EP0OUT par1=00000000 par2=00000000");
	assert_line("USB EP0IN cmd=00000000 par0=00000000");
	assert_line("USB EP0IN par1=00000000 par2=00000000");
	assert_line("USB PHY+00 A0000000 A0000001 A0000002 A0000003");
	assert_line("USB PHY+10 A0000004 A0000005 A0000006 A0000007");
	assert_line("USB PHY+20 A0000008 A0000009 A000000A A000000B");
	assert_line("USB PHY+30 A000000C A000000D A000000E A000000F");
	assert_line("USB PMU PHY usb20=00000033 usbdp=00000011");
	assert_line("USB EVBUF addr=00000000FDFC8000 end=00000000FDFC9000 (4096B)");
	assert_line("USB EVBUF in reserved evbuf@fdfc8000");
	assert_line("USB EVBUF clear of the shim image");
	assert_line("USB EVBUF clear of ramoops zone");
	/* Every register read hit the documented window (no stray reads). */
	assert(unexpected_reads == 0);
	/* ... and none was read twice within this report. */
	window_assert_single_reads();
}

static void test_unreadable_registers(void)
{
	window_reset();
	window_typical();
	fake_unreadable(USBPROBE_DWC3_BASE + USBPROBE_GCTL_OFF);
	fake_unreadable(USBPROBE_DWC3_BASE + USBPROBE_GEVNTSIZ_OFF);
	fake_unreadable(USBPROBE_PHY_BASE + 5 * 4);	/* PHY+10 slot 2 */
	fake_unreadable(USBPROBE_PMU_PHY_USBDP);
	con_log_reset();
	usbprobe_report(build_fdt_with_evbuf_reserved());

	assert_line("USB GSNPSID value=5533320A ip=5533 dwc3 0x5533 family");
	assert_line("USB GCTL raw=unreadable");
	assert_line("USB GEVNTSIZ raw=unreadable");
	assert_line("USB PHY+10 A0000004 unreadable A0000006 A0000007");
	assert_line("USB PMU PHY usb20=00000033 usbdp=unreadable");
	/* Unreadable GEVNTSIZ must suppress the range flags, not invent
	 * a 4096-byte range from a zeroed value. */
	assert_line("USB EVBUF GEVNTADR/SIZ unreadable; no range flags");
	assert(strstr(con_log, "addr=00000000FDFC8000 end=") == NULL);
	assert(unexpected_reads == 0);
	window_assert_single_reads();
}

static void test_skip_when_gsnpsid_unreadable(void)
{
	window_reset();
	/* Only GSNPSID exists in the window, marked unreadable: a probe
	 * that ignored the skip rule would either produce more USB lines
	 * or rack up unexpected_reads (unknown addresses return 0). */
	fake_unreadable(USBPROBE_DWC3_BASE + USBPROBE_GSNPSID_OFF);
	con_log_reset();
	usbprobe_report(build_fdt_with_evbuf_reserved());

	assert_line("USB GSNPSID unreadable");
	assert_line("USB probe skipped: core not responding");
	assert(con_str_calls == 2);
	assert(strstr(con_log, "USB GCTL") == NULL);
	assert(strstr(con_log, "USB PHY") == NULL);
	assert(strstr(con_log, "USB PMU") == NULL);
	assert(unexpected_reads == 0);
	window_assert_single_reads();
}

static void test_unknown_gsnpsid_family_still_probes(void)
{
	window_reset();
	window_typical();
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GSNPSID_OFF, 0x12345678u);
	con_log_reset();
	usbprobe_report(build_fdt_with_evbuf_reserved());

	assert_line("USB GSNPSID value=12345678 ip=1234 UNKNOWN family (expected dwc3 0x5533)");
	assert_line("USB GCTL raw=00112000 prtcap=2(device)");
	assert(unexpected_reads == 0);
	window_assert_single_reads();
}

static void test_evbuf_not_reserved_and_image_overlap(void)
{
	window_reset();
	window_typical();
	/* Point the event buffer at the fake shim image [_head, _end):
	 * both halves, since a PIE host address is wider than 32 bits. */
	uint64_t img = (uint64_t)(uintptr_t)_head;
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTADRLO_OFF,
		 (uint32_t)img);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTADRHI_OFF,
		 (uint32_t)(img >> 32));
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTSIZ_OFF, 0x00000040u);
	/* Ramoops zone also covering the event buffer: second flag fires. */
	fake_zone_base = img;
	fake_zone_size = 0x1000;
	con_log_reset();
	usbprobe_report(build_fdt_without_evbuf_reserved());

	assert(strstr(con_log, "not inside any FDT reserved range") != NULL);
	assert(strstr(con_log, "OVERLAPS the shim image") != NULL);
	assert(strstr(con_log, "OVERLAPS ramoops zone") != NULL);
	assert(strstr(con_log, "clear of ramoops zone") == NULL);
	assert(unexpected_reads == 0);
	window_assert_single_reads();
}

static void test_evbuf_zero_and_memmap_failure(void)
{
	window_reset();
	window_typical();
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTSIZ_OFF, 0x00000000u);
	con_log_reset();
	usbprobe_report(0);	/* null FDT: memmap walk fails */

	assert_line("USB EVBUF size 0: no event buffer programmed");
	assert(unexpected_reads == 0);
	window_assert_single_reads();

	/* Same window, real buffer again, but a dead FDT: the reserved
	 * flag must report the walk failure while the image flag still
	 * answers. */
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTSIZ_OFF, 0x00001000u);
	con_log_reset();
	usbprobe_report(0);
	assert_line("USB EVBUF FDT memmap walk failed; no reserved flag");
	assert_line("USB EVBUF clear of the shim image");
	assert(unexpected_reads == 0);
	window_assert_single_reads();
}

static void test_evbuf_addr_zero(void)
{
	window_reset();
	window_typical();
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTADRLO_OFF, 0u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTADRHI_OFF, 0u);
	con_log_reset();
	usbprobe_report(build_fdt_with_evbuf_reserved());

	assert_line("USB EVBUF addr 0: no event buffer programmed");
	/* addr 0 returns before any range flag line is printed. */
	assert(strstr(con_log, "clear of the shim image") == NULL);
	assert(strstr(con_log, "ramoops zone") == NULL);
	assert(unexpected_reads == 0);
	window_assert_single_reads();
}

static void test_evbuf_range_wraps(void)
{
	window_reset();
	window_typical();
	/* 0xFFFFFFFFFFFFF000 + 0x2000 wraps the 64-bit end address. */
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTADRLO_OFF, 0xFFFFF000u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTADRHI_OFF, 0xFFFFFFFFu);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTSIZ_OFF, 0x00002000u);
	con_log_reset();
	usbprobe_report(build_fdt_with_evbuf_reserved());

	assert_line("USB EVBUF range wraps; no range flags");
	assert(strstr(con_log, "EVBUF addr=") == NULL);
	assert(strstr(con_log, "reserved") == NULL);
	assert(unexpected_reads == 0);
	window_assert_single_reads();
}

static void test_evbuf_reserved_table_truncated(void)
{
	window_reset();
	window_typical();
	con_log_reset();
	usbprobe_report(build_fdt_truncated_reserved());

	assert_line("USB EVBUF reserved table truncated +1; flag unreliable");
	/* The kept 64 ranges sit at 0x90000000+, the buffer at 0xfdfc8000,
	 * so the (unreliable) containment answer is still printed. */
	assert_line("USB EVBUF not inside any FDT reserved range");
	assert_line("USB EVBUF clear of the shim image");
	assert_line("USB EVBUF ramoops zone off; no flag");
	assert(unexpected_reads == 0);
	window_assert_single_reads();
}

static void test_evbuf_partial_overlap_reserved(void)
{
	window_reset();
	window_typical();
	/* Buffer [0x90000000, 0x90002000) vs reserved [0x90000000,
	 * 0x90001000): a PARTIAL overlap is not containment - "in
	 * reserved" requires range_covers (covers vs overlaps, review-1
	 * L3), so this must report not-inside. */
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTADRLO_OFF, 0x90000000u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTADRHI_OFF, 0u);
	fake_set(USBPROBE_DWC3_BASE + USBPROBE_GEVNTSIZ_OFF, 0x00002000u);
	con_log_reset();
	usbprobe_report(build_fdt_partial_reserved());

	assert_line("USB EVBUF not inside any FDT reserved range");
	assert(strstr(con_log, "EVBUF in reserved ") == NULL);
	assert(unexpected_reads == 0);
	window_assert_single_reads();
}

int main(void)
{
	test_report_formatting();
	test_unreadable_registers();
	test_skip_when_gsnpsid_unreadable();
	test_unknown_gsnpsid_family_still_probes();
	test_evbuf_not_reserved_and_image_overlap();
	test_evbuf_zero_and_memmap_failure();
	test_evbuf_addr_zero();
	test_evbuf_range_wraps();
	test_evbuf_reserved_table_truncated();
	test_evbuf_partial_overlap_reserved();
	printf("PASS usbprobe host test: report format, unreadable path, "
	       "GSNPSID skip, EVBUF flags, edge branches, single reads\n");
	return 0;
}
