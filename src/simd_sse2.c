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

static inline __m128i shl1_from(__m128i cur, __m128i prev)
{
	return _mm_or_si128(_mm_slli_si128(cur, 1), _mm_srli_si128(prev, 15));
}

static inline __m128i shl2_from(__m128i cur, __m128i prev)
{
	return _mm_or_si128(_mm_slli_si128(cur, 2), _mm_srli_si128(prev, 14));
}

size_t tal_lwc_sse2(const unsigned char *p, size_t n, unsigned prev_is_ws,
		    struct lwc_out *out)
{
	const __m128i nl = _mm_set1_epi8('\n');
	const __m128i ones = _mm_set1_epi8((char)0xFF);
	unsigned long long lines = 0, words = 0;
	__m128i prevv = prev_is_ws ? _mm_setzero_si128() : ones;
	__m128i prev_sep = _mm_setzero_si128();
	__m128i prev_m3 = _mm_setzero_si128();
	bool prev_dirty = false;
	unsigned last_nonws;
	int ng = tal_ws.nmbws ? tal_ws.ngroups : 0;
	size_t k = n;

	if (ng)
		while (k > 0 && (tal_ws.suspect[p[k - 1]] ||
				 (k > 1 && tal_ws.is3lead[p[k - 2]])))
			k--;

	size_t rem = k;

	while (rem >= 18) {
		size_t block = (rem - 2) / 16;
		__m128i lacc = _mm_setzero_si128();
		__m128i wacc = _mm_setzero_si128();

		if (block > 255)
			block = 255;
		rem -= block * 16;
		do {
			__m128i v = _mm_loadu_si128(
				(const __m128i *)(const void *)p);
			__m128i nonws = _mm_xor_si128(
				match_set(v, tal_ws.ws_bytes,
					  tal_ws.n_ws_bytes),
				ones);

			if (ng) {
				__m128i sus = match_set(v, tal_ws.sus_bytes,
							tal_ws.n_sus_bytes);
				__m128i sep = _mm_setzero_si128();
				__m128i m3 = _mm_setzero_si128();

				if (_mm_movemask_epi8(sus) || prev_dirty) {
					__m128i v1 = _mm_loadu_si128(
						(const __m128i *)(const void *)(p + 1));
					__m128i v2 = _mm_loadu_si128(
						(const __m128i *)(const void *)(p + 2));

					for (int g = 0; g < ng; g++) {
						const struct mbws_group *s =
							&tal_ws.groups[g];
						__m128i hit = _mm_cmpeq_epi8(
							v, _mm_set1_epi8(
								(char)s->lead));

						if (s->len == 2) {
							hit = _mm_and_si128(
								hit,
								match_set(v1,
									s->set_bytes,
									s->nset));
						} else {
							hit = _mm_and_si128(
								hit,
								_mm_cmpeq_epi8(
									v1,
									_mm_set1_epi8(
										(char)s->second)));
							hit = _mm_and_si128(
								hit,
								match_set(v2,
									s->set_bytes,
									s->nset));
							m3 = _mm_or_si128(m3, hit);
						}
						sep = _mm_or_si128(sep, hit);
					}
					__m128i mask = _mm_or_si128(
						sep,
						_mm_or_si128(
							shl1_from(sep, prev_sep),
							shl2_from(m3, prev_m3)));

					nonws = _mm_andnot_si128(mask, nonws);
					prev_dirty =
						_mm_movemask_epi8(sep) != 0;
				}
				prev_sep = sep;
				prev_m3 = m3;
			}

			__m128i starts = _mm_andnot_si128(
				shl1_from(nonws, prevv), nonws);

			wacc = _mm_sub_epi8(wacc, starts);
			lacc = _mm_sub_epi8(lacc, _mm_cmpeq_epi8(v, nl));
			prevv = nonws;
			p += 16;
		} while (--block);
		lines += hsum(lacc);
		words += hsum(wacc);
	}
	last_nonws = ((unsigned)_mm_movemask_epi8(prevv) >> 15) & 1u;

	unsigned m3m = (unsigned)_mm_movemask_epi8(prev_m3);
	unsigned sepm = (unsigned)_mm_movemask_epi8(prev_sep);
	size_t cover = 0;

	if (m3m & 0x8000u)
		cover = 2;
	else if ((sepm & 0x8000u) || (m3m & 0x4000u))
		cover = 1;

	for (size_t i = 0; i < rem; i++) {
		unsigned char b = p[i];
		unsigned is_sep;

		if (cover) {
			is_sep = 1;
			cover--;
		} else {
			int m = ng ? tal_mbws_match(p + i, rem - i) : 0;

			if (m) {
				is_sep = 1;
				cover = (size_t)m - 1;
			} else {
				is_sep = tal_ws.kernel_ws[b];
			}
		}
		unsigned nw = !is_sep;

		lines += b == '\n';
		words += nw & !last_nonws;
		last_nonws = nw;
	}
	out->lines = lines;
	out->words = words;
	out->last_is_ws = !last_nonws;
	return k;
}

size_t tal_lscan_sse2(const unsigned char *p, size_t n,
		      unsigned long long *linepos, unsigned long long *maxlen)
{
	const __m128i nlv = _mm_set1_epi8('\n');
	unsigned long long lp = *linepos, ml = *maxlen;
	size_t consumed = 0;

	while (n - consumed >= 16) {
		__m128i v = _mm_loadu_si128(
			(const __m128i *)(const void *)(p + consumed));
		__m128i isnl = _mm_cmpeq_epi8(v, nlv);
		/* printable ASCII 0x20-0x7E: (v - 0x20) unsigned <= 0x5E */
		__m128i x = _mm_sub_epi8(v, _mm_set1_epi8(0x20));
		__m128i plain = _mm_cmpeq_epi8(
			_mm_min_epu8(x, _mm_set1_epi8(0x5E)), x);
		unsigned special = 0xFFFFu & ~(unsigned)_mm_movemask_epi8(
			_mm_or_si128(plain, isnl));

		if (special)
			break;

		unsigned nlm = (unsigned)_mm_movemask_epi8(isnl);

		if (!nlm) {
			lp += 16;
		} else {
			unsigned prev = 0;

			while (nlm) {
				unsigned j = (unsigned)__builtin_ctz(nlm);

				lp += j - prev;
				if (lp > ml)
					ml = lp;
				lp = 0;
				prev = j + 1;
				nlm &= nlm - 1;
			}
			lp += 16 - prev;
		}
		consumed += 16;
	}
	while (consumed < n) {
		unsigned char b = p[consumed];

		if (b == '\n') {
			if (lp > ml)
				ml = lp;
			lp = 0;
		} else if (b >= 0x20 && b <= 0x7E) {
			lp++;
		} else {
			break;
		}
		consumed++;
	}
	*linepos = lp;
	*maxlen = ml;
	return consumed;
}

#else
typedef int tal_simd_sse2_unused; /* ISO C forbids an empty translation unit */
#endif
