/*

Copyright (c) 2026, NI SP GmbH
SPDX-License-Identifier: MIT


qoi_stream_fuzz: a fuzz target for qoi_stream.h's decoder on hostile input (libFuzzer)

	clang -fsanitize=address,undefined,fuzzer -g -O1 qoi_stream_fuzz.c -o qoi_stream_fuzz && ./qoi_stream_fuzz

Without libFuzzer (any C compiler), -DQOIS_FUZZ_MAIN builds a driver that runs the target on each file given and on N random
mutations of each (./qoi_stream_fuzz N file.qoi ...).

The input's first byte chooses the destination's channels (0, 3, 4) and the part size the file is fed in; the rest is the file.
The destination is exactly the picture's size (a write past it is a sanitizer's error). Checked:
- fed in parts, the verdict and the pixels are those of one call;
- the pixels decoded are never more than the picture, and where the file is taken (QOIS_DONE) qoi.h decodes it to the same
  pixels, and qoi_stream's encode of them is byte-identical to qoi.h's.

*/

#define QOI_IMPLEMENTATION
#include "qoi.h"
#define QOI_STREAM_IMPLEMENTATION
#include "qoi_stream.h"

#include <stdlib.h>
#include <string.h>

#define FUZZ_MAX_PIXELS (1u << 22)   /* larger pictures are skipped (the target's memory, not a limit of the decoder) */

static int feed(const uint8_t* f, size_t n, uint8_t* dst, size_t cap, int ch, size_t part, uint64_t* px) {
	qois_dec d;
	int r = QOIS_MORE;
	qois_dec_begin(&d, dst, cap, 0, ch);
	if (!part) r = qois_dec_feed(&d, f, n, 1);
	else {
		size_t at = 0;
		while (r == QOIS_MORE) { at = n - at < part ? n : at + part; r = qois_dec_feed(&d, f, at, at == n); }
		if (r == QOIS_DONE && at < n) r = qois_dec_feed(&d, f, n, 1);   /* (done before the last part: what follows the end marker decides) */
	}
	*px = qois_dec_pixels(&d);
	return r;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	static const size_t parts[] = { 0, 1, 2, 3, 5, 97, 1169 };
	uint8_t one = 0;
	const uint8_t* f;
	size_t n, cap = 1, npx = 0;
	uint8_t *dst, *dst2;
	int ch, oc, r, r2;
	uint64_t px = 0, px2 = 0;
	if (size < 1) return 0;
	ch = (int[]){ 0, 3, 4 }[data[0] % 3];
	f = data + 1; n = size - 1;
	oc = ch;
	if (n >= 14 && !memcmp(f, "qoif", 4)) {   /* the destination exactly the picture's size */
		uint64_t w = (uint64_t)f[4] << 24 | f[5] << 16 | f[6] << 8 | f[7], h = (uint64_t)f[8] << 24 | f[9] << 16 | f[10] << 8 | f[11];
		if (!oc) oc = f[12] == 3 ? 3 : 4;
		if (w * h > FUZZ_MAX_PIXELS) return 0;
		npx = (size_t)(w * h);
		cap = npx * (size_t)oc;
	}
	if (!oc) oc = 4;
	dst = (uint8_t*)malloc(cap ? cap : 1);
	dst2 = (uint8_t*)malloc(cap ? cap : 1);
	if (!cap) { cap = 0; }
	r = feed(f, n, cap ? dst : &one, cap, ch, 0, &px);
	if (px > npx) abort();
	r2 = feed(f, n, cap ? dst2 : &one, cap, ch, parts[(data[0] / 3) % 7], &px2);
	if (r != r2 || px != px2 || (cap && memcmp(dst, dst2, (size_t)px * oc))) abort();
	if (r == QOIS_DONE) {
		qoi_desc d;
		void* hp = qoi_decode(f, (int)n, &d, oc);
		if (!hp || (size_t)d.width * d.height != npx || memcmp(hp, dst, cap)) abort();
		{   /* re-encoded: qoi_stream's file is qoi.h's */
			qoi_desc qd = { d.width, d.height, (unsigned char)oc, 0 };
			qois_desc sd = { d.width, d.height, (unsigned char)oc, 0 };
			int len = 0;
			void* ref = qoi_encode(hp, &qd, &len);
			size_t mx = qois_max_size(&sd);
			uint8_t* out = (uint8_t*)malloc(mx);
			qois_enc e;
			if (!ref || !out || !qois_enc_begin(&e, &sd, out, mx)) abort();
			if (qois_enc_rows(&e, hp, 0, d.height) != (size_t)len || qois_enc_done(&e) != 1 || memcmp(out, ref, (size_t)len)) abort();
			free(out); free(ref);
		}
		free(hp);
	}
	free(dst); free(dst2);
	return 0;
}

#ifdef QOIS_FUZZ_MAIN
#include <stdio.h>
int main(int argc, char** argv) {
	long runs = argc > 1 ? atol(argv[1]) : 1000, k, total = 0;
	int a;
	uint32_t s = 1;
	for (a = 2; a < argc; a++) {
		FILE* fp = fopen(argv[a], "rb");
		long len;
		uint8_t *b, *m;
		if (!fp) { fprintf(stderr, "%s: not readable\n", argv[a]); return 2; }
		fseek(fp, 0, SEEK_END); len = ftell(fp); fseek(fp, 0, SEEK_SET);
		b = (uint8_t*)malloc((size_t)len + 1); m = (uint8_t*)malloc((size_t)len + 1);
		if (fread(b + 1, 1, (size_t)len, fp) != (size_t)len) return 2;
		fclose(fp);
		for (k = 0; k < runs; k++) {
			size_t n = (size_t)len + 1, j, flips;
			memcpy(m, b, n);
			s ^= s << 13; s ^= s >> 17; s ^= s << 5;
			m[0] = (uint8_t)s;
			flips = k % 4 == 0 ? 0 : 1 + s % 4;
			for (j = 0; j < flips; j++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; m[1 + 14 + s % (n - 15)] ^= (uint8_t)(1 + (s >> 8) % 255); }
			if (k % 3 == 1) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; n = 1 + s % (n - 1); }   /* truncated */
			LLVMFuzzerTestOneInput(m, n);
			total++;
		}
		free(b); free(m);
	}
	printf("qoi_stream_fuzz: %ld inputs, no failure\n", total);
	return 0;
}
#endif
