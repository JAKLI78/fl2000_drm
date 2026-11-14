// SPDX-License-Identifier: GPL-2.0
/*
 * SSSE3/SSE4.1 pixel conversion. This file is built with SIMD enabled, so everything in it may use
 * vector registers: call it only between kernel_fpu_begin() and kernel_fpu_end(), and only if the
 * CPU has SSSE3 and SSE4.1 (see fl2000_stream_fill()). Output is identical to the scalar code.
 */

#include <linux/types.h>
#include <smmintrin.h> /* SSE4.1, includes SSSE3 */

void fl2000_simd_rgb565_line(u16 *dbuf, const u32 *sbuf, u32 pixels, const u32 *dither);
void fl2000_simd_rgb888_line(u8 *dbuf, const u32 *sbuf, u32 pixels);

/* XRGB8888 -> RGB565, 16-bit pixels with 32-bit halves of each 8-byte group swapped (x ^ 2) */
/* dither: per-channel offsets for pixels x % 4 == 0..3 of this line, added with saturation, or
 * NULL
 */
void fl2000_simd_rgb565_line(u16 *dbuf, const u32 *sbuf, u32 pixels, const u32 *dither)
{
	const __m128i dv = dither ? _mm_loadu_si128((const __m128i *)dither) : _mm_setzero_si128();
	const __m128i r_mask = _mm_set1_epi32(0xF800), g_mask = _mm_set1_epi32(0x07E0),
		      b_mask = _mm_set1_epi32(0x001F);
	unsigned int x;

	for (x = 0; x + 8 <= pixels; x += 8) {
		__m128i p0 = _mm_adds_epu8(_mm_loadu_si128((const __m128i *)(sbuf + x)), dv);
		__m128i p1 = _mm_adds_epu8(_mm_loadu_si128((const __m128i *)(sbuf + x + 4)), dv);
		__m128i v0 = _mm_or_si128(_mm_or_si128(_mm_and_si128(_mm_srli_epi32(p0, 8), r_mask),
						       _mm_and_si128(_mm_srli_epi32(p0, 5), g_mask)),
					  _mm_and_si128(_mm_srli_epi32(p0, 3), b_mask));
		__m128i v1 = _mm_or_si128(_mm_or_si128(_mm_and_si128(_mm_srli_epi32(p1, 8), r_mask),
						       _mm_and_si128(_mm_srli_epi32(p1, 5), g_mask)),
					  _mm_and_si128(_mm_srli_epi32(p1, 3), b_mask));
		/* 8 x u16 p0..p7, then swap 32-bit halves of each 64-bit group: p2 p3 p0 p1 ... */
		__m128i v = _mm_packus_epi32(v0, v1);

		v = _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1));
		_mm_storeu_si128((__m128i *)(dbuf + x), v);
	}
	for (; x < pixels; x++) {
		unsigned int p = sbuf[x];

		if (dither) {
			unsigned int d = dither[x & 3], q = 0;

			for (int c = 0; c < 24; c += 8) {
				unsigned int v = ((p >> c) & 0xFF) + ((d >> c) & 0xFF);

				q |= (v > 0xFF ? 0xFF : v) << c;
			}
			p = q;
		}
		dbuf[x ^ 2] = ((p & 0x00F80000) >> 8) | ((p & 0x0000FC00) >> 5) | ((p & 0x000000F8) >> 3);
	}
}

/* Byte k of 24 output bytes for 8 pixels is stream byte k ^ 4; stream byte j is channel j % 3
 * of pixel j / 3, i.e. input byte 4 * (j / 3) + j % 3. Shuffle masks for pixels 0-3 (in0) and
 * 4-7 (in1); 0x80 yields zero
 */
#define FL2000_SRC(k) (4 * (((k) ^ 4) / 3) + ((k) ^ 4) % 3)
#define FL2000_M0(k) (FL2000_SRC(k) < 16 ? FL2000_SRC(k) : 0x80)
#define FL2000_M1(k) (FL2000_SRC(k) >= 16 ? FL2000_SRC(k) - 16 : 0x80)
#define FL2000_MASK(M, b)                                                                  \
	_mm_setr_epi8(M(b + 0), M(b + 1), M(b + 2), M(b + 3), M(b + 4), M(b + 5), M(b + 6), \
		      M(b + 7), M(b + 8), M(b + 9), M(b + 10), M(b + 11), M(b + 12),       \
		      M(b + 13), M(b + 14), M(b + 15))

/* XRGB8888 -> RGB888 (B, G, R bytes) with 32-bit halves of each 8-byte group swapped (xx ^ 4) */
void fl2000_simd_rgb888_line(u8 *dbuf, const u32 *sbuf, u32 pixels)
{
	const __m128i lo0 = FL2000_MASK(FL2000_M0, 0), lo1 = FL2000_MASK(FL2000_M1, 0);
	const __m128i hi0 = FL2000_MASK(FL2000_M0, 16), hi1 = FL2000_MASK(FL2000_M1, 16);
	unsigned int x, xx;

	for (x = 0; x + 8 <= pixels; x += 8) {
		__m128i p0 = _mm_loadu_si128((const __m128i *)(sbuf + x));
		__m128i p1 = _mm_loadu_si128((const __m128i *)(sbuf + x + 4));
		__m128i lo = _mm_or_si128(_mm_shuffle_epi8(p0, lo0), _mm_shuffle_epi8(p1, lo1));
		__m128i hi = _mm_or_si128(_mm_shuffle_epi8(p0, hi0), _mm_shuffle_epi8(p1, hi1));

		_mm_storeu_si128((__m128i *)(dbuf + 3 * x), lo);
		_mm_storel_epi64((__m128i *)(dbuf + 3 * x + 16), hi);
	}
	/* 8 pixels are 3 whole 8-byte groups, so the tail starts on a group boundary */
	dbuf += 3 * x;
	for (xx = 0; x < pixels; x++) {
		unsigned int p = sbuf[x];

		dbuf[xx++ ^ 4] = p & 0xFF;
		dbuf[xx++ ^ 4] = (p >> 8) & 0xFF;
		dbuf[xx++ ^ 4] = (p >> 16) & 0xFF;
	}
}
