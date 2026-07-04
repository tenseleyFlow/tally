#include "config.h"

/* NEON is unconditional on aarch64. Same shape as coreutils wc_neon.c
 * (refs/coreutils/src/wc_neon.c) and our x86 kernels: 4 independent u8
 * accumulators, 64 B per iteration, widening-pairwise flush per block
 * (4*block < 256 for the all-newline worst case). */
#if TAL_HAS_NEON && defined(__ARM_NEON)

#include <arm_neon.h>

#include "simd.h"
#include "ws.h"

static inline unsigned long long hsum(uint8x16_t acc)
{
	uint64x2_t s = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(acc)));

	return vgetq_lane_u64(s, 0) + vgetq_lane_u64(s, 1);
}

unsigned long long tal_nlcount_neon(const unsigned char *p, size_t n)
{
	const uint8x16_t nl = vdupq_n_u8('\n');
	unsigned long long lines = 0;

	while (n >= 64) {
		size_t block = n / 64;
		uint8x16_t a0 = vdupq_n_u8(0);
		uint8x16_t a1 = vdupq_n_u8(0);
		uint8x16_t a2 = vdupq_n_u8(0);
		uint8x16_t a3 = vdupq_n_u8(0);

		if (block > 63)
			block = 63;
		n -= block * 64;
		do {
			a0 = vsubq_u8(a0, vceqq_u8(vld1q_u8(p), nl));
			a1 = vsubq_u8(a1, vceqq_u8(vld1q_u8(p + 16), nl));
			a2 = vsubq_u8(a2, vceqq_u8(vld1q_u8(p + 32), nl));
			a3 = vsubq_u8(a3, vceqq_u8(vld1q_u8(p + 48), nl));
			p += 64;
		} while (--block);
		lines += hsum(vaddq_u8(vaddq_u8(a0, a1), vaddq_u8(a2, a3)));
	}

	if (n >= 16) {
		uint8x16_t acc = vdupq_n_u8(0);

		do {
			acc = vsubq_u8(acc, vceqq_u8(vld1q_u8(p), nl));
			p += 16;
			n -= 16;
		} while (n >= 16); /* < 4 iterations: no overflow risk */
		lines += hsum(acc);
	}
	for (size_t i = 0; i < n; i++)
		lines += p[i] == '\n';
	return lines;
}

/* Fused lines+words kernel. vqtbl1q_u8 accepts any index (out-of-range
 * yields 0), so the Mula nibble LUTs work directly. Same carry scheme as the
 * x86 kernels; vext replaces alignr for the previous-byte vector. */

static inline uint8x16_t classify_and(uint8x16_t v, uint8x16_t lut_lo,
				      uint8x16_t lut_hi)
{
	uint8x16_t lo = vqtbl1q_u8(lut_lo, vandq_u8(v, vdupq_n_u8(0x0F)));
	uint8x16_t hi = vqtbl1q_u8(lut_hi, vshrq_n_u8(v, 4));

	return vandq_u8(lo, hi);
}

size_t tal_lwc_neon(const unsigned char *p, size_t n, unsigned prev_is_ws,
		    struct lwc_out *out)
{
	const uint8x16_t nl = vdupq_n_u8('\n');
	const uint8x16_t ws_lo = vld1q_u8(tal_ws.ws_lut_lo);
	const uint8x16_t ws_hi = vld1q_u8(tal_ws.ws_lut_hi);
	const uint8x16_t sus_lo = vld1q_u8(tal_ws.sus_lut_lo);
	const uint8x16_t sus_hi = vld1q_u8(tal_ws.sus_lut_hi);
	uint8x16_t glead[8], gsecond[8], glo[8], ghi[8];
	int glen[8];
	unsigned long long lines = 0, words = 0;
	uint8x16_t prevv = vdupq_n_u8(prev_is_ws ? 0 : 0xFF);
	uint8x16_t prev_sep = vdupq_n_u8(0);
	uint8x16_t prev_m3 = vdupq_n_u8(0);
	bool prev_dirty = false;
	unsigned last_nonws;
	int ng = tal_ws.nmbws ? tal_ws.ngroups : 0;
	size_t k = n;

	for (int g = 0; g < ng; g++) {
		const struct mbws_group *s = &tal_ws.groups[g];

		glead[g] = vdupq_n_u8(s->lead);
		gsecond[g] = vdupq_n_u8(s->second);
		glo[g] = vld1q_u8(s->set_lo);
		ghi[g] = vld1q_u8(s->set_hi);
		glen[g] = s->len;
	}
	if (ng)
		while (k > 0 && (tal_ws.suspect[p[k - 1]] ||
				 (k > 1 && tal_ws.is3lead[p[k - 2]])))
			k--;

	size_t rem = k;

	while (rem >= 18) {
		size_t block = (rem - 2) / 16;
		uint8x16_t lacc = vdupq_n_u8(0);
		uint8x16_t wacc = vdupq_n_u8(0);

		if (block > 255)
			block = 255;
		rem -= block * 16;
		do {
			uint8x16_t v = vld1q_u8(p);
			uint8x16_t nonws =
				vceqq_u8(classify_and(v, ws_lo, ws_hi),
					 vdupq_n_u8(0));

			if (ng) {
				uint8x16_t sep = vdupq_n_u8(0);
				uint8x16_t m3 = vdupq_n_u8(0);

				if (vmaxvq_u8(classify_and(v, sus_lo, sus_hi)) ||
				    prev_dirty) {
					uint8x16_t v1 = vld1q_u8(p + 1);
					uint8x16_t v2 = vld1q_u8(p + 2);

					for (int g = 0; g < ng; g++) {
						uint8x16_t hit =
							vceqq_u8(v, glead[g]);

						if (glen[g] == 2) {
							hit = vandq_u8(
								hit,
								vmvnq_u8(vceqq_u8(
									classify_and(
										v1,
										glo[g],
										ghi[g]),
									vdupq_n_u8(0))));
						} else {
							hit = vandq_u8(
								hit,
								vceqq_u8(v1,
									 gsecond[g]));
							hit = vandq_u8(
								hit,
								vmvnq_u8(vceqq_u8(
									classify_and(
										v2,
										glo[g],
										ghi[g]),
									vdupq_n_u8(0))));
							m3 = vorrq_u8(m3, hit);
						}
						sep = vorrq_u8(sep, hit);
					}
					uint8x16_t mask = vorrq_u8(
						sep,
						vorrq_u8(
							vextq_u8(prev_sep, sep, 15),
							vextq_u8(prev_m3, m3, 14)));

					nonws = vbicq_u8(nonws, mask);
					prev_dirty = vmaxvq_u8(sep) != 0;
				}
				prev_sep = sep;
				prev_m3 = m3;
			}

			uint8x16_t shifted = vextq_u8(prevv, nonws, 15);
			uint8x16_t starts = vbicq_u8(nonws, shifted);

			wacc = vsubq_u8(wacc, starts);
			lacc = vsubq_u8(lacc, vceqq_u8(v, nl));
			prevv = nonws;
			p += 16;
		} while (--block);
		lines += hsum(lacc);
		words += hsum(wacc);
	}
	last_nonws = vgetq_lane_u8(prevv, 15) & 1u;

	size_t cover = 0;

	if (vgetq_lane_u8(prev_m3, 15))
		cover = 2;
	else if (vgetq_lane_u8(prev_sep, 15) || vgetq_lane_u8(prev_m3, 14))
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

/* Validated -m kernel; mirrors simd_avx2.c's structural validator (see the
 * comment there): continuation mask must equal the shifted-lead expectation,
 * C0/C1/F5-FF always invalid, four second-byte specials, seq-complete spans. */

static inline uint8x16_t nrange(uint8x16_t v, unsigned char lo,
				unsigned char hi)
{
	return vcleq_u8(vsubq_u8(v, vdupq_n_u8(lo)), vdupq_n_u8((unsigned char)(hi - lo)));
}

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

size_t tal_u8count_neon(const unsigned char *p, size_t n,
			unsigned long long *chars, unsigned long long *lines)
{
	const uint8x16_t nlv = vdupq_n_u8('\n');
	const uint8x16_t zero = vdupq_n_u8(0);
	size_t consumed = 0;

	while (consumed < n) {
		size_t span = n - consumed < 8192 ? n - consumed : 8192;

		span = u8_seq_hold(p + consumed, span);
		if (span == 0)
			break;

		const unsigned char *q = p + consumed;
		size_t rem = span;
		unsigned long long ch = 0, nl = 0;
		uint8x16_t cacc = zero, lacc = zero, err = zero;
		uint8x16_t pl234 = zero, pl34 = zero, pl4 = zero;
		int iters = 0;
		bool ok = true;

		while (rem >= 17) {
			uint8x16_t v = vld1q_u8(q);
			uint8x16_t v1 = vld1q_u8(q + 1);
			uint8x16_t cont = vceqq_u8(
				vandq_u8(v, vdupq_n_u8(0xC0)),
				vdupq_n_u8(0x80));
			uint8x16_t l2 = nrange(v, 0xC2, 0xDF);
			uint8x16_t l3 = nrange(v, 0xE0, 0xEF);
			uint8x16_t l4 = nrange(v, 0xF0, 0xF4);
			uint8x16_t bad = vorrq_u8(
				nrange(v, 0xC0, 0xC1),
				vcgtq_u8(v, vdupq_n_u8(0xF4)));
			uint8x16_t sp = vandq_u8(
				vceqq_u8(v, vdupq_n_u8(0xE0)),
				vcleq_u8(v1, vdupq_n_u8(0x9F)));

			sp = vorrq_u8(sp, vandq_u8(
				vceqq_u8(v, vdupq_n_u8(0xED)),
				vcgtq_u8(v1, vdupq_n_u8(0x9F))));
			sp = vorrq_u8(sp, vandq_u8(
				vceqq_u8(v, vdupq_n_u8(0xF0)),
				vcleq_u8(v1, vdupq_n_u8(0x8F))));
			sp = vorrq_u8(sp, vandq_u8(
				vceqq_u8(v, vdupq_n_u8(0xF4)),
				vcgtq_u8(v1, vdupq_n_u8(0x8F))));

			uint8x16_t l234 = vorrq_u8(l2, vorrq_u8(l3, l4));
			uint8x16_t l34 = vorrq_u8(l3, l4);
			uint8x16_t expect = vorrq_u8(
				vextq_u8(pl234, l234, 15),
				vorrq_u8(vextq_u8(pl34, l34, 14),
					 vextq_u8(pl4, l4, 13)));

			err = vorrq_u8(err, vorrq_u8(bad, sp));
			err = vorrq_u8(err, veorq_u8(cont, expect));
			cacc = vsubq_u8(cacc, cont);
			lacc = vsubq_u8(lacc, vceqq_u8(v, nlv));
			pl234 = l234;
			pl34 = l34;
			pl4 = l4;
			ch += 16;
			q += 16;
			rem -= 16;
			if (++iters == 255 || rem < 17) {
				if (vmaxvq_u8(err)) {
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

size_t tal_lscan_neon(const unsigned char *p, size_t n,
		      unsigned long long *linepos, unsigned long long *maxlen)
{
	const uint8x16_t nlv = vdupq_n_u8('\n');
	unsigned long long lp = *linepos, ml = *maxlen;
	size_t consumed = 0;

	while (n - consumed >= 16) {
		uint8x16_t v = vld1q_u8(p + consumed);
		uint8x16_t isnl = vceqq_u8(v, nlv);
		uint8x16_t plain = nrange(v, 0x20, 0x7E);

		if (vminvq_u8(vorrq_u8(plain, isnl)) == 0)
			break; /* some byte is neither plain nor newline */

		if (vmaxvq_u8(isnl) == 0) {
			lp += 16;
		} else {
			for (unsigned j = 0; j < 16; j++) {
				if (p[consumed + j] == '\n') {
					if (lp > ml)
						ml = lp;
					lp = 0;
				} else {
					lp++;
				}
			}
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
typedef int tal_simd_neon_unused; /* ISO C forbids an empty translation unit */
#endif
