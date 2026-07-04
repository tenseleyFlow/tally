#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

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
	fputs("tally: ", stderr);
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
