#include "qrlog.h"
#include "qrstream.h"
#include "qrz.h"
#include "qrcodegen.h"

#define NONE16	0xFFFFu

/* ---- small helpers ---- */

static unsigned put_varint(uint8_t *o, uint64_t v)
{
	unsigned n = 0;
	while (v >= 128) {
		o[n++] = (uint8_t)(v | 128);
		v >>= 7;
	}
	o[n++] = (uint8_t)v;
	return n;
}

static const char b45[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

static unsigned base45(const uint8_t *in, unsigned n, char *out)
{
	unsigned o = 0;
	for (unsigned i = 0; i + 1 < n; i += 2) {
		unsigned v = in[i] * 256u + in[i + 1];
		out[o++] = b45[v % 45];
		out[o++] = b45[v / 45 % 45];
		out[o++] = b45[v / 2025];
	}
	if (n & 1) {
		unsigned v = in[n - 1];
		out[o++] = b45[v % 45];
		out[o++] = b45[v / 45];
	}
	out[o] = 0;
	return o;
}

/* ---- QR geometry ---- */

static uint8_t qr_tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QS_VER_MAX)];
static uint8_t qr_img[QS_TILES_MAX][qrcodegen_BUFFER_LEN_FOR_VERSION(QS_VER_MAX)];
static char probe[QS_TEXT_MAX + 1];

static int fits(const qs_t *q, unsigned chars)
{
	for (unsigned i = 0; i < chars; i++)
		probe[i] = 'A';
	probe[chars] = 0;
	return qrcodegen_encodeText(probe, qr_tmp, qr_img[0], (enum qrcodegen_Ecc)q->ecc, (int)q->ver, (int)q->ver,
				    qrcodegen_Mask_AUTO, 0);
}

unsigned qs_sizeof(void)
{
	return sizeof(qs_t);
}

void qs_set_fec(qs_t *q, unsigned on)
{
	q->nofec = !on;
}

void qs_set_red(qs_t *q, unsigned percent)
{
	q->repair_pct = percent > 100 ? 100 : percent;
}

unsigned qs_init(qs_t *q, unsigned ver, unsigned ecc, unsigned tiles, unsigned unit, unsigned klog, unsigned sid)
{
	if (ver == 21)		/* OpenCV's QR decoder (the host side) fails on version 21 even on a clean image (tools/qrsim.py): use 22 */
		ver = 22;
	if (ver < 1 || ver > QS_VER_MAX || ecc > 3 || !tiles || tiles > QS_TILES_MAX || !unit || unit > QS_UNIT_MAX ||
	    klog < 2 || klog > 9)
		return 0;
	uint8_t *p = (uint8_t *)q;
	for (unsigned i = 0; i < sizeof *q; i++)
		p[i] = 0;
	q->ver = ver; q->ecc = ecc; q->tiles = tiles; q->unit = unit; q->klog = klog; q->sid = sid & 0xffff;
	unsigned lo = 1, hi = QS_TEXT_MAX;
	if (!fits(q, lo))
		return 0;
	while (lo < hi) {		/* largest alphanumeric text that fits this version/ecc */
		unsigned mid = (lo + hi + 1) / 2;
		if (fits(q, mid))
			lo = mid;
		else
			hi = mid - 1;
	}
	q->cap_chars = lo;
	unsigned n = lo / 3 * 2 + (lo % 3 >= 2);		/* body bytes whose base45 fits */
	if (n < QS_HDR_MAX + 4 + 2 + 32 || n - QS_HDR_MAX - 4 > QS_B_MAX)
		return 0;
	q->B = n - QS_HDR_MAX - 4;
	q->D = q->B - 2;
	q->bfirst[0] = NONE16;
	return q->B;
}

/* ---- transport stream ---- */

static void tx_put(qs_t *q, uint8_t c)
{
	q->tx[q->tx_len % QS_TX_SIZE] = c;
	q->tx_len++;
	if (q->tx_len % q->D == 0)
		q->bfirst[(q->tx_len / q->D) % QS_MAXBLK] = NONE16;
}

static void tx_varint(qs_t *q, uint64_t v)
{
	uint8_t t[10];
	unsigned n = put_varint(t, v);
	for (unsigned i = 0; i < n; i++)
		tx_put(q, t[i]);
}

uint64_t qs_hi(const qs_t *q)
{
	return q->tx_len / q->D;
}

uint64_t qs_lo(const qs_t *q)
{
	uint64_t K = 1u << q->klog, lo = 0;
	if (q->tx_len > QS_TX_SIZE)
		lo = (q->tx_len - QS_TX_SIZE + q->D - 1) / q->D;
	if (qs_hi(q) > QS_MAXBLK && lo < qs_hi(q) - QS_MAXBLK)
		lo = qs_hi(q) - QS_MAXBLK;
	return (lo + K - 1) / K * K;
}

uint64_t qs_reduced(const qs_t *q)
{
	return q->red_base;
}

uint64_t qs_stream_len(const qs_t *q)
{
	return q->tx_len;
}

static uint8_t cbuf[QS_UNIT_MAX + 64];

/* compress red[0, commit) into one record and shift the rest down */
static void compress_unit(qs_t *q, unsigned commit)
{
	if (!commit)
		return;
	unsigned clen = qrz_deflate(q->red, commit, cbuf, commit - 1 < sizeof cbuf ? commit - 1 : sizeof cbuf);
	const uint8_t *body = clen ? cbuf : q->red;
	if (!clen)
		clen = commit;
	unsigned blk = q->tx_len / q->D;
	if (q->bfirst[blk % QS_MAXBLK] == NONE16)
		q->bfirst[blk % QS_MAXBLK] = (uint16_t)(q->tx_len % q->D);
	tx_varint(q, (uint64_t)commit << 1 | (body == q->red));
	tx_varint(q, clen);
	tx_varint(q, q->red_base);
	tx_varint(q, q->red0_raw);
	uint32_t crc = qrlog_crc32(q->red, commit);
	for (int i = 0; i < 4; i++)
		tx_put(q, crc >> (8 * i) & 255);
	for (unsigned i = 0; i < clen; i++)
		tx_put(q, body[i]);
	q->red_base += commit;
	for (unsigned i = commit; i < q->red_len; i++)
		q->red[i - commit] = q->red[i];
	q->red_len -= commit;
	q->lstart -= q->in_line ? commit : 0;
	if (!q->in_line)
		q->lstart = q->red_len;
	else
		q->red0_raw = q->line_raw0;
}

int qs_pending(const qs_t *q)
{
	return q->red_len || q->tx_len % q->D;
}

int qs_flush(qs_t *q)
{
	int did = q->red_len || q->tx_len % q->D;
	compress_unit(q, q->in_line ? q->lstart : q->red_len);
	while (q->tx_len % q->D)
		tx_put(q, 0);
	return did;
}

/* ---- reducer ---- */

static int has(const uint8_t *s, unsigned n, const char *w)
{
	unsigned m = 0;
	while (w[m])
		m++;
	for (unsigned i = 0; i + m <= n; i++) {
		unsigned k = 0;
		while (k < m && s[i + k] == (uint8_t)w[k])
			k++;
		if (k == m)
			return 1;
	}
	return 0;
}

/* lines that must survive shape-dedupe (only exact repeats are dropped): panics, crashes, our own SWEET* diagnostics */
static int is_protected(const uint8_t *s, unsigned n)
{
	static const char *const kw[] = { "SWEET", "panic", "PANIC", "Panic", "CRASH", "Assertion", "esr", "Kernel trap" };
	for (unsigned i = 0; i < sizeof kw / sizeof kw[0]; i++)
		if (i ? has(s, n, kw[i]) : (n >= 5 && has(s, 5, kw[0])))
			return 1;
	return 0;
}

static void red_put(qs_t *q, uint8_t c)
{
	q->red[q->red_len++] = c;
}

static unsigned put_dec_r(uint8_t *o, unsigned v)
{
	uint8_t t[10];
	unsigned n = 0, k = 0;
	do
		t[n++] = '0' + v % 10;
	while (v /= 10);
	while (n)
		o[k++] = t[--n];
	return k;
}

static void line_done(qs_t *q)
{
	unsigned len = q->red_len - q->lstart;
	const uint8_t *ln = q->red + q->lstart;
	q->in_line = 0;
	if (len <= 1) {			/* empty line: no information */
		q->red_len = q->lstart;
		return;
	}
	int prot = is_protected(ln, len), dup = 0;
	for (unsigned i = 0; i < 32 && i < q->nrec; i++)
		dup |= prot ? q->recF[i] == q->hf : q->recS[i] == q->hs;
	if (dup) {			/* roll back: the line never reached the stream */
		q->red_len = q->lstart;
		q->ndrop++;
		return;
	}
	if (q->ndrop) {			/* say how much was dropped, in front of the line that ends the run */
		uint8_t m[48];
		unsigned ml = 0;
		const char *a = "[~", *b = " similar lines dropped]\n";
		while (*a)
			m[ml++] = *a++;
		ml += put_dec_r(m + ml, q->ndrop);
		while (*b)
			m[ml++] = *b++;
		for (unsigned i = len; i-- > 0;)
			q->red[q->lstart + ml + i] = q->red[q->lstart + i];
		for (unsigned i = 0; i < ml; i++)
			q->red[q->lstart + i] = m[i];
		q->red_len += ml;
		q->ndrop = 0;
	}
	q->recS[q->nrec % 32] = q->hs;
	q->recF[q->nrec % 32] = q->hf;
	q->nrec++;
	q->lstart = q->red_len;
	if (q->red_len >= q->unit)
		compress_unit(q, q->red_len);
}

void qs_feed(qs_t *q, uint64_t raw_off, const char *p, unsigned n)
{
	if (!q->raw_next && !q->red_len && !q->tx_len && !q->in_line)
		q->raw_next = raw_off;
	if (raw_off != q->raw_next && n) {		/* the console ring overran the reader */
		uint64_t lost = raw_off - q->raw_next;
		if (q->in_line) {
			red_put(q, '\n');
			line_done(q);
		}
		uint8_t m[48];
		unsigned ml = 0;
		const char *a = "[log overrun: ", *b = " bytes lost]\n";
		while (*a)
			m[ml++] = *a++;
		ml += put_dec_r(m + ml, (unsigned)(lost > 0xffffffffu ? 0xffffffffu : lost));
		while (*b)
			m[ml++] = *b++;
		if (!q->red_len)
			q->red0_raw = q->raw_next;
		for (unsigned i = 0; i < ml; i++)
			red_put(q, m[i]);
		q->lstart = q->red_len;
		q->raw_next = raw_off;
		if (q->red_len >= q->unit)
			compress_unit(q, q->red_len);
	}
	for (unsigned i = 0; i < n; i++, q->raw_next++) {
		uint8_t c = (uint8_t)p[i];
		if (c == '\r')
			continue;
		if (!q->in_line) {
			q->in_line = 1;
			q->lstart = q->red_len;
			q->line_raw0 = q->raw_next;
			if (!q->red_len)
				q->red0_raw = q->raw_next;
			q->hs = q->hf = 2166136261u;
		}
		if (c == 0)
			c = '.';
		red_put(q, c);
		q->hf = (q->hf ^ c) * 16777619u;
		if (c < '0' || c > '9')
			q->hs = (q->hs ^ c) * 16777619u;
		if (c == '\n')
			line_done(q);
		else if (q->red_len - q->lstart >= QS_LINE_MAX) {
			red_put(q, '\n');
			line_done(q);
		}
	}
}

/* ---- frames ---- */

static uint8_t blkbuf[QS_B_MAX], acc[QS_B_MAX], body[QS_HDR_MAX + QS_B_MAX + 4];

static void get_block(const qs_t *q, uint64_t k, uint8_t *out)
{
	uint16_t f = q->bfirst[k % QS_MAXBLK];
	out[0] = f & 255;
	out[1] = f >> 8;
	uint64_t at = k * q->D;
	for (unsigned i = 0; i < q->D; i++)
		out[2 + i] = q->tx[(at + i) % QS_TX_SIZE];
}

/* which blocks of generation g a repair symbol covers: block i is in iff a murmur3-finalised hash of (g, seed, i) has bit 16 set.
 * Same on the host (tools/qrl2.py rep_mask). Must be nonlinear: a GF(2)-linear generator (xorshift) spans <= 32 dimensions, which cannot
 * rebuild a generation of more than 32 blocks. */
static uint32_t mix32(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x85EBCA6Bu;
	h ^= h >> 13;
	h *= 0xC2B2AE35u;
	h ^= h >> 16;
	return h;
}

static uint32_t rep_seed(uint64_t g, unsigned seed)
{
	return mix32((uint32_t)g * 0x9E3779B1u ^ seed * 0x85EBCA77u ^ 0xC2B2AE3Du);
}

static int rep_bit(uint32_t base, unsigned i)
{
	return mix32(base + i * 0x9E3779B9u) >> 16 & 1;
}

static unsigned frame_text(qs_t *q, int rep, uint64_t a, unsigned seed, char *text)
{
	unsigned hl = 0, K = 1u << q->klog;
	uint64_t hi = qs_hi(q);
	body[hl++] = 0x20 | rep << 3 | (q->klog - 2);
	body[hl++] = q->sid & 255;
	body[hl++] = q->sid >> 8;
	hl += put_varint(body + hl, hi);
	hl += put_varint(body + hl, qs_lo(q));
	hl += put_varint(body + hl, a);
	if (rep)
		hl += put_varint(body + hl, seed);
	if (!rep) {
		get_block(q, a, body + hl);
	} else {
		uint32_t x = rep_seed(a, seed);
		unsigned any = 0, kk = (a + 1) * K > hi ? (unsigned)(hi - a * K) : K;	/* the open generation: only the blocks it has so far */
		for (unsigned i = 0; i < q->B; i++)
			acc[i] = 0;
		for (unsigned i = 0; i < kk; i++) {
			if (!rep_bit(x, i))
				continue;
			any = 1;
			get_block(q, a * K + i, blkbuf);
			for (unsigned j = 0; j < q->B; j++)
				acc[j] ^= blkbuf[j];
		}
		if (!any) {
			get_block(q, a * K + seed % kk, acc);
		}
		for (unsigned i = 0; i < q->B; i++)
			body[hl + i] = acc[i];
	}
	unsigned bl = hl + q->B;
	uint32_t crc = qrlog_crc32(body, bl);
	for (int i = 0; i < 4; i++)
		body[bl++] = crc >> (8 * i) & 255;
	return base45(body, bl, text);
}

unsigned qs_next_frame(qs_t *q, char *text)
{
	uint64_t hi = qs_hi(q), lo = qs_lo(q), K = 1u << q->klog, R = q->nofec ? 0 : (K * q->repair_pct + 99) / 100;
	if (hi <= lo)
		return 0;
	q->tick++;
	if (hi != q->hi_seen) {
		q->hi_seen = hi;
		q->grow_tick = q->tick;
	}
	/* 2 slots of 3: the first pass, generation after generation: K plain blocks, then R repair frames */
	if (q->next_new < lo / K * (K + R))
		q->next_new = lo / K * (K + R);
	if (q->tick % 3 != 0) {
		uint64_t g = q->next_new / (K + R), pos = q->next_new % (K + R);
		if (pos < K && g * K + pos < hi) {
			q->next_new++;
			return frame_text(q, 0, g * K + pos, 0, text);
		}
		if (pos >= K && (g + 1) * K <= hi) {
			q->next_new++;
			return frame_text(q, 1, g, q->seed++ & 0xffff, text);
		}
	}
	/* the rest: repair frames rotating over all generations, the open one (blocks so far) included */
	uint64_t g0 = lo / K, g1 = (hi + K - 1) / K, bg = q->bg++;
	if (q->nofec)
		return frame_text(q, 0, lo + bg % (hi - lo), 0, text);
	int growing = q->tick - q->grow_tick < 64;
	uint64_t g = growing && (bg & 1) ? g1 - 1 : g0 + (growing ? bg >> 1 : bg) % (g1 - g0);
	return frame_text(q, 1, g, q->seed++ & 0xffff, text);
}

/* ---- drawing ---- */

unsigned qs_draw(const qs_t *q, volatile uint32_t *fb, unsigned w, unsigned h, unsigned stride, const char *const *texts,
		 unsigned nt)
{
	if (!nt || nt > QS_TILES_MAX)
		return 0;
	unsigned size = 0;
	for (unsigned t = 0; t < nt; t++) {
		if (!qrcodegen_encodeText(texts[t], qr_tmp, qr_img[t], (enum qrcodegen_Ecc)q->ecc, (int)q->ver, (int)q->ver,
					  qrcodegen_Mask_AUTO, 0))
			return 0;
		size = qrcodegen_getSize(qr_img[t]);
	}
	unsigned aw = w - 2 * QRLOG_MARGIN, ah = (h - 2 * QRLOG_MARGIN) / nt;
	unsigned scale = (aw < ah ? aw : ah) / (size + 8);
	if (!scale)
		return 0;
	unsigned side = (size + 8) * scale, spx = stride / 4;
	unsigned x0 = (w - side) / 2, y0 = (h - side * nt) / 2;
	static uint32_t line[2560];
	if (w > sizeof line / sizeof line[0])
		return 0;
	for (unsigned x = 0; x < w; x++)
		line[x] = 0xffffffffu;
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++)
			fb[y * spx + x] = line[x];
	for (unsigned t = 0; t < nt; t++) {
		for (unsigned my = 0; my < size; my++) {
			for (unsigned x = 0; x < w; x++)
				line[x] = 0xffffffffu;
			for (unsigned mx = 0; mx < size; mx++)
				if (qrcodegen_getModule(qr_img[t], mx, my))
					for (unsigned k = 0; k < scale; k++)
						line[x0 + (mx + 4) * scale + k] = 0xff000000u;
			for (unsigned k = 0; k < scale; k++) {
				unsigned y = y0 + t * side + (my + 4) * scale + k;
				for (unsigned x = x0 + 4 * scale; x < x0 + side - 4 * scale; x++)
					fb[y * spx + x] = line[x];
			}
		}
	}
	return size;
}
