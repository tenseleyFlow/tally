#include "config.h"

/* SSE2 is baseline on x86_64: no extra compiler flags, always available at
 * runtime. This is the x86 floor kernel when AVX2 is absent. Structure
 * mirrors simd_avx2.c: 4 independent accumulators, 64 B per iteration,
 * psadbw flush per block (4*block < 256 for the all-newline worst case). */
#if TAL_HAS_SSE2 && defined(__SSE2__)

#include <immintrin.h>

#include "simd.h"
#include "ws.h"

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

/* Fused lines+words kernel, SSE2 floor tier. No pshufb here (SSSE3), so byte
 * sets classify via a cmpeq chain over the explicit byte lists — the derived
 * sets are tiny (6-8 separators, <=4 suspects). Same word-start/carry scheme
 * as the AVX2 kernel with plain 16-byte shifts (no cross-lane dance). */

static inline __m128i match_set(__m128i v, const unsigned char *bytes, int n)
{
	__m128i m = _mm_setzero_si128();

	for (int i = 0; i < n; i++)
		m = _mm_or_si128(
			m, _mm_cmpeq_epi8(v, _mm_set1_epi8((char)bytes[i])));
	return m;
}

bool tal_lwc_sse2(const unsigned char *p, size_t n, unsigned prev_is_ws,
		  struct lwc_out *out, bool scan_suspect)
{
	const __m128i nl = _mm_set1_epi8('\n');
	const __m128i ones = _mm_set1_epi8((char)0xFF);
	unsigned long long lines = 0, words = 0;
	__m128i prevv = prev_is_ws ? _mm_setzero_si128() : ones;
	unsigned last_nonws;

	while (n >= 16) {
		size_t block = n / 16;
		__m128i lacc = _mm_setzero_si128();
		__m128i wacc = _mm_setzero_si128();

		if (block > 255)
			block = 255;
		n -= block * 16;
		do {
			__m128i v = _mm_loadu_si128(
				(const __m128i *)(const void *)p);

			if (scan_suspect) {
				__m128i s = match_set(v, tal_ws.sus_bytes,
						      tal_ws.n_sus_bytes);

				if (_mm_movemask_epi8(s))
					return false;
			}

			__m128i nonws = _mm_xor_si128(
				match_set(v, tal_ws.ws_bytes,
					  tal_ws.n_ws_bytes),
				ones);
			/* prev-byte vector: cur << 1 byte | prev >> 15. */
			__m128i shifted = _mm_or_si128(
				_mm_slli_si128(nonws, 1),
				_mm_srli_si128(prevv, 15));
			__m128i starts = _mm_andnot_si128(shifted, nonws);

			wacc = _mm_sub_epi8(wacc, starts);
			lacc = _mm_sub_epi8(lacc, _mm_cmpeq_epi8(v, nl));
			prevv = nonws;
			p += 16;
		} while (--block);
		lines += hsum(lacc);
		words += hsum(wacc);
	}
	last_nonws = ((unsigned)_mm_movemask_epi8(prevv) >> 15) & 1u;

	for (size_t i = 0; i < n; i++) {
		unsigned char b = p[i];

		if (scan_suspect && tal_ws.suspect[b])
			return false;
		unsigned nw = !tal_ws.is_ws[b];

		lines += b == '\n';
		words += nw & !last_nonws;
		last_nonws = nw;
	}
	out->lines = lines;
	out->words = words;
	out->last_is_ws = !last_nonws;
	return true;
}

#else
typedef int tal_simd_sse2_unused; /* ISO C forbids an empty translation unit */
#endif
