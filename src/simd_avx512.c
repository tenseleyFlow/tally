#include "config.h"

/* Compiled only when configure found AVX-512 F+BW support AND this TU got
 * AVX512_CFLAGS (Makefile applies them to this file alone — same isolation
 * rule as simd_avx2.c). Runtime-gated on avx512f+avx512bw.
 *
 * GNU wc 9.9+ counts -l with AVX-512 and beat our AVX2 kernel by 8-13% on
 * compute-bound hosts (Ice Lake CI runners, 2026-07-16). Mask compares make
 * this kernel simpler than the AVX2 one: cmpeq yields a bit per lane, popcnt
 * accumulates in scalar registers — no u8-lane saturation, no psadbw flush. */
#if TAL_HAS_AVX512 && defined(__AVX512F__) && defined(__AVX512BW__)

#include <immintrin.h>

#include "simd.h"

unsigned long long tal_nlcount_avx512(const unsigned char *p, size_t n)
{
	const __m512i nl = _mm512_set1_epi8('\n');
	unsigned long long lines = 0;

	while (n >= 256) {
		__mmask64 m0 = _mm512_cmpeq_epi8_mask(
			_mm512_loadu_si512((const void *)p), nl);
		__mmask64 m1 = _mm512_cmpeq_epi8_mask(
			_mm512_loadu_si512((const void *)(p + 64)), nl);
		__mmask64 m2 = _mm512_cmpeq_epi8_mask(
			_mm512_loadu_si512((const void *)(p + 128)), nl);
		__mmask64 m3 = _mm512_cmpeq_epi8_mask(
			_mm512_loadu_si512((const void *)(p + 192)), nl);

		lines += (unsigned long long)__builtin_popcountll(m0) +
			 (unsigned long long)__builtin_popcountll(m1) +
			 (unsigned long long)__builtin_popcountll(m2) +
			 (unsigned long long)__builtin_popcountll(m3);
		p += 256;
		n -= 256;
	}
	while (n >= 64) {
		__mmask64 m = _mm512_cmpeq_epi8_mask(
			_mm512_loadu_si512((const void *)p), nl);

		lines += (unsigned long long)__builtin_popcountll(m);
		p += 64;
		n -= 64;
	}
	for (size_t i = 0; i < n; i++)
		lines += p[i] == '\n';
	return lines;
}

#else
typedef int tal_simd_avx512_unused; /* ISO C forbids an empty translation unit */
#endif
