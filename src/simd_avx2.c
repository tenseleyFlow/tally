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

/* two-back vector: byte i = cur[i-2], bytes 0-1 = prev[30..31]. */
static inline __m256i shift_in_prev2(__m256i cur, __m256i prev)
{
	return _mm256_alignr_epi8(
		cur, _mm256_permute2x128_si256(prev, cur, 0x21), 14);
}

/* L1 pattern groups pre-broadcast for the dirty path. */
struct l1g {
	__m256i lead, second, lo, hi;
	int len;
};

static inline __m256i in_set(__m256i v, __m256i lo, __m256i hi, __m256i nib)
{
	__m256i t = _mm256_and_si256(
		_mm256_shuffle_epi8(lo, _mm256_and_si256(v, nib)),
		_mm256_shuffle_epi8(
			hi, _mm256_and_si256(_mm256_srli_epi16(v, 4), nib)));

	/* members => nonzero; return 0xFF membership mask */
	return _mm256_xor_si256(_mm256_cmpeq_epi8(t, _mm256_setzero_si256()),
				_mm256_set1_epi8((char)0xFF));
}

size_t tal_lwc_avx2(const unsigned char *p, size_t n, unsigned prev_is_ws,
		    struct lwc_out *out)
{
	struct luts L;
	struct l1g G[8];
	int ng = 0;
	unsigned long long lines = 0, words = 0;
	/* nonws-mask carry: all-ones means "previous byte was a constituent". */
	__m256i prevv = prev_is_ws ? _mm256_setzero_si256()
				   : _mm256_set1_epi8((char)0xFF);
	__m256i prev_sep = _mm256_setzero_si256();
	__m256i prev_m3 = _mm256_setzero_si256();
	bool prev_dirty = false;
	unsigned last_nonws = !prev_is_ws;
	size_t k = n;

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

	if (tal_ws.nmbws) {
		ng = tal_ws.ngroups;
		for (int g = 0; g < ng; g++) {
			const struct mbws_group *s = &tal_ws.groups[g];

			G[g].lead = _mm256_set1_epi8((char)s->lead);
			G[g].second = _mm256_set1_epi8((char)s->second);
			G[g].lo = _mm256_broadcastsi128_si256(_mm_loadu_si128(
				(const __m128i *)(const void *)s->set_lo));
			G[g].hi = _mm256_broadcastsi128_si256(_mm_loadu_si128(
				(const __m128i *)(const void *)s->set_hi));
			G[g].len = s->len;
		}
		/* Hold back a tail whose pattern can't be verified locally:
		 * no match may start at k-1 (needs >= k) or, for a 3-byte
		 * lead, at k-2. Held bytes go to the scalar oracle. */
		while (k > 0 && (tal_ws.suspect[p[k - 1]] ||
				 (k > 1 && tal_ws.is3lead[p[k - 2]])))
			k--;
	}

	size_t rem = k;

	/* Vector loop needs 2 bytes of in-span lookahead (loadu at p+2), so
	 * it stops 34 short; the pattern-aware scalar tail finishes. */
	while (rem >= 34) {
		size_t block = (rem - 2) / 32;
		__m256i lacc = _mm256_setzero_si256();
		__m256i wacc = _mm256_setzero_si256();

		if (block > 255)
			block = 255;
		rem -= block * 32;
		do {
			__m256i v = _mm256_loadu_si256(
				(const __m256i *)(const void *)p);
			__m256i nonws = classify_nonws(&L, v);

			if (ng) {
				__m256i sus = classify_suspect(&L, v);
				__m256i sep = _mm256_setzero_si256();
				__m256i m3 = _mm256_setzero_si256();

				if (!_mm256_testz_si256(sus, sus) ||
				    prev_dirty) {
					__m256i v1 = _mm256_loadu_si256(
						(const __m256i *)(const void *)(p + 1));
					__m256i v2 = _mm256_loadu_si256(
						(const __m256i *)(const void *)(p + 2));

					for (int g = 0; g < ng; g++) {
						__m256i hit = _mm256_cmpeq_epi8(
							v, G[g].lead);

						if (G[g].len == 2) {
							hit = _mm256_and_si256(
								hit,
								in_set(v1, G[g].lo,
								       G[g].hi, L.nib));
						} else {
							hit = _mm256_and_si256(
								hit,
								_mm256_cmpeq_epi8(
									v1, G[g].second));
							hit = _mm256_and_si256(
								hit,
								in_set(v2, G[g].lo,
								       G[g].hi, L.nib));
							m3 = _mm256_or_si256(m3, hit);
						}
						sep = _mm256_or_si256(sep, hit);
					}
					/* separator mask = starts + their
					 * continuation bytes */
					__m256i mask = _mm256_or_si256(
						sep,
						_mm256_or_si256(
							shift_in_prev(sep, prev_sep),
							shift_in_prev2(m3, prev_m3)));

					nonws = _mm256_andnot_si256(mask, nonws);
					prev_dirty = !_mm256_testz_si256(sep, sep);
				}
				prev_sep = sep;
				prev_m3 = m3;
			}

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

	/* Matches begun in the final vectors may cover the first tail bytes. */
	unsigned m3m = (unsigned)_mm256_movemask_epi8(prev_m3);
	unsigned sepm = (unsigned)_mm256_movemask_epi8(prev_sep);
	size_t cover = 0;

	if (m3m & 0x80000000u)
		cover = 2;
	else if ((sepm & 0x80000000u) || (m3m & 0x40000000u))
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

/* Validated -m kernel. Explicit structural validation (no lookup tables):
 * the continuation mask must EQUAL the expectation mask built from shifted
 * lead classes, C0/C1/F5-FF are always invalid, and four lead values carry
 * second-byte range constraints (E0 overlong, ED surrogate, F0 overlong,
 * F4 too-large). Spans end sequence-complete (holds), so expectation
 * carries start at zero per span and a rejected span never contains a
 * sequence begun in a committed one. */

static inline __m256i range_le(__m256i v, unsigned char lim)
{
	/* v <= lim (unsigned) */
	return _mm256_cmpeq_epi8(_mm256_min_epu8(v, _mm256_set1_epi8((char)lim)),
				 v);
}

static inline __m256i range_in(__m256i v, unsigned char lo, unsigned char hi)
{
	/* lo <= v <= hi (unsigned): (v - lo) <= (hi - lo) */
	__m256i x = _mm256_sub_epi8(v, _mm256_set1_epi8((char)lo));

	return range_le(x, (unsigned char)(hi - lo));
}

/* Hold back a trailing incomplete sequence (<=3 bytes) so the span ends
 * sequence-complete. */
static size_t u8_seq_hold(const unsigned char *p, size_t k)
{
	if (k >= 1 && p[k - 1] >= 0xC2 && p[k - 1] <= 0xF4)
		return k - 1;
	if (k >= 2 && p[k - 2] >= 0xE0 && p[k - 2] <= 0xF4)
		return k - 2;
	if (k >= 3 && p[k - 3] >= 0xF0 && p[k - 3] <= 0xF4)
		return k - 3;
	return k;
}

size_t tal_u8count_avx2(const unsigned char *p, size_t n,
			unsigned long long *chars, unsigned long long *lines)
{
	const __m256i zero = _mm256_setzero_si256();
	const __m256i nlv = _mm256_set1_epi8('\n');
	const __m256i contmask = _mm256_set1_epi8((char)0xC0);
	const __m256i contbits = _mm256_set1_epi8((char)0x80);
	const __m256i ones = _mm256_set1_epi8((char)0xFF);
	size_t consumed = 0;

	while (consumed < n) {
		size_t span = n - consumed < 8192 ? n - consumed : 8192;

		span = u8_seq_hold(p + consumed, span);
		if (span == 0)
			break;

		const unsigned char *q = p + consumed;
		size_t rem = span;
		unsigned long long ch = 0, nl = 0;
		__m256i cacc = zero, lacc = zero, err = zero;
		__m256i pl234 = zero, pl34 = zero, pl4 = zero;
		int iters = 0;
		bool ok = true;

		while (rem >= 33) {
			__m256i v = _mm256_loadu_si256(
				(const __m256i *)(const void *)q);
			__m256i v1 = _mm256_loadu_si256(
				(const __m256i *)(const void *)(q + 1));
			__m256i cont = _mm256_cmpeq_epi8(
				_mm256_and_si256(v, contmask), contbits);
			__m256i l2 = range_in(v, 0xC2, 0xDF);
			__m256i l3 = range_in(v, 0xE0, 0xEF);
			__m256i l4 = range_in(v, 0xF0, 0xF4);
			__m256i bad = _mm256_or_si256(
				range_in(v, 0xC0, 0xC1),
				_mm256_xor_si256(range_le(v, 0xF4), ones));
			__m256i sp = _mm256_and_si256(
				_mm256_cmpeq_epi8(v, _mm256_set1_epi8((char)0xE0)),
				range_le(v1, 0x9F));

			sp = _mm256_or_si256(sp, _mm256_and_si256(
				_mm256_cmpeq_epi8(v, _mm256_set1_epi8((char)0xED)),
				_mm256_xor_si256(range_le(v1, 0x9F), ones)));
			sp = _mm256_or_si256(sp, _mm256_and_si256(
				_mm256_cmpeq_epi8(v, _mm256_set1_epi8((char)0xF0)),
				range_le(v1, 0x8F)));
			sp = _mm256_or_si256(sp, _mm256_and_si256(
				_mm256_cmpeq_epi8(v, _mm256_set1_epi8((char)0xF4)),
				_mm256_xor_si256(range_le(v1, 0x8F), ones)));

			__m256i l234 = _mm256_or_si256(l2,
						       _mm256_or_si256(l3, l4));
			__m256i l34 = _mm256_or_si256(l3, l4);
			__m256i expect = _mm256_or_si256(
				shift_in_prev(l234, pl234),
				_mm256_or_si256(
					shift_in_prev2(l34, pl34),
					_mm256_alignr_epi8(
						l4,
						_mm256_permute2x128_si256(
							pl4, l4, 0x21),
						13)));

			err = _mm256_or_si256(err, _mm256_or_si256(bad, sp));
			err = _mm256_or_si256(err,
					      _mm256_xor_si256(cont, expect));
			cacc = _mm256_sub_epi8(cacc, cont);
			lacc = _mm256_sub_epi8(lacc,
					       _mm256_cmpeq_epi8(v, nlv));
			pl234 = l234;
			pl34 = l34;
			pl4 = l4;
			ch += 32;
			q += 32;
			rem -= 32;
			if (++iters == 255 || rem < 33) {
				if (!_mm256_testz_si256(err, err)) {
					ok = false;
					break;
				}
				ch -= hsum(cacc);
				nl += hsum(lacc);
				cacc = lacc = zero;
				iters = 0;
			}
		}
		if (ok && rem) {
			/* A char begun in the vector region may extend into
			 * the tail: back up to its lead (its expectation
			 * bytes were never compared) and re-walk it whole,
			 * deducting the lead's already-counted char. */
			size_t back = 0;

			while (back < 3 && q - back > p + consumed &&
			       (*(q - back - 1) & 0xC0) == 0x80)
				back++;
			if (q - back > p + consumed && *(q - back - 1) >= 0xC2 &&
			    *(q - back - 1) <= 0xF4) {
				back++;
				ch--;
			} else {
				back = 0;
			}
			if (tal_u8walk(q - back, rem + back, &ch, &nl) != 0)
				ok = false;
		}
		if (!ok)
			break;
		*chars += ch;
		*lines += nl;
		consumed += span;
	}
	return consumed;
}

size_t tal_lscan_avx2(const unsigned char *p, size_t n,
		      unsigned long long *linepos, unsigned long long *maxlen)
{
	const __m256i nlv = _mm256_set1_epi8('\n');
	unsigned long long lp = *linepos, ml = *maxlen;
	size_t consumed = 0;

	while (n - consumed >= 32) {
		__m256i v = _mm256_loadu_si256(
			(const __m256i *)(const void *)(p + consumed));
		__m256i isnl = _mm256_cmpeq_epi8(v, nlv);
		/* printable ASCII: 0x20 <= b <= 0x7E */
		__m256i plain = range_in(v, 0x20, 0x7E);
		unsigned special = ~(unsigned)_mm256_movemask_epi8(
			_mm256_or_si256(plain, isnl));

		if (special)
			break;

		unsigned nlm = (unsigned)_mm256_movemask_epi8(isnl);

		if (!nlm) {
			lp += 32;
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
			lp += 32 - prev;
		}
		consumed += 32;
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
			break; /* special: oracle window */
		}
		consumed++;
	}
	*linepos = lp;
	*maxlen = ml;
	return consumed;
}

#else
typedef int tal_simd_avx2_unused; /* ISO C forbids an empty translation unit */
#endif
