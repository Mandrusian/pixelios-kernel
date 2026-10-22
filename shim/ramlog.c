#include "ramlog.h"

/*
 * Circular buffer of complete lines. head is the oldest stored byte,
 * used the byte count; bytes (head + i) modulo RAMLOG_SIZE. Every write
 * appends one line ending in '\n', and eviction advances head past a
 * whole line, so the invariant "used bytes are whole lines" always holds.
 */

static char ramlog_buf[RAMLOG_SIZE];
static unsigned ramlog_head;
static unsigned ramlog_used;
static unsigned ramlog_appended;	/* lines ever appended: stable cursor across eviction */

static unsigned idx(unsigned i)
{
	i += ramlog_head;
	while (i >= RAMLOG_SIZE)
		i -= RAMLOG_SIZE;
	return i;
}

void ramlog_reset(void)
{
	ramlog_head = 0;
	ramlog_used = 0;
	ramlog_appended = 0;
}

/* Advance head past the oldest stored line (or drop everything if the
 * stored region somehow holds no newline). */
static void drop_oldest_line(void)
{
	for (unsigned k = 0; k < ramlog_used; k++) {
		if (ramlog_buf[idx(k)] == '\n') {
			ramlog_head = idx(k + 1);
			ramlog_used -= k + 1;
			return;
		}
	}
	ramlog_head = 0;
	ramlog_used = 0;
}

void ramlog_line(const char *line)
{
	unsigned len = 0;
	while (line[len] && len < RAMLOG_LINE_MAX - 1)
		len++;
	while (ramlog_used + len + 1 > RAMLOG_SIZE)
		drop_oldest_line();
	unsigned pos = ramlog_used;
	for (unsigned i = 0; i < len; i++)
		ramlog_buf[idx(pos + i)] = line[i];
	ramlog_buf[idx(pos + len)] = '\n';
	ramlog_used += len + 1;
	ramlog_appended++;
}

unsigned ramlog_total(void)
{
	return ramlog_appended;
}

unsigned ramlog_lines(void)
{
	unsigned count = 0;
	for (unsigned k = 0; k < ramlog_used; k++)
		if (ramlog_buf[idx(k)] == '\n')
			count++;
	return count;
}

unsigned ramlog_copy_line(unsigned index, char *dst, unsigned cap)
{
	if (!cap)
		return 0;
	unsigned seen = 0, out = 0;
	for (unsigned k = 0; k < ramlog_used; k++) {
		char c = ramlog_buf[idx(k)];
		if (c == '\n') {
			if (seen == index) {
				dst[out] = 0;
				return out;
			}
			seen++;
			continue;
		}
		if (seen == index && out + 1 < cap)
			dst[out++] = c;
	}
	dst[0] = 0;
	return 0;
}

unsigned ramlog_page_count(unsigned rows_per_page)
{
	if (!rows_per_page)
		return 0;
	unsigned lines = ramlog_lines();
	return (lines + rows_per_page - 1) / rows_per_page;
}
