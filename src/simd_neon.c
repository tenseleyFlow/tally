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

bool tal_lwc_neon(const unsigned char *p, size_t n, unsigned prev_is_ws,
		  struct lwc_out *out, bool scan_suspect)
{
	const uint8x16_t nl = vdupq_n_u8('\n');
	const uint8x16_t ws_lo = vld1q_u8(tal_ws.ws_lut_lo);
	const uint8x16_t ws_hi = vld1q_u8(tal_ws.ws_lut_hi);
	const uint8x16_t sus_lo = vld1q_u8(tal_ws.sus_lut_lo);
	const uint8x16_t sus_hi = vld1q_u8(tal_ws.sus_lut_hi);
	unsigned long long lines = 0, words = 0;
	uint8x16_t prevv = vdupq_n_u8(prev_is_ws ? 0 : 0xFF);
	unsigned last_nonws;

	while (n >= 16) {
		size_t block = n / 16;
		uint8x16_t lacc = vdupq_n_u8(0);
		uint8x16_t wacc = vdupq_n_u8(0);

		if (block > 255)
			block = 255;
		n -= block * 16;
		do {
			uint8x16_t v = vld1q_u8(p);

			if (scan_suspect &&
			    vmaxvq_u8(classify_and(v, sus_lo, sus_hi)))
				return false;

			uint8x16_t nonws =
				vceqq_u8(classify_and(v, ws_lo, ws_hi),
					 vdupq_n_u8(0));
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
typedef int tal_simd_neon_unused; /* ISO C forbids an empty translation unit */
#endif
