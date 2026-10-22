#include "qrz.h"
#include "qrz_tables.h"

#define WIN_MAX		(QRZ_DICT_LEN + QRZ_IN_MAX)
#define HASH_BITS	12
#define MAX_CHAIN	24
#define NICE_LEN	96

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* canonical codes (RFC 1951), stored bit-reversed: deflate packs Huffman codes MSB first into an LSB-first stream */
static uint16_t lit_code[286], dist_code[30];
static int tables_ready;

static void make_codes(const uint8_t *lens, unsigned n, uint16_t *code)
{
	unsigned count[16] = { 0 }, next[16];
	unsigned c = 0;
	for (unsigned i = 0; i < n; i++)
		count[lens[i]]++;
	count[0] = 0;
	for (unsigned b = 1; b < 16; b++) {
		c = (c + count[b - 1]) << 1;
		next[b] = c;
	}
	for (unsigned i = 0; i < n; i++) {
		unsigned l = lens[i], v = lens[i] ? next[l]++ : 0, r = 0;
		for (unsigned k = 0; k < l; k++, v >>= 1)
			r = r << 1 | (v & 1);
		code[i] = r;
	}
}

struct bw {
	uint8_t *o, *end;
	uint32_t acc;
	unsigned nb;
	int over;
};

static void put(struct bw *w, uint32_t v, unsigned n)
{
	w->acc |= v << w->nb;
	w->nb += n;
	while (w->nb >= 8) {
		if (w->o < w->end)
			*w->o++ = w->acc & 255;
		else
			w->over = 1;
		w->acc >>= 8;
		w->nb -= 8;
	}
}

#ifdef QRZ_STATS
uint32_t qrz_hist_lit[286], qrz_hist_dist[30];
#define STAT(a, i) (a[i]++)
#else
#define STAT(a, i) ((void)0)
#endif

static void emit_lit(struct bw *w, unsigned c)
{
	STAT(qrz_hist_lit, c);
	put(w, lit_code[c], qrz_litlen_len[c]);
}

static void emit_match(struct bw *w, unsigned len, unsigned dist)
{
	unsigned s = 28, d = 29;
	while (len_base[s] > len)
		s--;
	while (dist_base[d] > dist)
		d--;
	STAT(qrz_hist_lit, 257 + s);
	STAT(qrz_hist_dist, d);
	put(w, lit_code[257 + s], qrz_litlen_len[257 + s]);
	put(w, len - len_base[s], len_extra[s]);
	put(w, dist_code[d], qrz_dist_len[d]);
	put(w, dist - dist_base[d], dist_extra[d]);
}

static uint8_t win[WIN_MAX];
static uint16_t head[1u << HASH_BITS], prv[WIN_MAX];	/* position + 1, 0 = none */

static unsigned hash3(const uint8_t *p)
{
	return ((p[0] << 8 ^ p[1] << 4 ^ p[2]) * 2654435761u) >> (32 - HASH_BITS);
}

static void insert(unsigned p, unsigned end)
{
	if (p + 3 > end)
		return;
	unsigned h = hash3(win + p);
	prv[p] = head[h];
	head[h] = p + 1;
}

/* longest match at p (window end = end); returns the length (0 = none) and sets *dist */
static unsigned find(unsigned p, unsigned end, unsigned *dist)
{
	if (p + 3 > end)
		return 0;
	unsigned best = 0, maxl = end - p < 258 ? end - p : 258;
	unsigned c = head[hash3(win + p)];
	for (unsigned chain = 0; c && chain < MAX_CHAIN; chain++, c = prv[c - 1]) {
		unsigned q = c - 1;
		if (p - q > 32768)
			break;
		unsigned l = 0;
		while (l < maxl && win[q + l] == win[p + l])
			l++;
		if (l > best && !(l == 3 && p - q > 4096)) {
			best = l;
			*dist = p - q;
			if (l >= NICE_LEN || l == maxl)
				break;
		}
	}
	return best >= 3 ? best : 0;
}

unsigned qrz_deflate(const uint8_t *src, unsigned n, uint8_t *dst, unsigned dmax)
{
	if (n > QRZ_IN_MAX)
		return 0;
	if (!tables_ready) {
		make_codes(qrz_litlen_len, 286, lit_code);
		make_codes(qrz_dist_len, 30, dist_code);
		tables_ready = 1;
	}
	for (unsigned i = 0; i < QRZ_DICT_LEN; i++)
		win[i] = qrz_dict[i];
	for (unsigned i = 0; i < n; i++)
		win[QRZ_DICT_LEN + i] = src[i];
	for (unsigned i = 0; i < (1u << HASH_BITS); i++)
		head[i] = 0;
	unsigned end = QRZ_DICT_LEN + n;
	for (unsigned i = 0; i < QRZ_DICT_LEN; i++)
		insert(i, end);
	struct bw w = { dst, dst + dmax, 0, 0, 0 };
	unsigned p = QRZ_DICT_LEN;
	while (p < end) {
		unsigned d = 0, l = find(p, end, &d);
		if (l && l < NICE_LEN && p + 1 < end) {	/* lazy: a clearly longer match one byte later wins */
			unsigned d2 = 0;
			insert(p, end);
			if (find(p + 1, end, &d2) > l + 1) {
				emit_lit(&w, win[p]);
				p++;
				continue;
			}
			emit_match(&w, l, d);
			for (unsigned k = 1; k < l; k++)
				insert(p + k, end);
			p += l;
		} else if (l) {
			emit_match(&w, l, d);
			for (unsigned k = 0; k < l; k++)
				insert(p + k, end);
			p += l;
		} else {
			insert(p, end);
			emit_lit(&w, win[p]);
			p++;
		}
	}
	put(&w, lit_code[256], qrz_litlen_len[256]);
	if (w.nb)
		put(&w, 0, 8 - w.nb);
	return w.over ? 0 : (unsigned)(w.o - dst);
}
