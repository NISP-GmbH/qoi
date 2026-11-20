/*

Copyright (c) 2021, Dominic Szablewski - https://phoboslab.org
Copyright (c) 2026, NI SP GmbH (the streaming state machines)
SPDX-License-Identifier: MIT


qoi_stream.h - a resumable QOI encoder and decoder for live streams, beside qoi.h

-- About

qoi.h codes a whole picture per call. This header holds the same format with the coder's state (the index, the previous pixel,
the run, the position) in a struct of the caller's, so that

  - the encoder takes any number of rows per call and returns the bytes that are ready so far: the first bytes of a picture can
    leave before the picture is coded. A run still open at the end of a call waits for the next rows; every byte before it is
    final. The files are byte-identical to qoi.h's qoi_encode() of the same pixels (the same op decisions).

  - the decoder resumes on a growing, contiguous input (a file whose bytes arrive in parts, as network packets do): each call
    decodes every op that is complete in the bytes given so far and keeps the rest for the next call. Unlike qoi.h it is strict:
    a truncated file, a wrong end marker and bytes after the end marker are refused, never filled with the last pixel. It keeps
    qoi.h's index rule (the pixel of every op, RUN and INDEX included, is stored in the index), so it decodes every file exactly
    as qoi.h does, edgecase.qoi of qoiformat.org's test images included. A refused file leaves the pixels decoded before the
    error in place: a damaged file's first rows can still be used, and qois_dec_pixels() says how many are exact.

  - Neither allocates, and neither calls the C library: the header includes only <stddef.h> and <stdint.h> (freestanding
    headers). Every buffer is the caller's. (A compiler may still emit memset / memcpy for a loop of its own choosing; built for
    WebAssembly with -mbulk-memory it imports nothing.)

qoi.h itself is not changed; this header does not include it, and both can be used in one program.

-- Synopsis

// Define `QOI_STREAM_IMPLEMENTATION` in *one* C/C++ file before including this header to create the implementation.

#define QOI_STREAM_IMPLEMENTATION
#include "qoi_stream.h"

// Encode: rows in, bytes out.
qois_desc desc = { width, height, 4, QOI_STREAM_SRGB };
size_t cap = qois_max_size(&desc);            // 0: not a valid picture (qoi.h's limits)
qois_enc e;
if (!qois_enc_begin(&e, &desc, out, cap)) ...  // writes the 14-byte header
for (each band of rows) {
	size_t ready = qois_enc_rows(&e, rows, stride, nrows);   // out[0 .. ready) is final; 0 on an error
	send(out + sent, ready - sent); sent = ready;
}
// qois_enc_done(&e) is 1 after the last row: out[0 .. ready) is the whole file, end marker included.

// Decode: bytes in, pixels out.
qois_dec d;
qois_dec_begin(&d, dst, dst_cap, dst_stride, 4);  // dst_stride 0: width * channels; channels 0: the file's
while (more bytes arrive) {
	int r = qois_dec_feed(&d, file, bytes_so_far, last_part);
	// QOIS_MORE: more input needed; QOIS_DONE: every pixel and the end marker; QOIS_ERROR: refused
	// qois_dec_rows(&d): the rows complete so far (exact even after an error)
}

-- Limits

As qoi.h: width and height at least 1, height < 400 000 000 / width (QOI_STREAM_PIXELS_MAX), 3 or 4 channels, colour space 0
or 1. The decoder refuses a header outside these and a destination smaller than the picture.

*/

#ifndef QOI_STREAM_H
#define QOI_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QOI_STREAM_SRGB   0
#define QOI_STREAM_LINEAR 1

/* qoi.h's QOI_PIXELS_MAX */
#ifndef QOI_STREAM_PIXELS_MAX
	#define QOI_STREAM_PIXELS_MAX ((unsigned int)400000000)
#endif

#define QOI_STREAM_HEADER_SIZE 14
#define QOI_STREAM_PADDING_SIZE 8

typedef struct {
	unsigned int width;
	unsigned int height;
	unsigned char channels;
	unsigned char colorspace;
} qois_desc;

typedef union {
	struct { unsigned char r, g, b, a; } rgba;
	uint32_t v;
} qois_rgba_t;

/* The encoder's state: the caller's, opaque. */
typedef struct {
	qois_rgba_t index[64];
	qois_rgba_t prev;
	uint64_t left;          /* pixels not yet coded */
	size_t p, cap;          /* bytes written, the buffer's size */
	unsigned char* out;
	unsigned int width;
	int run, channels, state;   /* state: 0 not begun, 1 coding, 2 done, -1 failed */
} qois_enc;

/* The decoder's state: the caller's, opaque. */
typedef struct {
	qois_rgba_t index[64];
	qois_rgba_t px;
	qois_desc desc;
	uint64_t left, done;    /* pixels not yet written, pixels written */
	size_t p;               /* the input consumed so far (whole ops) */
	size_t cap, stride, col;
	unsigned char* dst;
	unsigned char* row;     /* the row being written */
	int channels;           /* the destination's, 0 until the header (asked 0: the file's) */
	int asked, hash, state; /* state: 0 header pending, 1 ops, 2 end marker pending, 3 done, -1 refused */
} qois_dec;

enum { QOIS_ERROR = -1, QOIS_MORE = 0, QOIS_DONE = 1 };

/* The largest file a picture of this size can make (qoi.h's max_size); 0 where desc is not a valid picture. */
size_t qois_max_size(const qois_desc* desc);

/* Starts a file into out[0 .. cap) (cap >= qois_max_size(desc)) and writes its header. 1, or 0 for a picture qoi.h would refuse
   or a buffer too small. */
int qois_enc_begin(qois_enc* e, const qois_desc* desc, void* out, size_t cap);

/* Codes the next n pixels (desc->channels bytes each, contiguous). Returns the bytes of the file that are final so far (header
   included; after the picture's last pixel the whole file with its end marker), 0 on an error (not begun, more pixels than the
   picture has). */
size_t qois_enc_pixels(qois_enc* e, const void* pixels, size_t n);

/* Codes the next nrows rows of the picture, each `stride` bytes after the one before (stride 0: width * channels). Returns as
   qois_enc_pixels. */
size_t qois_enc_rows(qois_enc* e, const void* rows, size_t stride, unsigned int nrows);

/* 1 once every pixel was coded (the file is complete), 0 before, -1 after an error. */
int qois_enc_done(const qois_enc* e);

/* Starts decoding into dst (dst_cap bytes). dst_stride: the bytes from one row's start to the next (0: width * channels);
   channels: 3 or 4 for the destination's, 0 for the file's. The picture's size comes with the header (qois_dec_desc). */
void qois_dec_begin(qois_dec* d, void* dst, size_t dst_cap, size_t dst_stride, int channels);

/* The file's first `avail` bytes (the same buffer on every call, grown; never fewer bytes than the call before). final: no more
   bytes will come. Decodes every complete op not decoded yet.
   QOIS_MORE: more input needed (never with final);
   QOIS_DONE: every pixel and the end marker are in, and with final nothing follows the end marker;
   QOIS_ERROR: refused (not a QOI header, a picture qoi.h would refuse, a destination too small, a wrong end marker, bytes after
   it with final, a truncated file with final); the pixels decoded before the error stay. */
int qois_dec_feed(qois_dec* d, const void* data, size_t avail, int final);

/* 1 once the header was read (desc filled), 0 before. */
int qois_dec_desc(const qois_dec* d, qois_desc* desc);

/* The pixels written so far, in raster order; each one is final and exact. */
uint64_t qois_dec_pixels(const qois_dec* d);

/* The rows complete so far. */
unsigned int qois_dec_rows(const qois_dec* d);

#ifdef __cplusplus
}
#endif
#endif /* QOI_STREAM_H */


/* -----------------------------------------------------------------------------
Implementation */

#ifdef QOI_STREAM_IMPLEMENTATION

#define QOIS_OP_INDEX  0x00 /* 00xxxxxx */
#define QOIS_OP_DIFF   0x40 /* 01xxxxxx */
#define QOIS_OP_LUMA   0x80 /* 10xxxxxx */
#define QOIS_OP_RUN    0xc0 /* 11xxxxxx */
#define QOIS_OP_RGB    0xfe /* 11111110 */
#define QOIS_OP_RGBA   0xff /* 11111111 */
#define QOIS_MASK_2    0xc0 /* 11000000 */

#define QOIS_HASH(C) ((C.rgba.r*3 + C.rgba.g*5 + C.rgba.b*7 + C.rgba.a*11) & 63)

/* The decoder keeps the hash of its pixel as the ops change it (qoixx's idea): QOI's hash is linear modulo 64 and 256 is 0 modulo
   64, so a DIFF or LUMA op adds a constant that depends only on its bytes. */
static const unsigned char qois_dh_diff[64] = {
	34,41,48,55,39,46,53,60,44,51,58,1,49,56,63,6,37,44,51,58,42,49,56,63,47,54,61,4,52,59,2,9,
	40,47,54,61,45,52,59,2,50,57,0,7,55,62,5,12,43,50,57,0,48,55,62,5,53,60,3,10,58,1,8,15};
static const unsigned char qois_dh_luma1[64] = {
	16,31,46,61,12,27,42,57,8,23,38,53,4,19,34,49,0,15,30,45,60,11,26,41,56,7,22,37,52,3,18,33,
	48,63,14,29,44,59,10,25,40,55,6,21,36,51,2,17,32,47,62,13,28,43,58,9,24,39,54,5,20,35,50,1};
static const unsigned char qois_dh_luma2[256] = {
	0,7,14,21,28,35,42,49,56,63,6,13,20,27,34,41,3,10,17,24,31,38,45,52,59,2,9,16,23,30,37,44,
	6,13,20,27,34,41,48,55,62,5,12,19,26,33,40,47,9,16,23,30,37,44,51,58,1,8,15,22,29,36,43,50,
	12,19,26,33,40,47,54,61,4,11,18,25,32,39,46,53,15,22,29,36,43,50,57,0,7,14,21,28,35,42,49,56,
	18,25,32,39,46,53,60,3,10,17,24,31,38,45,52,59,21,28,35,42,49,56,63,6,13,20,27,34,41,48,55,62,
	24,31,38,45,52,59,2,9,16,23,30,37,44,51,58,1,27,34,41,48,55,62,5,12,19,26,33,40,47,54,61,4,
	30,37,44,51,58,1,8,15,22,29,36,43,50,57,0,7,33,40,47,54,61,4,11,18,25,32,39,46,53,60,3,10,
	36,43,50,57,0,7,14,21,28,35,42,49,56,63,6,13,39,46,53,60,3,10,17,24,31,38,45,52,59,2,9,16,
	42,49,56,63,6,13,20,27,34,41,48,55,62,5,12,19,45,52,59,2,9,16,23,30,37,44,51,58,1,8,15,22};

static void qois_put32(unsigned char* b, unsigned int v) {
	b[0] = (unsigned char)(v >> 24); b[1] = (unsigned char)(v >> 16); b[2] = (unsigned char)(v >> 8); b[3] = (unsigned char)v;
}
static unsigned int qois_get32(const unsigned char* b) {
	return (unsigned int)b[0] << 24 | (unsigned int)b[1] << 16 | (unsigned int)b[2] << 8 | (unsigned int)b[3];
}
static int qois_desc_ok(const qois_desc* d) {
	return d && d->width != 0 && d->height != 0 && (d->channels == 3 || d->channels == 4) && d->colorspace <= 1 &&
	       d->height < QOI_STREAM_PIXELS_MAX / d->width;
}
static void qois_index_clear(qois_rgba_t* index) {
	int i;
	for (i = 0; i < 64; i++) index[i].v = 0;
}

size_t qois_max_size(const qois_desc* desc) {
	uint64_t n;
	if (!qois_desc_ok(desc)) return 0;
	n = (uint64_t)desc->width * desc->height * (desc->channels + 1) + QOI_STREAM_HEADER_SIZE + QOI_STREAM_PADDING_SIZE;
	return n > (uint64_t)(size_t)-1 ? 0 : (size_t)n;
}

int qois_enc_begin(qois_enc* e, const qois_desc* desc, void* out, size_t cap) {
	size_t need;
	unsigned char* o = (unsigned char*)out;
	if (!e) return 0;
	e->state = -1;
	need = qois_max_size(desc);
	if (!need || !out || cap < need) return 0;
	qois_index_clear(e->index);
	e->prev.v = 0; e->prev.rgba.a = 255;
	e->run = 0; e->out = o; e->cap = cap; e->width = desc->width; e->channels = desc->channels;
	e->left = (uint64_t)desc->width * desc->height;
	o[0] = 'q'; o[1] = 'o'; o[2] = 'i'; o[3] = 'f';
	qois_put32(o + 4, desc->width); qois_put32(o + 8, desc->height);
	o[12] = desc->channels; o[13] = desc->colorspace;
	e->p = QOI_STREAM_HEADER_SIZE;
	e->state = 1;
	return 1;
}

/* Codes n pixels (ch: the input's channels, a constant in each call). The index is copied into a local array while it runs: the
   output is written through a char pointer, which could alias the state, so the compiler would reload it after every byte. */
static
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline)) inline
#elif defined(_MSC_VER)
__forceinline
#endif
size_t qois_enc_run(qois_enc* e, const unsigned char* px_in, size_t n, const int ch) {
	unsigned char* const bytes = e->out;
	size_t p = e->p, i;
	int run = e->run, last, k;
	qois_rgba_t px, px_prev = e->prev, index[64];
	for (k = 0; k < 64; k++) index[k] = e->index[k];
	e->left -= n;
	last = e->left == 0;
	px = px_prev;
	for (i = 0; i < n; i++, px_in += ch) {
		px.rgba.r = px_in[0]; px.rgba.g = px_in[1]; px.rgba.b = px_in[2];
		if (ch == 4) px.rgba.a = px_in[3];
		if (px.v == px_prev.v) {
			run++;
			if (run == 62 || (last && i == n - 1)) { bytes[p++] = (unsigned char)(QOIS_OP_RUN | (run - 1)); run = 0; }
		} else {
			int index_pos;
			if (run > 0) { bytes[p++] = (unsigned char)(QOIS_OP_RUN | (run - 1)); run = 0; }
			index_pos = QOIS_HASH(px);
			if (index[index_pos].v == px.v) {
				bytes[p++] = (unsigned char)(QOIS_OP_INDEX | index_pos);
			} else {
				index[index_pos] = px;
				if (px.rgba.a == px_prev.rgba.a) {
					signed char vr = (signed char)(px.rgba.r - px_prev.rgba.r);
					signed char vg = (signed char)(px.rgba.g - px_prev.rgba.g);
					signed char vb = (signed char)(px.rgba.b - px_prev.rgba.b);
					signed char vg_r = (signed char)(vr - vg);
					signed char vg_b = (signed char)(vb - vg);
					if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
						bytes[p++] = (unsigned char)(QOIS_OP_DIFF | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2));
					} else if (vg_r > -9 && vg_r < 8 && vg > -33 && vg < 32 && vg_b > -9 && vg_b < 8) {
						bytes[p++] = (unsigned char)(QOIS_OP_LUMA | (vg + 32));
						bytes[p++] = (unsigned char)((vg_r + 8) << 4 | (vg_b + 8));
					} else {
						bytes[p++] = QOIS_OP_RGB;
						bytes[p++] = px.rgba.r; bytes[p++] = px.rgba.g; bytes[p++] = px.rgba.b;
					}
				} else {
					bytes[p++] = QOIS_OP_RGBA;
					bytes[p++] = px.rgba.r; bytes[p++] = px.rgba.g; bytes[p++] = px.rgba.b; bytes[p++] = px.rgba.a;
				}
			}
		}
		px_prev = px;
	}
	if (last) {
		for (k = 0; k < 7; k++) bytes[p++] = 0;
		bytes[p++] = 1;
		e->state = 2;
	}
	for (k = 0; k < 64; k++) e->index[k] = index[k];
	e->p = p; e->run = run; e->prev = px_prev;
	return p;
}

size_t qois_enc_pixels(qois_enc* e, const void* pixels, size_t n) {
	if (!e || e->state != 1 || (n && !pixels) || (uint64_t)n > e->left) { if (e && e->state == 1) e->state = -1; return 0; }
	return e->channels == 4 ? qois_enc_run(e, (const unsigned char*)pixels, n, 4) : qois_enc_run(e, (const unsigned char*)pixels, n, 3);
}

size_t qois_enc_rows(qois_enc* e, const void* rows, size_t stride, unsigned int nrows) {
	const unsigned char* r = (const unsigned char*)rows;
	size_t row, ready;
	unsigned int y;
	if (!e || e->state != 1) { if (e && e->state == 1) e->state = -1; return 0; }
	row = (size_t)e->width * e->channels;
	if (!stride) stride = row;
	if (stride < row || (uint64_t)nrows * e->width > e->left) { e->state = -1; return 0; }
	if (!nrows) return e->p;
	if (stride == row) return qois_enc_pixels(e, r, (size_t)nrows * e->width);
	ready = e->p;
	for (y = 0; y < nrows; y++, r += stride) {
		ready = qois_enc_pixels(e, r, e->width);
		if (!ready) return 0;
	}
	return ready;
}

int qois_enc_done(const qois_enc* e) {
	return !e || e->state < 0 ? -1 : e->state == 2;
}

void qois_dec_begin(qois_dec* d, void* dst, size_t dst_cap, size_t dst_stride, int channels) {
	if (!d) return;
	qois_index_clear(d->index);
	d->px.v = 0; d->px.rgba.a = 255;
	d->hash = QOIS_HASH(d->px);
	d->desc.width = d->desc.height = 0; d->desc.channels = d->desc.colorspace = 0;
	d->left = d->done = 0; d->p = 0; d->col = 0;
	d->dst = (unsigned char*)dst; d->row = d->dst; d->cap = dst_cap; d->stride = dst_stride;
	d->asked = channels; d->channels = 0;
	d->state = (channels == 0 || channels == 3 || channels == 4) && dst ? 0 : -1;
}

/* Decodes every op complete in in[d->p .. avail) into the destination (ch: the destination's channels, a constant in each call
   so that the compiler writes a loop for each). The state lives in locals while it runs. */
static
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline)) inline
#elif defined(_MSC_VER)
__forceinline
#endif
void qois_dec_ops(qois_dec* d, const unsigned char* in, size_t avail, const int ch) {
	size_t p = d->p, col = d->col;
	uint64_t left = d->left;
	const uint64_t start = left;
	qois_rgba_t px = d->px;
	qois_rgba_t* const index = d->index;
	int hh = d->hash;
	unsigned char* rowp = d->row;
	unsigned char* o = rowp + col * (size_t)ch;
	const size_t w = d->desc.width, stride = d->stride;
	while (left) {
		unsigned int b1;
		if (p >= avail) break;
		b1 = in[p];
		if (avail - p < 5) {   /* near the end of what is here: an op is taken only when it is complete */
			const size_t len = b1 == QOIS_OP_RGBA ? 5 : b1 == QOIS_OP_RGB ? 4 : (b1 & QOIS_MASK_2) == QOIS_OP_LUMA ? 2 : 1;
			if (avail - p < len) break;
		}
		if (b1 < QOIS_OP_DIFF) {   /* INDEX: its pixel is stored again (qoi.h's rule); an empty slot's pixel 0 hashes to 0 */
			px = index[b1];
			hh = px.v ? (int)b1 : 0;
			index[hh] = px;
			p++;
		} else if (b1 < QOIS_OP_LUMA) {
			px.rgba.r = (unsigned char)(px.rgba.r + ((b1 >> 4) & 0x03) - 2);
			px.rgba.g = (unsigned char)(px.rgba.g + ((b1 >> 2) & 0x03) - 2);
			px.rgba.b = (unsigned char)(px.rgba.b + ( b1       & 0x03) - 2);
			hh = (hh + qois_dh_diff[b1 - QOIS_OP_DIFF]) & 63;
			index[hh] = px;
			p++;
		} else if (b1 < QOIS_OP_RUN) {
			unsigned int b2 = in[p + 1];
			int vg = (int)(b1 & 0x3f) - 32;
			px.rgba.r = (unsigned char)(px.rgba.r + vg - 8 + (int)((b2 >> 4) & 0x0f));
			px.rgba.g = (unsigned char)(px.rgba.g + vg);
			px.rgba.b = (unsigned char)(px.rgba.b + vg - 8 + (int)( b2       & 0x0f));
			hh = (hh + qois_dh_luma1[b1 - QOIS_OP_LUMA] + qois_dh_luma2[b2]) & 63;
			index[hh] = px;
			p += 2;
		} else if (b1 < QOIS_OP_RGB) {   /* RUN */
			uint64_t r = (uint64_t)(b1 & 0x3f) + 1;
			if (r > left) r = left;   /* (qoi.h stops at the picture's end too) */
			index[hh] = px;
			p++;
			left -= r;
			while (r) {
				size_t k = w - col, j;
				if ((uint64_t)k > r) k = (size_t)r;
				for (j = 0; j < k; j++, o += ch) {
					o[0] = px.rgba.r; o[1] = px.rgba.g; o[2] = px.rgba.b;
					if (ch == 4) o[3] = px.rgba.a;
				}
				r -= k; col += k;
				if (col == w) { col = 0; rowp += stride; o = rowp; }
			}
			continue;
		} else if (b1 == QOIS_OP_RGB) {
			px.rgba.r = in[p + 1]; px.rgba.g = in[p + 2]; px.rgba.b = in[p + 3];
			hh = QOIS_HASH(px);
			index[hh] = px;
			p += 4;
		} else {
			px.rgba.r = in[p + 1]; px.rgba.g = in[p + 2]; px.rgba.b = in[p + 3]; px.rgba.a = in[p + 4];
			hh = QOIS_HASH(px);
			index[hh] = px;
			p += 5;
		}
		o[0] = px.rgba.r; o[1] = px.rgba.g; o[2] = px.rgba.b;
		if (ch == 4) o[3] = px.rgba.a;
		o += ch; left--;
		if (++col == w) { col = 0; rowp += stride; o = rowp; }
	}
	d->p = p; d->col = col; d->row = rowp; d->px = px; d->hash = hh;
	d->done += start - left; d->left = left;
}


int qois_dec_feed(qois_dec* d, const void* data, size_t avail, int final) {
	const unsigned char* in = (const unsigned char*)data;
	size_t p;
	if (!d || d->state < 0) return QOIS_ERROR;
	if (d->state == 3) {   /* done before: only bytes after the end marker can change the verdict */
		if (final && avail != d->p) { d->state = -1; return QOIS_ERROR; }
		return QOIS_DONE;
	}
	if (!in || avail < d->p) { d->state = -1; return QOIS_ERROR; }
	if (d->state == 0) {
		uint64_t row, need;
		qois_desc h;
		if (avail < QOI_STREAM_HEADER_SIZE) { if (final) d->state = -1; return final ? QOIS_ERROR : QOIS_MORE; }
		if (in[0] != 'q' || in[1] != 'o' || in[2] != 'i' || in[3] != 'f') { d->state = -1; return QOIS_ERROR; }
		h.width = qois_get32(in + 4); h.height = qois_get32(in + 8); h.channels = in[12]; h.colorspace = in[13];
		if (!qois_desc_ok(&h)) { d->state = -1; return QOIS_ERROR; }
		d->desc = h;
		d->channels = d->asked ? d->asked : d->desc.channels;
		row = (uint64_t)d->desc.width * d->channels;
		if (!d->stride) d->stride = (size_t)row;
		if ((uint64_t)d->stride < row) { d->state = -1; return QOIS_ERROR; }
		need = (uint64_t)d->stride * (d->desc.height - 1) + row;
		if (need > (uint64_t)d->cap) { d->state = -1; return QOIS_ERROR; }
		d->left = (uint64_t)d->desc.width * d->desc.height;
		d->p = QOI_STREAM_HEADER_SIZE;
		d->state = 1;
	}
	if (d->state == 1) {
		if (d->channels == 4) qois_dec_ops(d, in, avail, 4);
		else qois_dec_ops(d, in, avail, 3);
		if (d->left) {
			if (final) { d->state = -1; return QOIS_ERROR; }   /* truncated */
			return QOIS_MORE;
		}
		d->state = 2;
	}
	p = d->p;
	/* state 2: the end marker */
	if (avail - p < QOI_STREAM_PADDING_SIZE) {
		size_t k;
		for (k = 0; k < avail - p; k++) if (in[p + k] != (k == 7)) { d->state = -1; return QOIS_ERROR; }   /* wrong already */
		if (final) { d->state = -1; return QOIS_ERROR; }
		return QOIS_MORE;
	}
	{
		int k;
		for (k = 0; k < 7; k++) if (in[p + k] != 0) { d->state = -1; return QOIS_ERROR; }
		if (in[p + 7] != 1) { d->state = -1; return QOIS_ERROR; }
	}
	d->p = p + QOI_STREAM_PADDING_SIZE;
	d->state = 3;
	if (final && avail != d->p) { d->state = -1; return QOIS_ERROR; }
	return QOIS_DONE;
}

int qois_dec_desc(const qois_dec* d, qois_desc* desc) {
	if (!d || !d->desc.width) return 0;
	if (desc) *desc = d->desc;
	return 1;
}

uint64_t qois_dec_pixels(const qois_dec* d) {
	return d ? d->done : 0;
}

unsigned int qois_dec_rows(const qois_dec* d) {
	return d && d->desc.width ? (unsigned int)(d->done / d->desc.width) : 0;
}

#endif /* QOI_STREAM_IMPLEMENTATION */
