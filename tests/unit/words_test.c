#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "count.h"
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

/* Mixed-content fill: ASCII words, POSIX blanks, NBSP, U+3000, CJK, lone
 * continuations/leads (invalid), controls — every semantic class at once. */
static size_t fill_mixed(unsigned char *p, size_t cap)
{
	static const char *const frag[] = {
		"word", " ", "\t", "\n", "\xc2\xa0",         /* NBSP */
		"\xe3\x80\x80",                              /* U+3000 */
		"\xe4\xb8\xad",                              /* CJK */
		"\xd0\x96",                                  /* Cyrillic */
		"\xff", "\xc2", "\xb8", "\x01", "ab cd", "\v\f\r",
		"\xe2\x80\x99",                              /* U+2019 quote */
		"\xe2\x80\xa9",                              /* U+2029 sep */
	};
	size_t n = 0;

	while (n + 8 < cap) {
		const char *f = frag[next64() % 16];
		size_t l = strlen(f);

		memcpy(p + n, f, l);
		n += l;
	}
	return n;
}

/* Byte-semantic reference for the kernels. The kernel contract: separator
 * iff the byte is in tal_ws.kernel_ws (the derived byte SET — never raw
 * is_ws[], which macOS pollutes at 0xA0 in UTF-8 locales) or covered by an
 * L1 pattern match; hold back the tail per the kernels' rule. Matches are
 * evaluated at EVERY position independently (overlap-ORed), exactly like
 * the shifted-compare vector engine. */
static size_t ref_lwc(const unsigned char *p, size_t n, unsigned prev_is_ws,
		      struct lwc_out *out)
{
	unsigned long long lines = 0, words = 0;
	unsigned last_nonws = !prev_is_ws;
	size_t k = n;
	size_t cover = 0;

	if (tal_ws.nmbws)
		while (k > 0 && (tal_ws.suspect[p[k - 1]] ||
				 (k > 1 && tal_ws.is3lead[p[k - 2]])))
			k--;

	for (size_t i = 0; i < k; i++) {
		unsigned char b = p[i];
		unsigned is_sep;
		int m = tal_ws.nmbws ? tal_mbws_match(p + i, k - i) : 0;

		if (m && (size_t)m > cover)
			cover = (size_t)m;
		if (cover) {
			is_sep = 1;
			cover--;
		} else {
			is_sep = tal_ws.kernel_ws[b];
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

static void check_kernel_eq(const char *name,
			    size_t (*fn)(const unsigned char *, size_t,
					 unsigned, struct lwc_out *),
			    const unsigned char *base, size_t len)
{
	static const size_t sizes[] = { 0,  1,  2,  3,  15,  16,  17,   31,
					32, 33, 34, 63, 64,  127, 255,  257,
					4095, 4096, 8191, 8192 };
	char msg[128];

	for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
		size_t n = sizes[s] <= len ? sizes[s] : len;

		for (size_t off = 0; off < 8; off += 7) {
			for (unsigned pw = 0; pw < 2; pw++) {
				struct lwc_out a, b;
				size_t ka = fn(base + off, n, pw, &a);
				size_t kb = ref_lwc(base + off, n, pw, &b);

				snprintf(msg, sizeof msg,
					 "%s n=%zu off=%zu pw=%u", name, n,
					 off, pw);
				CHECK(msg, ka == kb);
				if (ka == kb &&
				    (a.lines != b.lines || a.words != b.words ||
				     a.last_is_ws != b.last_is_ws)) {
					fprintf(stderr,
						"  FAIL %s: kernel l=%llu w=%llu e=%u ref l=%llu w=%llu e=%u\n",
						msg, a.lines, a.words,
						a.last_is_ws, b.lines, b.words,
						b.last_is_ws);
					t_checks++;
					t_fails++;
				} else if (ka == kb) {
					CHECK(msg, 1);
				}
			}
		}
	}
}

/* Oracle chunk invariance: identical counts no matter where the stream is
 * split (the fragmentation-invariance property, unit-scale). */
static void check_oracle_splits(const unsigned char *p, size_t n)
{
	struct counts whole, split;
	struct wstate st;

	memset(&whole, 0, sizeof whole);
	wstate_init(&st);
	tal_swc_mb(p, n, &whole, &st);
	tal_swc_finish(&whole, &st);

	for (size_t cut = 1; cut < n && cut < 300; cut++) {
		memset(&split, 0, sizeof split);
		wstate_init(&st);
		tal_swc_mb(p, cut, &split, &st);
		tal_swc_mb(p + cut, n - cut, &split, &st);
		tal_swc_finish(&split, &st);
		if (split.words != whole.words ||
		    split.lines != whole.lines) {
			char msg[64];

			snprintf(msg, sizeof msg, "split@%zu", cut);
			CHECK(msg, 0);
			return;
		}
	}
	CHECK("oracle split-invariant", 1);
}

int main(void)
{
	unsigned char *base = malloc(8192 + 8);

	CHECK("alloc", base != 0);
	if (!base)
		return test_summary("words_test");

	setlocale(LC_ALL, "");
	ws_init(&tal_ws);

	/* ASCII sanity, hardcoded counts (safe cross-libc). */
	{
		struct counts c;
		struct wstate st;

		memset(&c, 0, sizeof c);
		wstate_init(&st);
		tal_swc_sb((const unsigned char *)"a bb  c\n\x01\x01 d", 13,
			   &c, &st);
		CHECK_SZ("ascii words", (size_t)c.words, 5);
		CHECK_SZ("ascii lines", (size_t)c.lines, 1);
	}

	if (tal_ws.multibyte) {
		state = 7;
		size_t n = fill_mixed(base, 300);

		check_oracle_splits(base, n);
	}

	/* Kernels vs the byte-semantic reference, both locales. */
	const char *locs[] = { "", "C" };

	for (int li = 0; li < 2; li++) {
		if (!setlocale(LC_ALL, locs[li]))
			continue;
		ws_init(&tal_ws);
		fprintf(stderr, "  locale '%s': mb=%d utf8=%d luts_ok=%d ws={",
			locs[li], tal_ws.multibyte, tal_ws.utf8,
			tal_ws.luts_ok);
		for (int k = 0; k < tal_ws.n_ws_bytes; k++)
			fprintf(stderr, "%s%02x", k ? "," : "",
				tal_ws.ws_bytes[k]);
		fprintf(stderr, "} nsus=%d\n", tal_ws.n_sus_bytes);
		if (!tal_ws.luts_ok)
			continue;
		state = 42;
		(void)fill_mixed(base, 8192 + 8);
#if TAL_HAS_SSE2 && defined(__SSE2__)
		check_kernel_eq("sse2", tal_lwc_sse2, base, 8192);
#endif
#if TAL_HAS_AVX2
		if (tal_cpu_has_avx2())
			check_kernel_eq("avx2", tal_lwc_avx2, base, 8192);
#endif
#if TAL_HAS_NEON && defined(__ARM_NEON)
		check_kernel_eq("neon", tal_lwc_neon, base, 8192);
#endif
	}

	free(base);
	return test_summary("words_test");
}
