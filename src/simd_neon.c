#include "config.h"

/* NEON is unconditional on aarch64. Pattern follows coreutils wc_neon.c
 * (refs/coreutils/src/wc_neon.c): u8 lane accumulators, widening-pairwise
 * reduction before overflow. */
#if TAL_HAS_NEON && defined(__ARM_NEON)

#include <arm_neon.h>

#include "simd.h"

unsigned long long tal_nlcount_neon(const unsigned char *p, size_t n)
{
	const uint8x16_t nl = vdupq_n_u8('\n');
	unsigned long long lines = 0;
	uint8x16_t acc = vdupq_n_u8(0);
	int iters = 0;

	while (n >= 16) {
		uint8x16_t v = vld1q_u8(p);

		/* vceqq lanes are 0xFF; subtracting increments u8 counters. */
		acc = vsubq_u8(acc, vceqq_u8(v, nl));
		p += 16;
		n -= 16;
		if (++iters == 255 || n < 16) {
			uint64x2_t sums =
				vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(acc)));

			lines += vgetq_lane_u64(sums, 0) +
				 vgetq_lane_u64(sums, 1);
			acc = vdupq_n_u8(0);
			iters = 0;
		}
	}
	for (size_t i = 0; i < n; i++)
		lines += p[i] == '\n';
	return lines;
}

#else
typedef int tal_simd_neon_unused; /* ISO C forbids an empty translation unit */
#endif
