/* Host unit tests for the probe RAM log: page counting, wraparound,
 * overflow eviction. Build: gcc -o out/ramlog_host_test ramlog.c ramlog_host_test.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ramlog.h"

static void append_numbered(const char *prefix, unsigned width, unsigned first, unsigned count)
{
	char line[RAMLOG_LINE_MAX];
	for (unsigned i = 0; i < count; i++) {
		snprintf(line, sizeof line, "%s%0*u", prefix, (int)width, first + i);
		ramlog_line(line);
	}
}

static void test_page_count(void)
{
	ramlog_reset();
	assert(ramlog_lines() == 0);
	assert(ramlog_page_count(1) == 0);
	assert(ramlog_page_count(48) == 0);
	append_numbered("line ", 1, 0, 10);
	assert(ramlog_lines() == 10);
	assert(ramlog_page_count(3) == 4);
	assert(ramlog_page_count(5) == 2);
	assert(ramlog_page_count(10) == 1);
	assert(ramlog_page_count(48) == 1);
	assert(ramlog_page_count(0) == 0);
}

static void test_copy_line(void)
{
	ramlog_reset();
	ramlog_line("UFS hce=00000001");
	ramlog_line("UFS hcs=0000010F");
	char buf[RAMLOG_LINE_MAX];
	assert(ramlog_copy_line(0, buf, sizeof buf) == 16);
	assert(!strcmp(buf, "UFS hce=00000001"));
	assert(ramlog_copy_line(1, buf, sizeof buf) == 16);
	assert(!strcmp(buf, "UFS hcs=0000010F"));
	assert(ramlog_copy_line(2, buf, sizeof buf) == 0);
	assert(buf[0] == 0);
	assert(ramlog_copy_line(0, buf, 4) == 3);
	assert(!strcmp(buf, "UFS"));
}

/* Fixed 64-char lines (65 stored bytes each): after enough appends the
 * write cursor wraps and the oldest lines are evicted whole. */
static void test_wraparound(void)
{
	const unsigned line_len = 64;
	const unsigned stored_len = line_len + 1;
	const unsigned total = 4 * RAMLOG_SIZE / stored_len;
	ramlog_reset();
	append_numbered("W", 63, 0, total);
	unsigned lines = ramlog_lines();
	assert(lines == RAMLOG_SIZE / stored_len);
	unsigned first = total - lines;
	char buf[RAMLOG_LINE_MAX];
	assert(ramlog_copy_line(0, buf, sizeof buf) == line_len);
	char expect[RAMLOG_LINE_MAX];
	snprintf(expect, sizeof expect, "W%0*u", 63, first);
	assert(!strcmp(buf, expect));
	assert(ramlog_copy_line(lines - 1, buf, sizeof buf) == line_len);
	snprintf(expect, sizeof expect, "W%0*u", 63, total - 1);
	assert(!strcmp(buf, expect));
}

/* A line longer than the per-line cap is stored truncated; a line longer
 * than the whole buffer still leaves a valid single-line log. */
static void test_overflow(void)
{
	ramlog_reset();
	char big[RAMLOG_SIZE + 512];
	memset(big, 'G', sizeof big);
	ramlog_line(big);
	assert(ramlog_lines() == 1);
	char buf[RAMLOG_LINE_MAX];
	assert(ramlog_copy_line(0, buf, sizeof buf) == RAMLOG_LINE_MAX - 1);
	assert(buf[0] == 'G' && buf[RAMLOG_LINE_MAX - 2] == 'G');
}

/* Mixed lengths under churn: every retained line must be a whole line of
 * one of the two known shapes (a torn merge would show another length). */
static void test_eviction_keeps_whole_lines(void)
{
	const unsigned short_len = 10, long_len = 90;
	ramlog_reset();
	const unsigned total = 4000;
	char buf[RAMLOG_LINE_MAX];
	for (unsigned i = 0; i < total; i++)
		append_numbered((i & 1) ? "S" : "L", (i & 1) ? short_len - 1 : long_len - 1, i, 1);
	unsigned lines = ramlog_lines();
	assert(lines > 0 && lines < total);
	unsigned expect_last = total - 1;
	for (unsigned i = 0; i < lines; i++) {
		unsigned len = ramlog_copy_line(i, buf, sizeof buf);
		assert(len == short_len || len == long_len);
		if (i == lines - 1) {
			char expect[RAMLOG_LINE_MAX];
			snprintf(expect, sizeof expect, "%s%0*u",
				 (expect_last & 1) ? "S" : "L",
				 (int)((expect_last & 1) ? short_len - 1 : long_len - 1), expect_last);
			assert(!strcmp(buf, expect));
		}
	}
	/* The stored region is the newest `lines` lines, so the parity of the
	 * first retained line follows from the last appended one. */
	unsigned first_index = total - lines;
	unsigned len0 = ramlog_copy_line(0, buf, sizeof buf);
	assert(len0 == ((first_index & 1) ? short_len : long_len));
}

/* ramlog_total(): absolute line cursor that survives eviction. */
static void test_total_cursor(void)
{
	char buf[RAMLOG_LINE_MAX], line[40];
	ramlog_reset();
	assert(ramlog_total() == 0);
	for (unsigned i = 0; i < 5; i++) {
		snprintf(line, sizeof line, "line %u", i);
		ramlog_line(line);
	}
	assert(ramlog_total() == 5 && ramlog_lines() == 5);
	/* Overflow the buffer so old lines evict; total keeps counting and
	 * retained line k is absolute line (total - lines + k). */
	for (unsigned i = 5; i < 4000; i++) {
		snprintf(line, sizeof line, "line %u", i);
		ramlog_line(line);
	}
	unsigned total = ramlog_total(), lines = ramlog_lines();
	assert(total == 4000 && lines < total);
	for (unsigned k = 0; k < lines; k++) {
		snprintf(line, sizeof line, "line %u", total - lines + k);
		ramlog_copy_line(k, buf, sizeof buf);
		assert(!strcmp(buf, line));
	}
	ramlog_reset();
	assert(ramlog_total() == 0);
}

int main(void)
{
	test_page_count();
	test_copy_line();
	test_wraparound();
	test_overflow();
	test_eviction_keeps_whole_lines();
	test_total_cursor();
	printf("PASS ramlog host test: page count, copy, wraparound, overflow, whole-line eviction, total cursor\n");
	return 0;
}
