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

/* -m kernel: counts VALID SEQUENCE STARTS. Under wc's resync semantics
 * (valid sequence consumes its length, anything else consumes one byte) a
 * valid start can never sit inside another valid sequence -- interiors are
 * continuation bytes, and no continuation is a valid lead. So chars = the
 * number of positions where a structurally valid sequence begins, and lines
 * = the number of 0x0A bytes (multibyte encodings of U+000A are overlong,
 * hence invalid): both position-independent, both vectorizable, NO span
 * rejection. This replaced the whole-span validator that fell back to a
 * scalar walker on any invalid byte and held random binary to ~2x (P3).
 *
 * Validity at position i, from RFC 3629 (same table as u8dec):
 *   00-7F                        1 byte
 *   C2-DF + cont                 2 bytes
 *   E0-EF + second + cont        second: E0 >= A0, ED <= 9F, else cont
 *   F0-F4 + second + cont + cont second: F0 >= 90, F4 <= 8F, else cont
 * Everything else (stray continuations, C0/C1, F5-FF, bad followers) is not
 * a start. Needs 3 bytes of lookahead: the caller's scalar path finishes the
 * unconsumed <= 34-byte tail (and carries chunk-boundary prefixes). */

size_t tal_u8count_avx2(const unsigned char *p, size_t n,
			unsigned long long *chars, unsigned long long *lines)
{
	const __m256i zero = _mm256_setzero_si256();
	const __m256i nlv = _mm256_set1_epi8('\n');
	const __m256i contmask = _mm256_set1_epi8((char)0xC0);
	const __m256i contbits = _mm256_set1_epi8((char)0x80);
	const __m256i lo3d = _mm256_set1_epi8((char)0x80);
	const __m256i lo3e0 = _mm256_set1_epi8((char)0xA0);
	const __m256i hi3d = _mm256_set1_epi8((char)0xBF);
	const __m256i hi3ed = _mm256_set1_epi8((char)0x9F);
	const __m256i lo4f0 = _mm256_set1_epi8((char)0x90);
	const __m256i hi4f4 = _mm256_set1_epi8((char)0x8F);
	unsigned long long ch = 0, nl = 0;
	size_t rem = n;

	while (rem >= 35) {
		size_t block = (rem - 3) / 32;
		__m256i cacc = zero, lacc = zero;

		if (block > 255)
			block = 255; /* u8 lanes: <= 1 start per lane per iter */
		rem -= block * 32;
		do {
			__m256i v = _mm256_loadu_si256(
				(const __m256i *)(const void *)p);
			__m256i v1 = _mm256_loadu_si256(
				(const __m256i *)(const void *)(p + 1));
			__m256i v2 = _mm256_loadu_si256(
				(const __m256i *)(const void *)(p + 2));
			__m256i v3 = _mm256_loadu_si256(
				(const __m256i *)(const void *)(p + 3));
			__m256i cont1 = _mm256_cmpeq_epi8(
				_mm256_and_si256(v1, contmask), contbits);
			__m256i cont2 = _mm256_cmpeq_epi8(
				_mm256_and_si256(v2, contmask), contbits);
			__m256i cont3 = _mm256_cmpeq_epi8(
				_mm256_and_si256(v3, contmask), contbits);
			__m256i ascii = _mm256_cmpeq_epi8(
				_mm256_and_si256(v, contbits), zero);
			__m256i ok2 = _mm256_and_si256(range_in(v, 0xC2, 0xDF),
						       cont1);
			/* second-byte bounds tighten for E0/ED (F0/F4). */
			__m256i lo3 = _mm256_blendv_epi8(
				lo3d, lo3e0,
				_mm256_cmpeq_epi8(v, _mm256_set1_epi8((char)0xE0)));
			__m256i hi3 = _mm256_blendv_epi8(
				hi3d, hi3ed,
				_mm256_cmpeq_epi8(v, _mm256_set1_epi8((char)0xED)));
			__m256i sec3 = _mm256_and_si256(
				_mm256_cmpeq_epi8(_mm256_max_epu8(v1, lo3), v1),
				_mm256_cmpeq_epi8(_mm256_min_epu8(v1, hi3), v1));
			__m256i ok3 = _mm256_and_si256(
				_mm256_and_si256(range_in(v, 0xE0, 0xEF), sec3),
				cont2);
			__m256i lo4 = _mm256_blendv_epi8(
				lo3d, lo4f0,
				_mm256_cmpeq_epi8(v, _mm256_set1_epi8((char)0xF0)));
			__m256i hi4 = _mm256_blendv_epi8(
				hi3d, hi4f4,
				_mm256_cmpeq_epi8(v, _mm256_set1_epi8((char)0xF4)));
			__m256i sec4 = _mm256_and_si256(
				_mm256_cmpeq_epi8(_mm256_max_epu8(v1, lo4), v1),
				_mm256_cmpeq_epi8(_mm256_min_epu8(v1, hi4), v1));
			__m256i ok4 = _mm256_and_si256(
				_mm256_and_si256(range_in(v, 0xF0, 0xF4), sec4),
				_mm256_and_si256(cont2, cont3));
			__m256i start = _mm256_or_si256(
				_mm256_or_si256(ascii, ok2),
				_mm256_or_si256(ok3, ok4));

			cacc = _mm256_sub_epi8(cacc, start);
			lacc = _mm256_sub_epi8(lacc,
					       _mm256_cmpeq_epi8(v, nlv));
			p += 32;
		} while (--block);
		ch += hsum(cacc);
		nl += hsum(lacc);
	}
	*chars += ch;
	*lines += nl;
	return n - rem;
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
