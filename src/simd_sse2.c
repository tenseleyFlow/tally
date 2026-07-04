#include "config.h"

/* SSE2 is baseline on x86_64: no extra compiler flags, always available at
 * runtime. This is the x86 floor kernel when AVX2 is absent. */
#if TAL_HAS_SSE2 && defined(__SSE2__)

#include <immintrin.h>

#include "simd.h"

unsigned long long tal_nlcount_sse2(const unsigned char *p, size_t n)
{
	const __m128i nl = _mm_set1_epi8('\n');
	const __m128i zero = _mm_setzero_si128();
	unsigned long long lines = 0;
	__m128i acc = zero;
	int iters = 0;

	while (n >= 16) {
		__m128i v = _mm_loadu_si128((const __m128i *)(const void *)p);

		acc = _mm_sub_epi8(acc, _mm_cmpeq_epi8(v, nl));
		p += 16;
		n -= 16;
		if (++iters == 255 || n < 16) {
			__m128i sums = _mm_sad_epu8(acc, zero);

			lines += (unsigned long long)_mm_cvtsi128_si64(sums)
			       + (unsigned long long)_mm_cvtsi128_si64(
					_mm_srli_si128(sums, 8));
			acc = zero;
			iters = 0;
		}
	}
	for (size_t i = 0; i < n; i++)
		lines += p[i] == '\n';
	return lines;
}

#else
typedef int tal_simd_sse2_unused; /* ISO C forbids an empty translation unit */
#endif
