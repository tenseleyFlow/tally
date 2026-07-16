#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "simd.h"
#include "sys/detect.h"
#include "ws.h"
#include "tests/unit/test.h"

static unsigned long long state;

static unsigned long long next64(void)
{
	unsigned long long z = (state += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

/* Valid UTF-8 of every length class, plus newlines. */
static size_t fill_valid(unsigned char *p, size_t cap)
{
	static const char *const frag[] = {
		"ascii ",  "\n",
		"\xc2\xa2",         /* U+00A2, 2-byte */
		"\xdf\xbf",         /* U+07FF, 2-byte max */
		"\xe0\xa0\x80",     /* U+0800, E0 lower bound */
		"\xed\x9f\xbf",     /* U+D7FF, ED upper bound */
		"\xe4\xb8\xad",     /* CJK */
		"\xef\xbf\xbd",     /* U+FFFD */
		"\xf0\x90\x80\x80", /* U+10000, F0 lower bound */
		"\xf0\x9f\x98\x80", /* emoji */
		"\xf4\x8f\xbf\xbf", /* U+10FFFF, max */
	};
	size_t n = 0;

	while (n + 8 < cap) {
		const char *f = frag[next64() % 11];
		size_t l = strlen(f);

		memcpy(p + n, f, l);
		n += l;
	}
	return n;
}

static const char *const badshape[] = {
	"\x80",             /* stray continuation */
	"\xc0\xaf",         /* overlong 2 */
	"\xc1\x81",         /* overlong 2 */
	"\xe0\x80\x80",     /* overlong 3 */
	"\xe0\x9f\xbf",     /* overlong 3 upper */
	"\xed\xa0\x80",     /* surrogate low bound */
	"\xed\xbf\xbf",     /* surrogate high bound */
	"\xf0\x8f\xbf\xbf", /* overlong 4 */
	"\xf4\x90\x80\x80", /* > U+10FFFF */
	"\xf5\x80",         /* invalid lead */
	"\xff",             /* invalid lead */
	"\xc2\x41",         /* missing continuation */
	"\xe4\xb8",         /* truncated (mid-buffer, next is ascii) */
};

/* Resync-count oracle over [p, p+n): chars = valid sequence starts, lines =
 * 0x0A bytes; an incomplete pend at EOF counts nothing (error bytes). */
static void resync_count(const unsigned char *p, size_t n,
			 unsigned long long *ch, unsigned long long *nl)
{
	struct counts c;
	struct wstate st;

	memset(&c, 0, sizeof c);
	wstate_init(&st);
	tal_u8scalar(p, n, &c, &st);
	*ch = c.chars;
	*nl = c.lines;
}

static void check_u8(const char *name,
		     size_t (*fn)(const unsigned char *, size_t,
				  unsigned long long *, unsigned long long *))
{
	unsigned char *b = malloc(65536 + 16);
	char msg[96];

	CHECK("alloc", b != 0);
	if (!b)
		return;

	/* Valid corpus at awkward sizes and offsets. The kernel consumes all
	 * but a bounded lookahead tail, and kernel(prefix) + scalar(suffix)
	 * must equal the scalar count of the whole slice -- exactly how
	 * chars_chunk composes them. Offset 3 starts mid-character, which the
	 * start-counting semantics absorb without special cases. */
	state = 99;
	size_t n = fill_valid(b, 65536);
	static const size_t sizes[] = { 0, 1, 5, 16, 17, 33, 40, 4095, 4097,
					8191, 8192, 8193, 16384, 65536 };

	for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
		size_t len = sizes[s] < n ? sizes[s] : n;

		for (size_t off = 0; off < 4; off += 3) {
			unsigned long long kc = 0, kl = 0, oc, ol, sc, sl;
			size_t used = fn(b + off, len, &kc, &kl);

			snprintf(msg, sizeof msg, "%s valid n=%zu off=%zu",
				 name, len, off);
			CHECK(msg, used <= len && len - used < 35);
			resync_count(b + off + used, len - used, &sc, &sl);
			resync_count(b + off, len, &oc, &ol);
			CHECK(msg, kc + sc == oc && kl + sl == ol);
		}
	}

	/* Each invalid shape, injected mid-corpus: no rejection anymore -- the
	 * kernel counts straight through and composition must still hold. */
	for (size_t k = 0; k < sizeof badshape / sizeof badshape[0]; k++) {
		size_t pos = 1000 + 17 * k;
		size_t blen = strlen(badshape[k]);
		unsigned char save[8];

		memcpy(save, b + pos, blen);
		memcpy(b + pos, badshape[k], blen);

		unsigned long long kc = 0, kl = 0, oc, ol, sc, sl;
		size_t used = fn(b, 20000, &kc, &kl);

		snprintf(msg, sizeof msg, "%s bad[%zu]", name, k);
		CHECK(msg, used <= 20000 && 20000 - used < 35);
		resync_count(b + used, 20000 - used, &sc, &sl);
		resync_count(b, 20000, &oc, &ol);
		CHECK(msg, kc + sc == oc && kl + sl == ol);
		memcpy(b + pos, save, blen);
	}

	/* Random binary, the P3 workload: dense invalid bytes, all consumed. */
	for (size_t i = 0; i < 65536; i += 8) {
		unsigned long long r = next64();

		memcpy(b + i, &r, 8);
	}
	for (int t = 0; t < 8; t++) {
		size_t len = 4096 + (size_t)(next64() % 60000);
		unsigned long long kc = 0, kl = 0, oc, ol, sc, sl;
		size_t used = fn(b, len, &kc, &kl);

		snprintf(msg, sizeof msg, "%s binary n=%zu", name, len);
		CHECK(msg, used <= len && len - used < 35);
		resync_count(b + used, len - used, &sc, &sl);
		resync_count(b, len, &oc, &ol);
		CHECK(msg, kc + sc == oc && kl + sl == ol);
	}
	free(b);
}

int main(void)
{
	setlocale(LC_ALL, "");
	ws_init(&tal_ws);

	/* Walker sanity against hardcoded expectations. */
	unsigned long long ch = 0, nl = 0;

	CHECK("walk ascii", tal_u8walk((const unsigned char *)"ab\nc", 4, &ch,
				       &nl) == 0 && ch == 4 && nl == 1);
	ch = nl = 0;
	CHECK("walk cjk", tal_u8walk((const unsigned char *)"\xe4\xb8\xad", 3,
				     &ch, &nl) == 0 && ch == 1);
	for (size_t k = 0; k < sizeof badshape / sizeof badshape[0]; k++) {
		unsigned char tmp[16];
		size_t l = strlen(badshape[k]);

		memcpy(tmp, badshape[k], l);
		tmp[l] = 'x'; /* truncation case needs a non-cont follower */
		ch = nl = 0;
		CHECK("walk rejects", tal_u8walk(tmp, l + 1, &ch, &nl) == -1);
	}

#if TAL_HAS_SSE2 && defined(__SSE2__)
	check_u8("sse2", tal_u8count_sse2);
#endif
#if TAL_HAS_AVX2
	if (tal_cpu_has_avx2())
		check_u8("avx2", tal_u8count_avx2);
#endif
#if TAL_HAS_NEON && defined(__ARM_NEON)
	check_u8("neon", tal_u8count_neon);
#endif
	return test_summary("u8count_test");
}
