/* Host test: frames of a sample log -> PGM images + frame texts, decoded by tools/qr-log.py --selftest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qrlog.h"

#define W 1080
#define H 2400
static uint32_t fb[W * H];

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	FILE *f = fopen(argv[1], "rb");
	if (!f)
		return 2;
	static char text[40000];
	unsigned len = fread(text, 1, sizeof text, f);
	fclose(f);
	unsigned chunk = argc > 3 ? atoi(argv[3]) : 480;
	unsigned n = qrlog_nframes(len, chunk);
	static char frame[2048];
	for (unsigned i = 0; i < n; i++) {
		qrlog_make_frame(text, len, chunk, i, frame);
		unsigned sz = qrlog_draw((volatile uint32_t *)fb, W, H, W * 4, frame);
		if (!sz) {
			fprintf(stderr, "frame %u: draw failed\n", i);
			return 1;
		}
		char name[512];
		snprintf(name, sizeof name, "%s/f%03u.pgm", argv[2], i);
		FILE *o = fopen(name, "wb");
		fprintf(o, "P5\n%d %d\n255\n", W, H);
		for (unsigned p = 0; p < W * H; p++)
			fputc(fb[p] & 0xff, o);
		fclose(o);
		if (i == 0)
			fprintf(stderr, "frames=%u qr modules=%u\n", n, sz);
	}
	return 0;
}
