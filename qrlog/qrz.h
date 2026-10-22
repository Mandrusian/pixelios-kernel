#pragma once
#include <stdint.h>

/*
 * qrz: tiny freestanding raw-DEFLATE compressor for the QL2 log channel.
 * LZ77 (hash chains, one-step lazy) over [preset dictionary + data] and a
 * fixed Huffman table (qrz_tables.h, trained by tools/qrz-train.py). The
 * block header is NOT sent: the host (tools/qrl2.py) prepends the same
 * dynamic-block header (built from the same table) and inflates with python's
 * zlib using the same dictionary. Output = the block body bits + end-of-block,
 * padded to a byte.
 */
#define QRZ_IN_MAX	12288u

/* Compress n (<= QRZ_IN_MAX) bytes. Returns the output size, or 0 when it does not fit in dmax. */
unsigned qrz_deflate(const uint8_t *src, unsigned n, uint8_t *dst, unsigned dmax);
