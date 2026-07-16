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

/* -m kernel: valid-sequence-start counting (see simd_avx2.c for the
 * derivation). Mask compares collapse the whole accumulator scheme: start
 * positions land in a __mmask64, popcnt accumulates in scalar registers. */

static inline __mmask64 range_k(__m512i v, unsigned char lo, unsigned char hi)
{
	return _mm512_cmp_epu8_mask(v, _mm512_set1_epi8((char)lo),
				    _MM_CMPINT_NLT) &
	       _mm512_cmp_epu8_mask(v, _mm512_set1_epi8((char)hi),
				    _MM_CMPINT_LE);
}

size_t tal_u8count_avx512(const unsigned char *p, size_t n,
			  unsigned long long *chars, unsigned long long *lines)
{
	const __m512i nlv = _mm512_set1_epi8('\n');
	const __m512i contmask = _mm512_set1_epi8((char)0xC0);
	const __m512i contbits = _mm512_set1_epi8((char)0x80);
	const __m512i lob = _mm512_set1_epi8((char)0x80);
	const __m512i hib = _mm512_set1_epi8((char)0xBF);
	unsigned long long ch = 0, nl = 0;
	size_t rem = n;

	while (rem >= 67) {
		__m512i v = _mm512_loadu_si512((const void *)p);
		__m512i v1 = _mm512_loadu_si512((const void *)(p + 1));
		__mmask64 cont2 = _mm512_cmpeq_epi8_mask(
			_mm512_and_si512(
				_mm512_loadu_si512((const void *)(p + 2)),
				contmask),
			contbits);
		__mmask64 cont3 = _mm512_cmpeq_epi8_mask(
			_mm512_and_si512(
				_mm512_loadu_si512((const void *)(p + 3)),
				contmask),
			contbits);
		__mmask64 cont1 = _mm512_cmpeq_epi8_mask(
			_mm512_and_si512(v1, contmask), contbits);
		__mmask64 ascii = _mm512_cmplt_epu8_mask(v, contbits);
		__mmask64 ok2 = range_k(v, 0xC2, 0xDF) & cont1;
		/* second-byte bounds tighten for E0/ED (F0/F4). */
		__m512i lo3 = _mm512_mask_blend_epi8(
			_mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8((char)0xE0)),
			lob, _mm512_set1_epi8((char)0xA0));
		__m512i hi3 = _mm512_mask_blend_epi8(
			_mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8((char)0xED)),
			hib, _mm512_set1_epi8((char)0x9F));
		__mmask64 sec3 = _mm512_cmp_epu8_mask(v1, lo3, _MM_CMPINT_NLT) &
				 _mm512_cmp_epu8_mask(v1, hi3, _MM_CMPINT_LE);
		__mmask64 ok3 = range_k(v, 0xE0, 0xEF) & sec3 & cont2;
		__m512i lo4 = _mm512_mask_blend_epi8(
			_mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8((char)0xF0)),
			lob, _mm512_set1_epi8((char)0x90));
		__m512i hi4 = _mm512_mask_blend_epi8(
			_mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8((char)0xF4)),
			hib, _mm512_set1_epi8((char)0x8F));
		__mmask64 sec4 = _mm512_cmp_epu8_mask(v1, lo4, _MM_CMPINT_NLT) &
				 _mm512_cmp_epu8_mask(v1, hi4, _MM_CMPINT_LE);
		__mmask64 ok4 = range_k(v, 0xF0, 0xF4) & sec4 & cont2 & cont3;

		ch += (unsigned long long)__builtin_popcountll(
			ascii | ok2 | ok3 | ok4);
		nl += (unsigned long long)__builtin_popcountll(
			_mm512_cmpeq_epi8_mask(v, nlv));
		p += 64;
		rem -= 64;
	}
	*chars += ch;
	*lines += nl;
	return n - rem;
}

#else
typedef int tal_simd_avx512_unused; /* ISO C forbids an empty translation unit */
#endif
