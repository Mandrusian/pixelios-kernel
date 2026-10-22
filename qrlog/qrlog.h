#pragma once
#include <stdint.h>

/*
 * QR log channel: the text log is cut into chunks, each shown as one QR code.
 * Frame text (ASCII only, so any QR reader returns it unchanged):
 *   "QL1 <sid:8hex> <idx>/<n> <chunk crc32:8hex>\n" + chunk bytes
 * sid = crc32 of the whole text, so the host can tell loops of the same
 * snapshot from a new one. tools/qr-log.py reassembles and verifies.
 */
#define QRLOG_HDR_MAX	48u
#define QRLOG_VER_MAX	25
#define QRLOG_MARGIN	48u	/* px kept clear on every screen edge */

uint32_t qrlog_crc32(const void *p, unsigned len);
unsigned qrlog_nframes(unsigned len, unsigned chunk);

/* Build frame idx into out (>= chunk + QRLOG_HDR_MAX + 1 bytes, NUL-terminated).
 * Non-printable bytes in the chunk become '.', bytes >= 0x7f '?' (newline kept).
 * Returns the frame length. */
unsigned qrlog_make_frame(const char *text, unsigned len, unsigned chunk, unsigned idx, char *out);

/* Encode frame text and paint it black on white, centred, integer-scaled, on a
 * 32-bit framebuffer (stride in bytes). Returns the QR size in modules, 0 on failure. */
unsigned qrlog_draw(volatile uint32_t *fb, unsigned w, unsigned h, unsigned stride, const char *frame);
