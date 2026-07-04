#include <ctype.h>
#include <langinfo.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "config.h"
#include "ws.h"

struct ws_spec tal_ws;

/* GNU c32isnbspace (system.h:158-161). */
static bool is_nbspace(unsigned long wc)
{
	return wc == 0x00A0 || wc == 0x2007 || wc == 0x202F || wc == 0x2060;
}

static bool sep_cp(const struct ws_spec *w, unsigned long cp)
{
	return iswspace((wint_t)cp) ||
	       (!w->posix_correct && is_nbspace(cp));
}

bool tal_sep_wchar(unsigned long wc)
{
	return sep_cp(&tal_ws, wc);
}

static int utf8_encode(unsigned long cp, unsigned char *out)
{
	if (cp < 0x80) {
		out[0] = (unsigned char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (unsigned char)(0xC0 | (cp >> 6));
		out[1] = (unsigned char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (unsigned char)(0xE0 | (cp >> 12));
		out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (unsigned char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (unsigned char)(0xF0 | (cp >> 18));
	out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (unsigned char)(0x80 | (cp & 0x3F));
	return 4;
}

/* Mula 2-pshufb byte-set LUTs: one bit per distinct high nibble among the
 * set; lo LUT gets that bit at every low nibble forming a member with it.
 * match(b) = hi[b>>4] & lo[b&15]. Exact for <=8 distinct high nibbles. */
static bool build_luts(const unsigned char *bytes, int n,
		       unsigned char *lut_lo, unsigned char *lut_hi)
{
	unsigned char his[8];
	int nhis = 0;

	memset(lut_lo, 0, 16);
	memset(lut_hi, 0, 16);
	for (int i = 0; i < n; i++) {
		unsigned char hi = bytes[i] >> 4;
		int g = -1;

		for (int k = 0; k < nhis; k++)
			if (his[k] == hi)
				g = k;
		if (g < 0) {
			if (nhis == 8)
				return false;
			his[nhis] = hi;
			g = nhis++;
		}
		lut_hi[hi] |= (unsigned char)(1 << g);
		lut_lo[bytes[i] & 15] |= (unsigned char)(1 << g);
	}
	return true;
}

void ws_init(struct ws_spec *w)
{
	memset(w, 0, sizeof *w);
	w->posix_correct = getenv("POSIXLY_CORRECT") != NULL;
	w->multibyte = MB_CUR_MAX > 1;
	w->utf8 = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;

	/* Byte table, built exactly like wc's (wc.c:866-868): isspace plus
	 * the byte whose wide char is NBSP-family. Where btowc fails in a
	 * single-byte locale (glibc's ASCII-only C locale on byte 0xA0),
	 * gnulib's btoc32 — what the ref actually calls — still yields the
	 * byte value as the code point; probed on glibc: 0xA0 separates
	 * words in LC_ALL=C, all other high bytes don't. Mirror that. */
	for (int i = 0; i < 256; i++) {
		wint_t wc = btowc(i);
		unsigned long cp = (wc != WEOF) ? (unsigned long)wc
			: (!w->multibyte ? (unsigned long)i : 0);

		w->is_ws[i] = (unsigned char)(isspace(i) != 0 ||
					      (!w->posix_correct &&
					       is_nbspace(cp)));
		w->is_print[i] = (unsigned char)(isprint(i) != 0);
	}

	/* SIMD byte set: full table in single-byte locales; ASCII part in
	 * multibyte locales (bytes >= 0x80 there go through decode). */
	int lim = w->multibyte ? 0x80 : 0x100;

	w->n_ws_bytes = 0;
	for (int i = 0; i < lim; i++) {
		if (!w->is_ws[i])
			continue;
		if (w->n_ws_bytes == 16) {
			w->n_ws_bytes = -1;
			break;
		}
		w->ws_bytes[w->n_ws_bytes++] = (unsigned char)i;
	}

	/* Multibyte separators (utf8 only): probe the libc, encode, collect
	 * lead bytes. All real libcs stop at U+3000 (audit 02). */
	if (w->multibyte && w->utf8) {
		for (unsigned long cp = 0x80; cp <= 0x3000; cp++) {
			if (!sep_cp(w, cp))
				continue;
			if (w->nmbws == 64)
				break;
			struct mbws *m = &w->mbws[w->nmbws++];

			m->len = (unsigned char)utf8_encode(cp, m->seq);
			w->suspect[m->seq[0]] = true;
		}
		w->n_sus_bytes = 0;
		for (int i = 0x80; i < 0x100; i++) {
			if (!w->suspect[i])
				continue;
			if (w->n_sus_bytes == 16) {
				w->n_sus_bytes = -1;
				break;
			}
			w->sus_bytes[w->n_sus_bytes++] = (unsigned char)i;
		}
	}

	w->luts_ok = w->n_ws_bytes >= 0 && w->n_sus_bytes >= 0 &&
		     build_luts(w->ws_bytes, w->n_ws_bytes, w->ws_lut_lo,
				w->ws_lut_hi) &&
		     build_luts(w->sus_bytes, w->n_sus_bytes, w->sus_lut_lo,
				w->sus_lut_hi);

	if (w->n_ws_bytes > 0)
		for (int i = 0; i < w->n_ws_bytes; i++)
			w->kernel_ws[w->ws_bytes[i]] = 1;

	/* L1 groups. Real UTF-8 locales yield ~5: (C2), (E1,9A), (E2,80),
	 * (E2,81), (E3,80). Overflow => l1_ok false => scalar word counting
	 * in this locale (parity preserved, speed sacrificed). */
	w->l1_ok = true;
	for (int i = 0; i < w->nmbws; i++) {
		const struct mbws *m = &w->mbws[i];
		struct mbws_group *g = NULL;

		if (m->len < 2 || m->len > 3) {
			w->l1_ok = false; /* can't happen for cp <= 0x3000 */
			break;
		}
		if (m->len == 3)
			w->is3lead[m->seq[0]] = true;
		for (int k = 0; k < w->ngroups; k++) {
			struct mbws_group *c = &w->groups[k];

			if (c->len == m->len && c->lead == m->seq[0] &&
			    (m->len == 2 || c->second == m->seq[1])) {
				g = c;
				break;
			}
		}
		if (!g) {
			if (w->ngroups == 8) {
				w->l1_ok = false;
				break;
			}
			g = &w->groups[w->ngroups++];
			g->lead = m->seq[0];
			g->second = m->len == 3 ? m->seq[1] : 0;
			g->len = m->len;
			g->nset = 0;
		}
		if (g->nset == (int)sizeof g->set_bytes) {
			w->l1_ok = false;
			break;
		}
		g->set_bytes[g->nset++] = m->seq[m->len - 1];
	}
	for (int k = 0; w->l1_ok && k < w->ngroups; k++) {
		struct mbws_group *g = &w->groups[k];

		/* build_luts caps at 16 members; the (E2,80) group holds ~15
		 * across libcs. Overflow degrades to scalar, never to wrong. */
		if (g->nset > 16 ||
		    !build_luts(g->set_bytes, g->nset, g->set_lo, g->set_hi))
			w->l1_ok = false;
	}
}

int tal_wcwidth(unsigned long cp)
{
	/* Value stored as wcwidth+1 so 0 means "uncached impossible" and the
	 * whole-table memset(0)+fill is race-free in our single thread. */
	static unsigned char bmp[0x10000];
	static bool ready;

	if (cp > 0xFFFF)
		return wcwidth((wchar_t)cp);
	if (!ready) {
		for (unsigned long c = 0; c < 0x10000; c++) {
			int w = wcwidth((wchar_t)c);

			bmp[c] = (unsigned char)(w < -1 ? 0 : w + 1);
		}
		ready = true;
	}
	return (int)bmp[cp] - 1;
}

int tal_mbws_match(const unsigned char *p, size_t n)
{
	if (n < 2)
		return 0;
	for (int k = 0; k < tal_ws.ngroups; k++) {
		const struct mbws_group *g = &tal_ws.groups[k];

		if (p[0] != g->lead)
			continue;
		if (g->len == 2) {
			for (int i = 0; i < g->nset; i++)
				if (p[1] == g->set_bytes[i])
					return 2;
		} else if (n >= 3 && p[1] == g->second) {
			for (int i = 0; i < g->nset; i++)
				if (p[2] == g->set_bytes[i])
					return 3;
		}
	}
	return 0;
}
