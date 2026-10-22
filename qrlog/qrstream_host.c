/* Host driver for the QL2 encoder (tools/test_qrl2.py, tools/qrsim.py): log on stdin -> frame texts on stdout, one per line.
 *   qrstream_host [--ver V] [--ecc 0-3] [--tiles T] [--unit U] [--klog L] [--sid S] [--frames N] [--grow BYTES]
 *                 [--flush-every N] [--pgm DIR --w W --h H]
 * --grow: feed that many log bytes before each frame (a log that keeps growing); the rest is fed in one go.
 * --flush-every N: qs_flush after every N frames while the log is still growing (the idle flush of the phone thread).
 * --pgm: also paint every T frames as one screen into DIR/sNNNN.pgm (8-bit gray). Stats go to stderr as key=value. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qrlog.h"
#include "qrstream.h"

static qs_t q;
static char log_[1 << 24];

int main(int argc, char **argv)
{
	unsigned ver = 10, ecc = 0, tiles = 1, unit = 8192, klog = 6, sid = 1, frames = 100, grow = 0, flush_every = 0, W = 1080, H = 2400;
	const char *pgm = NULL;
	for (int i = 1; i + 1 < argc; i += 2) {
		unsigned v = (unsigned)strtoul(argv[i + 1], NULL, 0);
		if (!strcmp(argv[i], "--ver")) ver = v;
		else if (!strcmp(argv[i], "--ecc")) ecc = v;
		else if (!strcmp(argv[i], "--tiles")) tiles = v;
		else if (!strcmp(argv[i], "--unit")) unit = v;
		else if (!strcmp(argv[i], "--klog")) klog = v;
		else if (!strcmp(argv[i], "--sid")) sid = v;
		else if (!strcmp(argv[i], "--frames")) frames = v;
		else if (!strcmp(argv[i], "--grow")) grow = v;
		else if (!strcmp(argv[i], "--flush-every")) flush_every = v;
		else if (!strcmp(argv[i], "--w")) W = v;
		else if (!strcmp(argv[i], "--h")) H = v;
		else if (!strcmp(argv[i], "--pgm")) pgm = argv[i + 1];
		else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
	}
	size_t len = fread(log_, 1, sizeof log_, stdin), fed = 0;
	unsigned B = qs_init(&q, ver, ecc, tiles, unit, klog, sid);
	if (!B) { fprintf(stderr, "qs_init failed\n"); return 1; }
	if (!grow) { qs_feed(&q, 0, log_, len); fed = len; qs_flush(&q); }
	static char texts[QS_TILES_MAX][QS_TEXT_MAX + 1];
	static uint32_t fb[2560 * 2600];
	const char *tp[QS_TILES_MAX];
	for (unsigned f = 0; f < frames; f++) {
		if (grow && fed < len) {
			size_t n = len - fed < grow ? len - fed : grow;
			qs_feed(&q, fed, log_ + fed, n);
			fed += n;
			if (fed == len || (flush_every && f % flush_every == flush_every - 1)) qs_flush(&q);
		}
		unsigned t = f % tiles;
		unsigned n = qs_next_frame(&q, texts[t]);
		if (!n) { printf("-\n"); continue; }
		puts(texts[t]);
		tp[t] = texts[t];
		if (pgm && t == tiles - 1) {
			unsigned sz = qs_draw(&q, fb, W, H, W * 4, tp, tiles);
			if (!sz) { fprintf(stderr, "draw failed\n"); return 1; }
			char name[512];
			snprintf(name, sizeof name, "%s/s%04u.pgm", pgm, f / tiles);
			FILE *o = fopen(name, "wb");
			fprintf(o, "P5\n%u %u\n255\n", W, H);
			for (unsigned y = 0; y < H; y++)
				for (unsigned x = 0; x < W; x++)
					fputc(fb[y * W + x] & 255, o);
			fclose(o);
		}
	}
	fprintf(stderr, "B=%u D=%u cap_chars=%u hi=%llu lo=%llu tx_len=%llu red_base=%llu\n", q.B, q.D, q.cap_chars,
		(unsigned long long)qs_hi(&q), (unsigned long long)qs_lo(&q), (unsigned long long)q.tx_len,
		(unsigned long long)q.red_base);
	return 0;
}
