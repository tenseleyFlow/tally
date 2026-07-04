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
			if (i >= n)
				break;
		}
		while (st->npend < sizeof st->pend && i < n)
			st->pend[st->npend++] = p[i++];

		while (st->npend) {
			mbstate_t ms;
			wchar_t wc;
			size_t r;

			memset(&ms, 0, sizeof ms);
			r = mbrtowc(&wc, (const char *)st->pend, st->npend,
				    &ms);
			if (r == (size_t)-2) {
				if (i < n && st->npend < sizeof st->pend)
					break; /* refill */
				if (i >= n)
					goto out; /* carry across chunks */
				/* pend full yet incomplete: no real charset
				 * needs >8 bytes — treat lead as an error. */
				r = (size_t)-1;
			}
			if (r == (size_t)-1) {
				/* Encoding error: one byte, non-space, word
				 * constituent, NOT a char (wc.c:531-549). */
				c->words += !in_word;
				in_word = true;
				st->npend--;
				memmove(st->pend, st->pend + 1, st->npend);
				continue;
			}
			size_t k = r ? r : 1; /* r==0: decoded NUL, 1 byte */

			c->chars++;
			classify_wc((unsigned long)wc, c, st, &in_word);
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
