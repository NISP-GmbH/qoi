/*

Copyright (c) 2026, NI SP GmbH
SPDX-License-Identifier: MIT


qoi_stream_test: qoi_stream.h against qoi.h

	cc -std=c99 -O2 qoi_stream_test.c -o qoi_stream_test && ./qoi_stream_test [file.qoi ...]

For each QOI file given (qoiformat.org's qoi_test_images, for example) and for generated pictures (odd sizes, runs around 62,
alpha that varies, noise, the index's edge cases), with 3 and 4 channels:
- qoi_stream's files are byte-identical to qoi.h's qoi_encode(), coded in one call, by rows in bands of every size and stride,
  and by pixels in random parts;
- qoi_stream decodes every file to qoi.h's pixels, in one call and fed in parts of 1, 7, 97, 1169 bytes and random sizes;
- every truncation is refused with final and leaves only pixels that equal the whole decode's (a prefix, never more than the
  ops complete in it); fed again with more bytes it goes on to the same pixels; a wrong end marker and bytes after it are refused;
- headers qoi.h refuses are refused, and a destination too small.
Exits 0 when every check passed, and prints the count.

*/

#define QOI_IMPLEMENTATION
#include "qoi.h"
#define QOI_STREAM_IMPLEMENTATION
#include "qoi_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks = 0, failures = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; if (failures < 50) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* qoi_stream's file of the pixels: in one call (mode 0), by rows in bands of `band` (mode 1, through a strided copy when
   stride > row), by random parts of pixels (mode 2). Returns its size, 0 on a failure. */
static size_t stream_encode(const unsigned char* px, const qois_desc* d, unsigned char* out, size_t cap, int mode, unsigned band, size_t stride) {
	qois_enc e;
	size_t ready = 0, n = (size_t)d->width * d->height, row = (size_t)d->width * d->channels;
	if (!qois_enc_begin(&e, d, out, cap)) return 0;
	if (mode == 0) ready = qois_enc_pixels(&e, px, n);
	else if (mode == 1) {
		unsigned char* sp = NULL;
		const unsigned char* src = px;
		unsigned y;
		if (stride > row) {
			sp = (unsigned char*)malloc(stride * d->height);
			for (y = 0; y < d->height; y++) { memcpy(sp + y * stride, px + y * row, row); memset(sp + y * stride + row, 0xa5, stride - row); }
			src = sp;
		}
		for (y = 0; y < d->height; y += band) {
			unsigned k = d->height - y < band ? d->height - y : band;
			size_t before = ready;
			ready = qois_enc_rows(&e, src + (size_t)y * (stride > row ? stride : row), stride > row ? stride : 0, k);
			if (!ready || ready < before) { free(sp); return 0; }
		}
		free(sp);
	} else {
		size_t at = 0;
		while (at < n) {
			size_t k = 1 + rnd() % (n < 5000 ? n : 5000);
			if (k > n - at) k = n - at;
			ready = qois_enc_pixels(&e, px + at * d->channels, k);
			if (!ready) return 0;
			at += k;
		}
	}
	if (qois_enc_done(&e) != 1) return 0;
	return ready;
}

/* qoi_stream's decode of a whole file in parts of `part` bytes (0: one call; -1: random); the result, the pixels decoded */
static int stream_decode(const unsigned char* f, size_t size, unsigned char* dst, size_t cap, int channels, long part, uint64_t* pixels) {
	qois_dec d;
	int r = QOIS_MORE;
	qois_dec_begin(&d, dst, cap, 0, channels);
	if (part == 0) r = qois_dec_feed(&d, f, size, 1);
	else {
		size_t at = 0;
		while (r == QOIS_MORE) {
			size_t k = part > 0 ? (size_t)part : 1 + rnd() % 3000;
			at = size - at < k ? size : at + k;
			r = qois_dec_feed(&d, f, at, at == size);
		}
		if (r == QOIS_DONE && at < size) r = qois_dec_feed(&d, f, size, 1);   /* (done before the last part: what follows the end marker decides) */
	}
	if (pixels) *pixels = qois_dec_pixels(&d);
	return r;
}

/* one picture: the files, the decodes, the truncations */
static void test_picture(const char* name, const unsigned char* px, unsigned w, unsigned h, int ch, int truncations) {
	qoi_desc qd; qois_desc sd;
	int len = 0;
	void* ref;
	size_t cap, n, rowb, i;
	unsigned char *out, *dst, *full;
	static const long parts[] = { 0, 1, 7, 97, 1169, -1 };
	static const unsigned bands[] = { 1, 2, 3, 7, 16, 64, 1000000 };
	qd.width = w; qd.height = h; qd.channels = (unsigned char)ch; qd.colorspace = QOI_SRGB;
	sd.width = w; sd.height = h; sd.channels = (unsigned char)ch; sd.colorspace = QOI_STREAM_SRGB;
	ref = qoi_encode(px, &qd, &len);
	CHECK(ref != NULL, "%s: qoi.h did not encode", name);
	if (!ref) return;
	cap = qois_max_size(&sd);
	CHECK(cap >= (size_t)len, "%s: max size %zu < qoi.h's file %d", name, cap, len);
	out = (unsigned char*)malloc(cap);
	n = (size_t)w * h; rowb = (size_t)w * ch;
	/* the files */
	{
		size_t s = stream_encode(px, &sd, out, cap, 0, 0, 0);
		CHECK(s == (size_t)len && !memcmp(out, ref, (size_t)len), "%s/%d: one call: %zu bytes against qoi.h's %d", name, ch, s, len);
		for (i = 0; i < sizeof bands / sizeof bands[0]; i++) {
			if (bands[i] > h && i + 1 < sizeof bands / sizeof bands[0]) continue;
			s = stream_encode(px, &sd, out, cap, 1, bands[i], 0);
			CHECK(s == (size_t)len && !memcmp(out, ref, (size_t)len), "%s/%d: bands of %u rows differ", name, ch, bands[i]);
		}
		s = stream_encode(px, &sd, out, cap, 1, 3, rowb + 4 * ch + 1);
		CHECK(s == (size_t)len && !memcmp(out, ref, (size_t)len), "%s/%d: strided rows differ", name, ch);
		s = stream_encode(px, &sd, out, cap, 2, 0, 0);
		CHECK(s == (size_t)len && !memcmp(out, ref, (size_t)len), "%s/%d: random parts differ", name, ch);
		CHECK(!qois_enc_begin(&(qois_enc){0}, &sd, out, cap - 1), "%s: a buffer one byte short taken", name);
	}
	/* the decodes, to 3 and 4 channels */
	dst = (unsigned char*)malloc(n * 4 + 16);
	full = (unsigned char*)malloc(n * 4 + 16);
	{
		int oc;
		for (oc = 3; oc <= 4; oc++) {
			qoi_desc dd;
			void* hp = qoi_decode(ref, len, &dd, oc);
			CHECK(hp != NULL, "%s: qoi.h did not decode", name);
			for (i = 0; i < sizeof parts / sizeof parts[0]; i++) {
				uint64_t got = 0;
				int r;
				memset(dst, 0x5a, n * oc);
				r = stream_decode((const unsigned char*)ref, (size_t)len, dst, n * oc, oc, parts[i], &got);
				CHECK(r == QOIS_DONE && got == n && hp && !memcmp(dst, hp, n * oc), "%s/%d->%d: parts of %ld: %d, %llu pixels", name, ch, oc, parts[i], r, (unsigned long long)got);
			}
			/* strided destination */
			{
				qois_dec d; size_t st = (size_t)w * oc + 13; unsigned y;
				unsigned char* sd2 = (unsigned char*)malloc(st * h);
				int ok = 1;
				qois_dec_begin(&d, sd2, st * (h - 1) + (size_t)w * oc, st, oc);
				ok = qois_dec_feed(&d, ref, (size_t)len, 1) == QOIS_DONE;
				for (y = 0; y < h && ok && hp; y++) ok = !memcmp(sd2 + y * st, (unsigned char*)hp + (size_t)y * w * oc, (size_t)w * oc);
				CHECK(ok, "%s/%d->%d: strided destination", name, ch, oc);
				free(sd2);
			}
			/* a destination one byte short */
			{
				uint64_t got;
				CHECK(stream_decode((const unsigned char*)ref, (size_t)len, dst, n * oc - 1, oc, 0, &got) == QOIS_ERROR && got == 0, "%s: a short destination taken", name);
			}
			if (oc == ch && hp) memcpy(full, hp, n * oc);
			free(hp);
		}
		/* the file's own channels (0) */
		{
			uint64_t got;
			CHECK(stream_decode((const unsigned char*)ref, (size_t)len, dst, n * ch, 0, 0, &got) == QOIS_DONE && !memcmp(dst, full, n * ch), "%s: channels 0", name);
		}
	}
	/* the end marker and what follows it */
	{
		unsigned char* f = (unsigned char*)malloc((size_t)len + 1);
		int k;
		uint64_t got;
		memcpy(f, ref, (size_t)len); f[len] = 0;
		CHECK(stream_decode(f, (size_t)len + 1, dst, n * ch, ch, 0, &got) == QOIS_ERROR, "%s: a byte after the end marker taken", name);
		{
			qois_dec d;
			qois_dec_begin(&d, dst, n * ch, 0, ch);
			CHECK(qois_dec_feed(&d, f, (size_t)len + 1, 0) == QOIS_DONE, "%s: not final: the trailing byte should wait", name);
			CHECK(qois_dec_feed(&d, f, (size_t)len + 1, 1) == QOIS_ERROR, "%s: final: the trailing byte refused", name);
		}
		for (k = 0; k < 8; k++) {
			memcpy(f, ref, (size_t)len);
			f[len - 8 + k] ^= (unsigned char)(k == 7 ? 3 : 1);
			CHECK(stream_decode(f, (size_t)len, dst, n * ch, ch, 0, &got) == QOIS_ERROR && got <= n, "%s: end marker byte %d changed, taken", name, k);
		}
		free(f);
	}
	/* truncations: refused with final; the pixels a prefix of the whole decode; resumable to the end */
	if (truncations) {
		size_t t, step = (size_t)len <= 20000 ? 1 : (size_t)len / 500 + (rnd() % 7);
		uint64_t prev = 0;
		for (t = 0; t < (size_t)len; t += step) {
			qois_dec d;
			uint64_t got;
			int r;
			memset(dst, 0x5a, n * ch);
			qois_dec_begin(&d, dst, n * ch, 0, ch);
			r = qois_dec_feed(&d, ref, t, 1);
			got = qois_dec_pixels(&d);
			CHECK(r == QOIS_ERROR, "%s: truncated at %zu of %d taken", name, t, len);
			CHECK(got <= n && !memcmp(dst, full, (size_t)got * ch), "%s: truncated at %zu: pixels not a prefix", name, t);
			CHECK(got >= prev, "%s: truncated at %zu: fewer pixels than at a shorter one", name, t);
			CHECK(qois_dec_rows(&d) == (unsigned)(got / w), "%s: rows", name);
			prev = got;
			/* not final: waits, then resumes on the whole file */
			qois_dec_begin(&d, dst, n * ch, 0, ch);
			r = qois_dec_feed(&d, ref, t, 0);
			CHECK(r == QOIS_MORE && qois_dec_pixels(&d) == got, "%s: at %zu not final: %d, %llu pixels against %llu", name, t, r, (unsigned long long)qois_dec_pixels(&d), (unsigned long long)got);
			r = qois_dec_feed(&d, ref, (size_t)len, 1);
			CHECK(r == QOIS_DONE && !memcmp(dst, full, n * ch), "%s: resumed from %zu: %d", name, t, r);
		}
	}
	free(full); free(dst); free(out); free(ref);
}

static unsigned char* gen(int kind, unsigned w, unsigned h, int ch) {
	size_t n = (size_t)w * h, i;
	unsigned char* p = (unsigned char*)malloc(n * ch);
	unsigned char cur[4] = { 0, 0, 0, 255 };
	int runleft = 0;
	for (i = 0; i < n; i++) {
		unsigned char* q = p + i * ch;
		switch (kind) {
		case 0: q[0] = (unsigned char)rnd(); q[1] = (unsigned char)rnd(); q[2] = (unsigned char)rnd(); if (ch == 4) q[3] = (unsigned char)rnd(); break;   /* noise */
		case 1:   /* runs of 1..130 of random colours, alpha changing now and then */
			if (!runleft) { runleft = 1 + (int)(rnd() % 130); cur[0] = (unsigned char)rnd(); cur[1] = (unsigned char)rnd(); cur[2] = (unsigned char)rnd(); if (!(rnd() % 5)) cur[3] = (unsigned char)rnd(); }
			runleft--; memcpy(q, cur, (size_t)ch); break;
		case 2:   /* small steps: DIFF and LUMA */
			cur[0] = (unsigned char)(cur[0] + (int)(rnd() % 5) - 2); cur[1] = (unsigned char)(cur[1] + (int)(rnd() % 41) - 20); cur[2] = (unsigned char)(cur[2] + (int)(rnd() % 9) - 4);
			memcpy(q, cur, (size_t)ch); break;
		case 3: {   /* a palette of 70 colours (index hits and misses), black and transparent black among them */
			static unsigned char pal[70][4]; static int made = 0; int k;
			if (!made) { for (k = 0; k < 70; k++) { pal[k][0] = (unsigned char)rnd(); pal[k][1] = (unsigned char)rnd(); pal[k][2] = (unsigned char)rnd(); pal[k][3] = (unsigned char)(k % 3 ? 255 : rnd()); } pal[0][0] = pal[0][1] = pal[0][2] = 0; pal[0][3] = 0; pal[1][0] = pal[1][1] = pal[1][2] = 0; pal[1][3] = 255; made = 1; }
			memcpy(q, pal[rnd() % 70], (size_t)ch); break; }
		case 4:   /* opaque black first (as a captured video), then transparent black, then colours: the index's edge cases */
			if (i < n / 3) { q[0] = q[1] = q[2] = 0; if (ch == 4) q[3] = 255; }
			else if (i < n / 2) { q[0] = q[1] = q[2] = 0; if (ch == 4) q[3] = (unsigned char)(i % 7 ? 0 : 255); }
			else { q[0] = (unsigned char)(i * 3); q[1] = (unsigned char)(i >> 3); q[2] = 7; if (ch == 4) q[3] = 255; }
			break;
		default:   /* runs of exactly 61..64 and 123..125 of one colour, then one change */
			if (!runleft) { static const int lens[] = { 61, 62, 63, 64, 123, 124, 125, 1, 2 }; runleft = lens[rnd() % 9]; cur[0] ^= 0x10; }
			runleft--; memcpy(q, cur, (size_t)ch); break;
		}
	}
	return p;
}

static unsigned char* readfile(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	unsigned char* b;
	long n;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	b = (unsigned char*)malloc((size_t)n + 1);
	if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
	fclose(f);
	*size = (size_t)n;
	return b;
}

int main(int argc, char** argv) {
	static const unsigned sizes[][2] = { { 1, 1 }, { 3, 5 }, { 67, 2 }, { 641, 1 }, { 1, 1080 }, { 62, 1 }, { 63, 1 }, { 64, 64 }, { 61, 37 }, { 1917, 7 }, { 125, 3 } };
	int a, k, ch;
	size_t s;
	char name[256];
	/* generated pictures */
	for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
		for (k = 0; k < 6; k++)
			for (ch = 3; ch <= 4; ch++) {
				unsigned char* px = gen(k, sizes[s][0], sizes[s][1], ch);
				snprintf(name, sizeof name, "gen%d_%ux%u", k, sizes[s][0], sizes[s][1]);
				test_picture(name, px, sizes[s][0], sizes[s][1], ch, 1);
				free(px);
			}
	/* files: decoded by qoi.h, then the same checks on their pixels, and qoi_stream decodes the file itself exactly as qoi.h */
	for (a = 1; a < argc; a++) {
		size_t size; uint64_t got;
		unsigned char* f = readfile(argv[a], &size);
		qoi_desc d;
		void* px;
		CHECK(f != NULL, "%s: not readable", argv[a]);
		if (!f) continue;
		px = qoi_decode(f, (int)size, &d, 0);
		CHECK(px != NULL, "%s: qoi.h did not decode it", argv[a]);
		if (px) {
			size_t n = (size_t)d.width * d.height;
			int oc;
			for (oc = 3; oc <= 4; oc++) {
				void* hp = qoi_decode(f, (int)size, &d, oc);
				unsigned char* dst = (unsigned char*)malloc(n * oc);
				int r = stream_decode(f, size, dst, n * oc, oc, 0, &got);
				CHECK(r == QOIS_DONE && !memcmp(dst, hp, n * oc), "%s: the file itself to %d channels: %d", argv[a], oc, r);
				r = stream_decode(f, size, dst, n * oc, oc, 1169, &got);
				CHECK(r == QOIS_DONE && !memcmp(dst, hp, n * oc), "%s: the file itself in 1169-byte parts: %d", argv[a], r);
				free(dst); free(hp);
			}
			test_picture(argv[a], (unsigned char*)px, d.width, d.height, d.channels, 1);
			free(px);
		}
		free(f);
	}
	/* headers qoi.h refuses */
	{
		unsigned char hdr[30] = { 'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 4, 0, 0xfe, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 1 };
		unsigned char dst[8];
		uint64_t got;
		CHECK(stream_decode(hdr, 26, dst, 4, 4, 0, &got) == QOIS_DONE && dst[0] == 1 && dst[3] == 255, "a 1 x 1 file");
		hdr[12] = 5; CHECK(stream_decode(hdr, 26, dst, 4, 4, 0, &got) == QOIS_ERROR, "channels 5 taken"); hdr[12] = 4;
		hdr[13] = 2; CHECK(stream_decode(hdr, 26, dst, 4, 4, 0, &got) == QOIS_ERROR, "colour space 2 taken"); hdr[13] = 0;
		hdr[7] = 0; CHECK(stream_decode(hdr, 26, dst, 4, 4, 0, &got) == QOIS_ERROR, "width 0 taken"); hdr[7] = 1;
		hdr[4] = 0x7f; hdr[8] = 0x7f; CHECK(stream_decode(hdr, 26, dst, 4, 4, 0, &got) == QOIS_ERROR, "a picture over the pixel limit taken"); hdr[4] = 0; hdr[8] = 0;
		hdr[0] = 'Q'; CHECK(stream_decode(hdr, 26, dst, 4, 4, 0, &got) == QOIS_ERROR, "a wrong magic taken"); hdr[0] = 'q';
		{ qois_desc bad = { 20000, 20000, 4, 0 }; CHECK(qois_max_size(&bad) == 0, "a picture over the pixel limit sized"); }
		{ qois_desc bad = { 1, 1, 2, 0 }; CHECK(qois_max_size(&bad) == 0, "2 channels sized"); }
	}
	printf("qoi_stream_test: %lu checks, %lu failed\n", checks, failures);
	return failures ? 1 : 0;
}
