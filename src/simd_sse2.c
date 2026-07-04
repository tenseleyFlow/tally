#include "config.h"

/* SSE2 is baseline on x86_64: no extra compiler flags, always available at
 * runtime. This is the x86 floor kernel when AVX2 is absent. Structure
 * mirrors simd_avx2.c: 4 independent accumulators, 64 B per iteration,
 * psadbw flush per block (4*block < 256 for the all-newline worst case). */
#if TAL_HAS_SSE2 && defined(__SSE2__)

#include <immintrin.h>

#include "simd.h"

static inline unsigned long long hsum(__m128i acc)
{
	__m128i s = _mm_sad_epu8(acc, _mm_setzero_si128());

	return (unsigned long long)_mm_cvtsi128_si64(s) +
	       (unsigned long long)_mm_cvtsi128_si64(_mm_srli_si128(s, 8));
}

unsigned long long tal_nlcount_sse2(const unsigned char *p, size_t n)
{
	const __m128i nl = _mm_set1_epi8('\n');
	unsigned long long lines = 0;

	while (n >= 64) {
		size_t block = n / 64;
		__m128i a0 = _mm_setzero_si128();
		__m128i a1 = _mm_setzero_si128();
		__m128i a2 = _mm_setzero_si128();
		__m128i a3 = _mm_setzero_si128();

		if (block > 63)
			block = 63;
		n -= block * 64;
		do {
			const __m128i *v = (const __m128i *)(const void *)p;

			a0 = _mm_sub_epi8(a0,
				_mm_cmpeq_epi8(_mm_loadu_si128(v), nl));
			a1 = _mm_sub_epi8(a1,
				_mm_cmpeq_epi8(_mm_loadu_si128(v + 1), nl));
			a2 = _mm_sub_epi8(a2,
				_mm_cmpeq_epi8(_mm_loadu_si128(v + 2), nl));
			a3 = _mm_sub_epi8(a3,
				_mm_cmpeq_epi8(_mm_loadu_si128(v + 3), nl));
			p += 64;
		} while (--block);
		lines += hsum(_mm_add_epi8(_mm_add_epi8(a0, a1),
					   _mm_add_epi8(a2, a3)));
	}

	if (n >= 16) {
		__m128i acc = _mm_setzero_si128();

		do {
			acc = _mm_sub_epi8(acc,
				_mm_cmpeq_epi8(_mm_loadu_si128(
					(const __m128i *)(const void *)p), nl));
			p += 16;
			n -= 16;
		} while (n >= 16); /* < 4 iterations: no overflow risk */
		lines += hsum(acc);
	}
	for (size_t i = 0; i < n; i++)
		lines += p[i] == '\n';
	return lines;
}

#else
typedef int tal_simd_sse2_unused; /* ISO C forbids an empty translation unit */
#endif
