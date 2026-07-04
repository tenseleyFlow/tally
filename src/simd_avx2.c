#include "config.h"

/* Compiled only when configure found AVX2 support AND this TU got -mavx2
 * (Makefile applies AVX2_CFLAGS to this file alone — a global -mavx2 could
 * autovectorize scalar paths into illegal instructions on SSE2-only hosts). */
#if TAL_HAS_AVX2 && defined(__AVX2__)

#include <immintrin.h>

#include "simd.h"
#include "ws.h"

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

/* Fused lines+words kernel. Whitespace-byte classification via the Mula
 * 2-pshufb LUTs from tal_ws (indices pre-masked to the low nibble, so bytes
 * >= 0x80 route through the LUT logic correctly — 0xA0 in FreeBSD's C locale
 * classifies as a separator). Word starts: non-separator preceded by
 * separator; the previous-byte vector comes from the fastlwc alignr idiom.
 * Accumulation matches the nl kernel: u8 lanes, psadbw flush per block. */

struct luts {
	__m256i ws_lo, ws_hi, sus_lo, sus_hi, nib, nl;
};

static inline __m256i classify_nonws(const struct luts *L, __m256i v)
{
	__m256i lo = _mm256_shuffle_epi8(L->ws_lo, _mm256_and_si256(v, L->nib));
	__m256i hi = _mm256_shuffle_epi8(
		L->ws_hi,
		_mm256_and_si256(_mm256_srli_epi16(v, 4), L->nib));

	/* nonzero AND => separator; cmpeq-zero => NON-separator mask. */
	return _mm256_cmpeq_epi8(_mm256_and_si256(lo, hi),
				 _mm256_setzero_si256());
}

static inline __m256i classify_suspect(const struct luts *L, __m256i v)
{
	__m256i lo = _mm256_shuffle_epi8(L->sus_lo, _mm256_and_si256(v, L->nib));
	__m256i hi = _mm256_shuffle_epi8(
		L->sus_hi,
		_mm256_and_si256(_mm256_srli_epi16(v, 4), L->nib));

	return _mm256_and_si256(lo, hi); /* nonzero byte => suspect */
}

/* previous-byte vector: byte i = cur[i-1], byte 0 = prev[31]. */
static inline __m256i shift_in_prev(__m256i cur, __m256i prev)
{
	return _mm256_alignr_epi8(
		cur, _mm256_permute2x128_si256(prev, cur, 0x21), 15);
}

bool tal_lwc_avx2(const unsigned char *p, size_t n, unsigned prev_is_ws,
		  struct lwc_out *out, bool scan_suspect)
{
	struct luts L;
	unsigned long long lines = 0, words = 0;
	/* nonws-mask carry: all-ones means "previous byte was a constituent". */
	__m256i prevv = prev_is_ws ? _mm256_setzero_si256()
				   : _mm256_set1_epi8((char)0xFF);
	unsigned last_nonws = !prev_is_ws;

	L.nib = _mm256_set1_epi8(0x0F);
	L.nl = _mm256_set1_epi8('\n');
	L.ws_lo = _mm256_broadcastsi128_si256(
		_mm_loadu_si128((const __m128i *)(const void *)tal_ws.ws_lut_lo));
	L.ws_hi = _mm256_broadcastsi128_si256(
		_mm_loadu_si128((const __m128i *)(const void *)tal_ws.ws_lut_hi));
	L.sus_lo = _mm256_broadcastsi128_si256(
		_mm_loadu_si128((const __m128i *)(const void *)tal_ws.sus_lut_lo));
	L.sus_hi = _mm256_broadcastsi128_si256(
		_mm_loadu_si128((const __m128i *)(const void *)tal_ws.sus_lut_hi));

	while (n >= 32) {
		size_t block = n / 32;
		__m256i lacc = _mm256_setzero_si256();
		__m256i wacc = _mm256_setzero_si256();

		if (block > 255)
			block = 255;
		n -= block * 32;
		do {
			__m256i v = _mm256_loadu_si256(
				(const __m256i *)(const void *)p);

			if (scan_suspect) {
				__m256i s = classify_suspect(&L, v);

				if (!_mm256_testz_si256(s, s))
					return false;
			}

			__m256i nonws = classify_nonws(&L, v);
			__m256i starts =
				_mm256_andnot_si256(shift_in_prev(nonws, prevv),
						    nonws);

			wacc = _mm256_sub_epi8(wacc, starts);
			lacc = _mm256_sub_epi8(lacc,
					       _mm256_cmpeq_epi8(v, L.nl));
			prevv = nonws;
			p += 32;
		} while (--block);
		lines += hsum(lacc);
		words += hsum(wacc);
	}
	last_nonws = ((unsigned)_mm256_movemask_epi8(prevv) >> 31) & 1u;

	for (size_t i = 0; i < n; i++) {
		unsigned char b = p[i];

		if (scan_suspect && tal_ws.suspect[b])
			return false;
		unsigned nw = !tal_ws.kernel_ws[b];

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
typedef int tal_simd_avx2_unused; /* ISO C forbids an empty translation unit */
#endif
