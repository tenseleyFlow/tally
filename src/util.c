#include <errno.h>
#include <langinfo.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "util.h"

const char *tal_prog = "tally";

void tal_set_program(const char *argv0)
{
	const char *slash = strrchr(argv0, '/');

	tal_prog = slash ? slash + 1 : argv0;
}

static int locale_is_utf8(void)
{
	static signed char cached; /* 0 unknown, 1 yes, -1 no */

	if (!cached)
		cached = strcmp(nl_langinfo(CODESET), "UTF-8") == 0 ? 1 : -1;
	return cached > 0;
}

const char *tal_qs(void)
{
	return locale_is_utf8() ? "\342\200\230" : "'"; /* U+2018 */
}

const char *tal_qe(void)
{
	return locale_is_utf8() ? "\342\200\231" : "'"; /* U+2019 */
}

/* Shell-safe bytes print bare under quotef (probed/fuzzed against the ref:
 * '=', '^', '!', '(', ';', '*', '$', '\\', '[', ':', space all quote; '~',
 * '#', '%', ',', '@', '/', '.', '-', '_', '+', '{', '}', ']' and bytes >= 0x80
 * do not — though '#' and '~' quote at position 0). */
static bool sh_safe(unsigned char c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	    (c >= '0' && c <= '9') || c >= 0x80)
		return true;
	return strchr("%+,-./@_~#{}]", c) != NULL;
}

static char *qbuf;
static size_t qcap;

static void qput(size_t *len, const char *s, size_t n)
{
	if (*len + n + 1 > qcap) {
		qcap = (*len + n + 1) * 2 + 32;
		qbuf = realloc(qbuf, qcap);
		if (!qbuf)
			tal_die(1, errno, "memory exhausted");
	}
	memcpy(qbuf + *len, s, n);
	*len += n;
}

/* Per-character classification for shell quoting: decodes multibyte chars
 * with the locale (fresh state per char, like the oracle). */
enum qclass { QC_SAFE, QC_PLAIN, QC_SQ, QC_ESC };

static enum qclass qclassify(const unsigned char *p, size_t n, size_t *step)
{
	unsigned char c = *p;

	*step = 1;
	if (c == '\'')
		return QC_SQ;
	if (c < 0x80) {
		if (c < 0x20 || c == 0x7F)
			return QC_ESC;
		return sh_safe(c) ? QC_SAFE : QC_PLAIN;
	}

	mbstate_t ms;
	wchar_t wc;
	size_t r;

	memset(&ms, 0, sizeof ms);
	r = mbrtowc(&wc, (const char *)p, n, &ms);
	if (r == (size_t)-1 || r == (size_t)-2 || r == 0)
		return QC_ESC; /* invalid or truncated: escape one byte */
	*step = r;
	return iswprint((wint_t)wc) ? QC_SAFE : QC_ESC;
}

static const char *quote_shell(const char *name, bool always)
{
	const unsigned char *s = (const unsigned char *)name;
	size_t slen = strlen(name);
	bool unsafe = !name[0], esc = false, sq = false, dqspecial = false;
	bool plain = false;

	/* '#' and '~' are safe mid-string but shell-special at the front
	 * (comment, tilde expansion) — the ref quotes '#%j0' but not 'ha#sh'.
	 * A name that IS a brace is a shell reserved word (fuzzed: '}'). */
	if (name[0] == '#' || name[0] == '~' ||
	    ((name[0] == '{' || name[0] == '}') && !name[1])) {
		unsafe = true;
		plain = true;
	}
	for (size_t i = 0; i < slen;) {
		size_t step;
		enum qclass q = qclassify(s + i, slen - i, &step);

		if (q != QC_SAFE)
			unsafe = true;
		if (q == QC_ESC)
			esc = true;
		if (q == QC_SQ)
			sq = true;
		if (q == QC_PLAIN)
			plain = true;
		if (s[i] == '"' || s[i] == '$' || s[i] == '`' || s[i] == '\\')
			dqspecial = true;
		i += step;
	}

	size_t len = 0;

	if (!unsafe) {
		if (!always)
			return name;
		qput(&len, "'", 1);
		qput(&len, name, slen);
		qput(&len, "'", 1);
		qbuf[len] = '\0';
		return qbuf;
	}
	if (sq && !dqspecial && !esc && !plain) {
		/* The ONLY offender is a single-quote: "..." — any other
		 * unsafe char forces the '\'' splice form (fuzzed: '_*H). */
		qput(&len, "\"", 1);
		qput(&len, name, slen);
		qput(&len, "\"", 1);
		qbuf[len] = '\0';
		return qbuf;
	}

	/* '...' segments; escape-class bytes as $'\X' (consecutive ones share
	 * one segment); quotes as '\'' splices with immediate reopen. The
	 * output begins with an open segment, so a leading escape yields an
	 * empty '' prefix — all probed/fuzzed against the ref. */
	static const char escc[] = "abtnvfr"; /* \a=7 .. \r=13 */
	bool open = true;

	qput(&len, "'", 1);
	for (size_t i = 0; i < slen;) {
		size_t step;
		enum qclass q = qclassify(s + i, slen - i, &step);

		if (q == QC_ESC) {
			if (open) {
				qput(&len, "'", 1);
				open = false;
			}
			qput(&len, "$'", 2);
			for (;;) {
				for (size_t j = 0; j < step; j++) {
					unsigned char c = s[i + j];
					char tmp[8];
					int n;

					if (c >= 7 && c <= 13)
						n = snprintf(tmp, sizeof tmp,
							     "\\%c",
							     escc[c - 7]);
					else
						n = snprintf(tmp, sizeof tmp,
							     "\\%03o", c);
					qput(&len, tmp, (size_t)n);
				}
				i += step;
				if (i >= slen ||
				    qclassify(s + i, slen - i, &step) != QC_ESC)
					break;
			}
			qput(&len, "'", 1);
		} else if (q == QC_SQ) {
			if (open) {
				qput(&len, "'", 1);
				open = false;
			}
			/* gnulib re-opens immediately after the splice, so a
			 * following escape yields an empty '' pair. */
			qput(&len, "\\''", 3);
			open = true;
			i += step;
		} else {
			if (!open) {
				qput(&len, "'", 1);
				open = true;
			}
			qput(&len, (const char *)(s + i), step);
			i += step;
		}
	}
	if (open)
		qput(&len, "'", 1);
	qbuf[len] = '\0';
	return qbuf;
}

const char *tal_quotef(const char *name)
{
	return quote_shell(name, false);
}

const char *tal_quoteaf(const char *name)
{
	return quote_shell(name, true);
}

char *tal_u64tostr(unsigned long long v, char *buf)
{
	char *p = buf + TAL_U64_BUFSIZE - 1;

	*p = '\0';
	do {
		*--p = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	return p;
}

static void verror(int errnum, const char *fmt, va_list ap)
{
	fputs(tal_prog, stderr);
	fputs(": ", stderr);
	vfprintf(stderr, fmt, ap);
	if (errnum)
		fprintf(stderr, ": %s", strerror(errnum));
	putc('\n', stderr);
}

void tal_error(int errnum, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	verror(errnum, fmt, ap);
	va_end(ap);
}

void tal_die(int status, int errnum, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	verror(errnum, fmt, ap);
	va_end(ap);
	exit(status);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p)
		tal_die(1, errno, "memory exhausted");
	return p;
}
