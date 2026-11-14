/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Empty stand-in for the compiler's mm_malloc.h: xmmintrin.h includes it for _mm_malloc(), which
 * needs the C library's stdlib.h. The kernel has no C library and fl2000_simd.c does not use it.
 */
