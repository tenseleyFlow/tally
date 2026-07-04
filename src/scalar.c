#include <string.h>
#include <wchar.h>

#include "simd.h"
#include "ws.h"

/* Parity oracles and last-resort fallbacks. Every SIMD kernel must equal
 * these functions on every input (unit + fuzz enforced). */

unsigned long long tal_nlcount_scalar(const unsigned char *p, size_t n)
{
	unsigned long long lines = 0;

	for (size_t i = 0; i < n; i++)
		lines += p[i] == '\n';
	return lines;
}

void wstate_init(struct wstate *st)
{
	memset(st, 0, sizeof *st);
}

/* RFC 3629 decode at p[0] with n bytes visible: returns the sequence length
 * (1-4) with *cp set, 0 for a valid-but-incomplete prefix (chunk boundary),
 * -1 for invalid (overlongs, surrogates, > U+10FFFF, stray or missing
 * continuations). glibc, FreeBSD and musl decoders agree with this exactly
 * (fuzzed against the ref per box) — the single decode truth for the oracle
 * and the strict walker. */
static int u8dec(const unsigned char *p, size_t n, unsigned long *cp)
{
	unsigned char b = p[0];

	if (b < 0x80) {
		*cp = b;
		return 1;
	}

	size_t need;
	unsigned char lo = 0x80, hi = 0xBF;
	unsigned long v;

	if (b >= 0xC2 && b <= 0xDF) {
		need = 1;
		v = b & 0x1Fu;
	} else if (b >= 0xE0 && b <= 0xEF) {
		need = 2;
		v = b & 0x0Fu;
		if (b == 0xE0)
			lo = 0xA0; /* overlong-3 */
		else if (b == 0xED)
			hi = 0x9F; /* surrogates */
	} else if (b >= 0xF0 && b <= 0xF4) {
		need = 3;
		v = b & 0x07u;
		if (b == 0xF0)
			lo = 0x90; /* overlong-4 */
		else if (b == 0xF4)
			hi = 0x8F; /* > U+10FFFF */
	} else {
		return -1; /* stray continuation, C0/C1, F5-FF */
	}
	size_t have = n - 1 < need ? n - 1 : need;

	if (have >= 1) {
		if (p[1] < lo || p[1] > hi)
			return -1;
		v = (v << 6) | (p[1] & 0x3Fu);
	}
	for (size_t j = 2; j <= have; j++) {
		if ((p[j] & 0xC0) != 0x80)
			return -1;
		v = (v << 6) | (p[j] & 0x3Fu);
	}
	if (have < need)
		return 0; /* incomplete prefix */
	*cp = v;
	return (int)need + 1;
}

/* Strict UTF-8 walk: counts chars (at valid starts) and newlines; rejects on
 * the first invalid OR truncated sequence. */
int tal_u8walk(const unsigned char *p, size_t n, unsigned long long *chars,
	       unsigned long long *lines)
{
	unsigned long long ch = 0, nl = 0;
	size_t i = 0;

	while (i < n) {
		unsigned long cp;
		int r = u8dec(p + i, n - i, &cp);

		if (r <= 0)
			return -1; /* invalid, or truncated at end */
		nl += cp == '\n';
		ch++;
		i += (size_t)r;
	}
	*chars += ch;
	*lines += nl;
	return 0;
}

/* The wc.c per-character switch (audit 01 table), byte form. Width math runs
 * only when st->width (-L requested), matching GNU's c32width gating. */
static void classify_byte(unsigned char b, struct counts *c,
			  struct wstate *st, bool *in_word)
{
	switch (b) {
	case '\n':
		c->lines++;
		/* fall through */
	case '\r':
	case '\f':
		if (st->linepos > c->linelength)
			c->linelength = st->linepos;
		st->linepos = 0;
		*in_word = false;
		break;
	case '\t':
		st->linepos += 8 - st->linepos % 8;
		*in_word = false;
		break;
	case ' ':
		st->linepos++;
		/* fall through */
	case '\v':
		*in_word = false;
		break;
	default: {
		bool in_word2 = !tal_ws.is_ws[b];

		st->linepos += tal_ws.is_print[b];
		c->words += (unsigned)(!*in_word & in_word2);
		*in_word = in_word2;
		break;
	}
	}
}

/* Single-byte locales: the byte tables ARE the whole rule (wc.c:619-674). */
void tal_swc_sb(const unsigned char *p, size_t n, struct counts *c,
		struct wstate *st)
{
	bool in_word = st->in_word;

	if (!st->width) {
		unsigned long long words = 0, lines = 0;

		for (size_t i = 0; i < n; i++) {
			unsigned char b = p[i];
			bool in_word2 = !tal_ws.is_ws[b];

			lines += b == '\n';
			words += (unsigned)(!in_word & in_word2);
			in_word = in_word2;
		}
		c->words += words;
		c->lines += lines;
	} else {
		for (size_t i = 0; i < n; i++)
			classify_byte(p[i], c, st, &in_word);
	}
	st->in_word = in_word;
}

static void classify_wc(unsigned long wc, struct counts *c, struct wstate *st,
			bool *in_word)
{
	if (wc < 0x80) {
		classify_byte((unsigned char)wc, c, st, in_word);
		return;
	}
	if (st->width) {
		int w = wcwidth((wchar_t)wc);

		if (w > 0)
			st->linepos += (unsigned)w;
	}
	if (tal_sep_wchar(wc)) {
		*in_word = false;
	} else {
		c->words += !*in_word;
		*in_word = true;
	}
}

/* Multibyte locales, any charset. Pending partial sequences carry as RAW
 * bytes and every character decodes with a fresh state: equivalent to GNU's
 * mbstate carry for stateless charsets (fragmentation invariance probed on
 * the ref for valid AND invalid splits), and it lets the error path consume
 * rejected bytes one at a time exactly like wc.c:531-549. Stateful shift
 * encodings (ISO-2022) would need real mbstate carry; no target libc ships
 * such locales. */
void tal_swc_mb(const unsigned char *p, size_t n, struct counts *c,
		struct wstate *st)
{
	size_t i = 0;
	bool in_word = st->in_word;

	while (i < n) {
		if (st->npend == 0) {
			/* Bulk ASCII via the byte tables (GNU's fast path,
			 * wc.c:503-510). Each ASCII byte is one char. */
			if (!st->width) {
				unsigned long long words = 0, lines = 0,
						   chars = 0;

				while (i < n && p[i] < 0x80) {
					unsigned char b = p[i++];
					bool in_word2 = !tal_ws.is_ws[b];

					chars++;
					lines += b == '\n';
					words += (unsigned)(!in_word & in_word2);
					in_word = in_word2;
				}
				c->chars += chars;
				c->words += words;
				c->lines += lines;
			} else {
				while (i < n && p[i] < 0x80) {
					c->chars++;
					classify_byte(p[i++], c, st, &in_word);
				}
			}
			/* UTF-8 locales decode inline with u8dec — no libc
			 * round-trip, no pend churn mid-buffer. Bytes that
			 * can never start a character are one-byte errors at
			 * byte-scan speed; a valid-but-incomplete prefix at
			 * the chunk end goes to pend. */
			if (tal_ws.utf8) {
				while (i < n && p[i] >= 0x80) {
					unsigned long cp;
					int r = u8dec(p + i, n - i, &cp);

					if (r > 0) {
						c->chars++;
						classify_wc(cp, c, st,
							    &in_word);
						i += (size_t)r;
					} else if (r < 0) {
						c->words += !in_word;
						in_word = true;
						i++;
					} else {
						/* incomplete at chunk end */
						while (i < n)
							st->pend[st->npend++] =
								p[i++];
						goto out;
					}
				}
				if (i < n)
					continue; /* back to the ASCII bulk */
			}
			if (i >= n)
				break;
		}
		while (st->npend < sizeof st->pend && i < n)
			st->pend[st->npend++] = p[i++];

		while (st->npend) {
			size_t k;

			if (tal_ws.utf8) {
				unsigned long cp;
				int r = u8dec(st->pend, st->npend, &cp);

				if (r == 0) {
					if (i < n &&
					    st->npend < sizeof st->pend)
						break; /* refill */
					if (i >= n)
						goto out; /* carry */
					r = -1; /* pend full: error */
				}
				if (r < 0) {
					c->words += !in_word;
					in_word = true;
					st->npend--;
					memmove(st->pend, st->pend + 1,
						st->npend);
					continue;
				}
				c->chars++;
				classify_wc(cp, c, st, &in_word);
				k = (size_t)r;
			} else {
				mbstate_t ms;
				wchar_t wc;
				size_t r;

				memset(&ms, 0, sizeof ms);
				r = mbrtowc(&wc, (const char *)st->pend,
					    st->npend, &ms);
				if (r == (size_t)-2) {
					if (i < n &&
					    st->npend < sizeof st->pend)
						break; /* refill */
					if (i >= n)
						goto out; /* carry */
					/* pend full yet incomplete: no real
					 * charset needs >8 bytes — error. */
					r = (size_t)-1;
				}
				if (r == (size_t)-1) {
					/* Encoding error: one byte, non-space,
					 * word constituent, NOT a char
					 * (wc.c:531-549). */
					c->words += !in_word;
					in_word = true;
					st->npend--;
					memmove(st->pend, st->pend + 1,
						st->npend);
					continue;
				}
				c->chars++;
				classify_wc((unsigned long)wc, c, st,
					    &in_word);
				k = r ? r : 1; /* r==0: NUL, 1 byte */
			}
			st->npend -= (unsigned)k;
			memmove(st->pend, st->pend + k, st->npend);
		}
	}
out:
	st->in_word = in_word;
}

void tal_swc_finish(struct counts *c, struct wstate *st)
{
	/* EOF with a pending valid prefix: GNU consumes each byte through the
	 * error path — non-space constituents, not chars. */
	for (unsigned j = 0; j < st->npend; j++) {
		c->words += !st->in_word;
		st->in_word = true;
	}
	st->npend = 0;
	/* A final line without '\n' still counts for -L (wc.c:616-617). */
	if (st->linepos > c->linelength)
		c->linelength = st->linepos;
}
