#include "config.h"

/* Compiled only when configure found AVX2 support AND this TU got -mavx2
 * (Makefile applies AVX2_CFLAGS to this file alone — a global -mavx2 could
 * autovectorize scalar paths into illegal instructions on SSE2-only hosts). */
#if TAL_HAS_AVX2 && defined(__AVX2__)

#include <immintrin.h>

#include "simd.h"

/* 4 independent u8-lane accumulators (cmpeq yields -1; subtracting counts),
 * 128 B per iteration, flushed through psadbw every 8 KiB block — well under
 * the 255-adds-per-lane u8 bound. Single-accumulator loops serialize on the
 * sub dependency and lose ~40% to GNU's AVX-512 kernel (measured, audit 03). */
static inline unsigned long long hsum(__m256i acc)
{
	__m256i s = _mm256_sad_epu8(acc, _mm256_setzero_si256());

	return (unsigned long long)_mm256_extract_epi64(s, 0) +
	       (unsigned long long)_mm256_extract_epi64(s, 1) +
	       (unsigned long long)_mm256_extract_epi64(s, 2) +
	       (unsigned long long)_mm256_extract_epi64(s, 3);
}

unsigned long long tal_nlcount_avx2(const unsigned char *p, size_t n)
{
	const __m256i nl = _mm256_set1_epi8('\n');
	unsigned long long lines = 0;

	while (n >= 128) {
		size_t block = n / 128;
		__m256i a0 = _mm256_setzero_si256();
		__m256i a1 = _mm256_setzero_si256();
		__m256i a2 = _mm256_setzero_si256();
		__m256i a3 = _mm256_setzero_si256();

		/* Accumulators merge lane-wise before one hsum, so 4*block must
		 * stay under 256 (all-newline input maxes every lane). */
		if (block > 63)
			block = 63; /* ~8 KiB per flush */
		n -= block * 128;
		do {
			const __m256i *v = (const __m256i *)(const void *)p;

			a0 = _mm256_sub_epi8(a0,
				_mm256_cmpeq_epi8(_mm256_loadu_si256(v), nl));
			a1 = _mm256_sub_epi8(a1,
				_mm256_cmpeq_epi8(_mm256_loadu_si256(v + 1), nl));
			a2 = _mm256_sub_epi8(a2,
				_mm256_cmpeq_epi8(_mm256_loadu_si256(v + 2), nl));
			a3 = _mm256_sub_epi8(a3,
				_mm256_cmpeq_epi8(_mm256_loadu_si256(v + 3), nl));
			p += 128;
		} while (--block);
		lines += hsum(_mm256_add_epi8(_mm256_add_epi8(a0, a1),
					      _mm256_add_epi8(a2, a3)));
	}

	if (n >= 32) {
		__m256i acc = _mm256_setzero_si256();

		do {
			acc = _mm256_sub_epi8(acc,
				_mm256_cmpeq_epi8(_mm256_loadu_si256(
					(const __m256i *)(const void *)p), nl));
			p += 32;
			n -= 32;
		} while (n >= 32); /* < 8 iterations: no overflow risk */
		lines += hsum(acc);
	}
	for (size_t i = 0; i < n; i++)
		lines += p[i] == '\n';
	return lines;
}

#else
typedef int tal_simd_avx2_unused; /* ISO C forbids an empty translation unit */
#endif
