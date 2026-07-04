#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "simd.h"
#include "sys/detect.h"
#include "tests/unit/test.h"

/* Deterministic buffer fill (splitmix64, same generator family as tests/gen.c). */
static unsigned long long state;

static unsigned long long next64(void)
{
	unsigned long long z = (state += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static void fill(unsigned char *p, size_t n, int newline_every)
{
	for (size_t i = 0; i < n; i++) {
		unsigned long long r = next64();

		p[i] = (newline_every && r % (unsigned)newline_every == 0)
			       ? '\n'
			       : (unsigned char)(r & 0xFF);
	}
}

/* Sizes around vector widths, accumulator flush (255 iters), and buffer edges. */
static const size_t sizes[] = { 0,    1,    15,   16,   17,   31,   32,
				33,   63,   64,   65,   255,  4095, 4096,
				4097, 8160, 8161, 16320, 16321, 65536 };

static void check_kernel(const char *name,
			 unsigned long long (*fn)(const unsigned char *, size_t),
			 unsigned char *base)
{
	char msg[128];

	for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
		for (size_t off = 0; off < 8; off += 7) { /* aligned + odd */
			unsigned char *p = base + off;
			size_t n = sizes[s];

			snprintf(msg, sizeof msg, "%s n=%zu off=%zu", name, n,
				 off);
			CHECK_SZ(msg, (size_t)fn(p, n),
				 (size_t)tal_nlcount_scalar(p, n));
		}
	}
}

int main(void)
{
	size_t cap = 65536 + 8;
	unsigned char *base = malloc(cap);

	CHECK("alloc", base != 0);
	if (!base)
		return test_summary("nlcount_test");

	/* Three fills: newline-dense, sparse, none. Each kernel must equal the
	 * scalar oracle on every (fill, size, alignment) cell. */
	int densities[] = { 3, 97, 0 };

	for (int d = 0; d < 3; d++) {
		state = 42;
		fill(base, cap, densities[d]);
#if TAL_HAS_SSE2 && defined(__SSE2__)
		check_kernel("sse2", tal_nlcount_sse2, base);
#endif
#if TAL_HAS_AVX2
		if (tal_cpu_has_avx2())
			check_kernel("avx2", tal_nlcount_avx2, base);
#endif
#if TAL_HAS_NEON && defined(__ARM_NEON)
		check_kernel("neon", tal_nlcount_neon, base);
#endif
	}

	/* All-newlines exercises the u8 accumulator saturation boundary. */
	memset(base, '\n', cap);
#if TAL_HAS_SSE2 && defined(__SSE2__)
	check_kernel("sse2 all-nl", tal_nlcount_sse2, base);
#endif
#if TAL_HAS_AVX2
	if (tal_cpu_has_avx2())
		check_kernel("avx2 all-nl", tal_nlcount_avx2, base);
#endif
#if TAL_HAS_NEON && defined(__ARM_NEON)
	check_kernel("neon all-nl", tal_nlcount_neon, base);
#endif
	CHECK_SZ("scalar all-nl", (size_t)tal_nlcount_scalar(base, 65536),
		 (size_t)65536);

	free(base);
	return test_summary("nlcount_test");
}
