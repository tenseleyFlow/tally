#include "config.h"

/* NEON is unconditional on aarch64. Same shape as coreutils wc_neon.c
 * (refs/coreutils/src/wc_neon.c) and our x86 kernels: 4 independent u8
 * accumulators, 64 B per iteration, widening-pairwise flush per block
 * (4*block < 256 for the all-newline worst case). */
#if TAL_HAS_NEON && defined(__ARM_NEON)

#include <arm_neon.h>

#include "simd.h"

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

#else
typedef int tal_simd_neon_unused; /* ISO C forbids an empty translation unit */
#endif
