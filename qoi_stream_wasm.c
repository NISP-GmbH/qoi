/*

Copyright (c) 2026, NI SP GmbH
SPDX-License-Identifier: MIT


qoi_stream_wasm: qoi_stream.h as WebAssembly without a C library (make -f Makefile.stream wasm), for a check that the header
needs none (no import) and as an example of exports a page can call. Memory is the module's own; the caller places its buffers
above __heap_base.

	qois_wasm_dec_begin(dst, cap, stride, channels) / qois_wasm_dec_feed(in, avail, final) -> QOIS_* / qois_wasm_dec_pixels()
	qois_wasm_enc_begin(width, height, channels, out, cap) -> 1 / 0, qois_wasm_enc_rows(rows, stride, n) -> bytes ready

*/

#define QOI_STREAM_IMPLEMENTATION
#include "qoi_stream.h"

static qois_dec dec;
static qois_enc enc;

#define EXPORT(name) __attribute__((export_name(#name)))

EXPORT(qois_wasm_dec_begin) void qois_wasm_dec_begin(unsigned char* dst, unsigned int cap, unsigned int stride, int channels) {
	qois_dec_begin(&dec, dst, cap, stride, channels);
}
EXPORT(qois_wasm_dec_feed) int qois_wasm_dec_feed(const unsigned char* in, unsigned int avail, int final) {
	return qois_dec_feed(&dec, in, avail, final);
}
EXPORT(qois_wasm_dec_pixels) unsigned int qois_wasm_dec_pixels(void) {
	return (unsigned int)qois_dec_pixels(&dec);
}
EXPORT(qois_wasm_enc_begin) int qois_wasm_enc_begin(unsigned int w, unsigned int h, int channels, unsigned char* out, unsigned int cap) {
	qois_desc d;
	d.width = w; d.height = h; d.channels = (unsigned char)channels; d.colorspace = QOI_STREAM_SRGB;
	return qois_enc_begin(&enc, &d, out, cap);
}
EXPORT(qois_wasm_enc_rows) unsigned int qois_wasm_enc_rows(const unsigned char* rows, unsigned int stride, unsigned int n) {
	return (unsigned int)qois_enc_rows(&enc, rows, stride, n);
}
