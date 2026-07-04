#include "config.h"

/* Compiled only when configure found AVX2 support AND this TU got -mavx2
 * (Makefile applies AVX2_CFLAGS to this file alone — a global -mavx2 could
 * autovectorize scalar paths into illegal instructions on SSE2-only hosts). */
#if TAL_HAS_AVX2 && defined(__AVX2__)

#include <immintrin.h>

#include "simd.h"

unsigned long long tal_nlcount_avx2(const unsigned char *p, size_t n)
{
	const __m256i nl = _mm256_set1_epi8('\n');
	const __m256i zero = _mm256_setzero_si256();
	unsigned long long lines = 0;
	__m256i acc = zero;
	int iters = 0;

	while (n >= 32) {
		__m256i v = _mm256_loadu_si256((const __m256i *)(const void *)p);

		/* cmpeq lanes are 0xFF (-1); subtracting increments u8 counters. */
		acc = _mm256_sub_epi8(acc, _mm256_cmpeq_epi8(v, nl));
		p += 32;
		n -= 32;
		if (++iters == 255 || n < 32) {
			__m256i sums = _mm256_sad_epu8(acc, zero);

			lines += (unsigned long long)_mm256_extract_epi64(sums, 0)
			       + (unsigned long long)_mm256_extract_epi64(sums, 1)
			       + (unsigned long long)_mm256_extract_epi64(sums, 2)
			       + (unsigned long long)_mm256_extract_epi64(sums, 3);
			acc = zero;
			iters = 0;
		}
	}
	for (size_t i = 0; i < n; i++)
		lines += p[i] == '\n';
	return lines;
}

#else
typedef int tal_simd_avx2_unused; /* ISO C forbids an empty translation unit */
#endif
