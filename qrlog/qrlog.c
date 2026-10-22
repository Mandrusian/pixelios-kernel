#include "qrlog.h"
#include "qrcodegen.h"

uint32_t qrlog_crc32(const void *p, unsigned len)
{
	const uint8_t *b = p;
	uint32_t c = 0xFFFFFFFFu;
	while (len--) {
		c ^= *b++;
		for (int k = 0; k < 8; k++)
			c = c >> 1 ^ (0xEDB88320u & -(c & 1));
	}
	return ~c;
}

unsigned qrlog_nframes(unsigned len, unsigned chunk)
{
	return len ? (len + chunk - 1) / chunk : 1;
}

static char *put_hex(char *o, uint32_t v)
{
	for (int s = 28; s >= 0; s -= 4)
		*o++ = "0123456789abcdef"[v >> s & 15];
	return o;
}

static char *put_dec(char *o, unsigned v)
{
	char t[10];
	int n = 0;
	do
		t[n++] = '0' + v % 10;
	while (v /= 10);
	while (n)
		*o++ = t[--n];
	return o;
}

unsigned qrlog_make_frame(const char *text, unsigned len, unsigned chunk, unsigned idx, char *out)
{
	unsigned start = idx * chunk > len ? len : idx * chunk;
	unsigned n = len - start < chunk ? len - start : chunk;
	char *o = out;
	*o++ = 'Q'; *o++ = 'L'; *o++ = '1'; *o++ = ' ';
	o = put_hex(o, qrlog_crc32(text, len));
	*o++ = ' ';
	o = put_dec(o, idx);
	*o++ = '/';
	o = put_dec(o, qrlog_nframes(len, chunk));
	*o++ = ' ';
	/* crc over the sanitised bytes: sanitise first into the tail, then hash it */
	char *b = o + 9;
	for (unsigned i = 0; i < n; i++) {
		unsigned char c = text[start + i];
		b[i] = c == '\n' ? '\n' : c < 0x20 ? '.' : c >= 0x7f ? '?' : c;
	}
	o = put_hex(o, qrlog_crc32(b, n));
	*o++ = '\n';
	o += n;
	*o = 0;
	return o - out;
}

static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QRLOG_VER_MAX)];
static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QRLOG_VER_MAX)];

unsigned qrlog_draw(volatile uint32_t *fb, unsigned w, unsigned h, unsigned stride, const char *frame)
{
	if (!qrcodegen_encodeText(frame, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, QRLOG_VER_MAX,
				  qrcodegen_Mask_AUTO, 1))
		return 0;
	unsigned size = qrcodegen_getSize(qr);
	unsigned avail = (w < h ? w : h) - 2 * QRLOG_MARGIN;	/* keep clear of rounded corners/bezel */
	unsigned scale = avail / (size + 8);		/* 4-module quiet zone each side */
	if (!scale)
		return 0;
	unsigned side = (size + 8) * scale;
	unsigned x0 = (w - side) / 2, y0 = (h - side) / 2;
	unsigned spx = stride / 4;
	static uint32_t line[2560];		/* one pixel row, replicated per module row */
	if (w > sizeof line / sizeof line[0])
		return 0;
	for (unsigned y = 0; y < h; y++) {
		if (y < y0 + 4 * scale || y >= y0 + side - 4 * scale || (y - y0) % scale == 0 || y == 0) {
			int blank = y < y0 + 4 * scale || y >= y0 + side - 4 * scale;
			for (unsigned x = 0; x < w; x++) {
				uint32_t v = 0xffffffffu;
				if (!blank && x >= x0 + 4 * scale && x < x0 + side - 4 * scale &&
				    qrcodegen_getModule(qr, (x - x0) / scale - 4, (y - y0) / scale - 4))
					v = 0xff000000u;
				line[x] = v;
			}
		}
		for (unsigned x = 0; x < w; x++)
			fb[y * spx + x] = line[x];
	}
	return size;
}
