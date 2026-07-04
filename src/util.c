#include <errno.h>
#include <langinfo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
