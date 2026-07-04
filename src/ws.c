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
	 * the byte whose wide char is NBSP-family (FreeBSD C locale: 0xA0). */
	for (int i = 0; i < 256; i++) {
		wint_t wc = btowc(i);

		w->is_ws[i] = (unsigned char)(isspace(i) != 0 ||
					      (!w->posix_correct &&
					       wc != WEOF &&
					       is_nbspace((unsigned long)wc)));
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
}
