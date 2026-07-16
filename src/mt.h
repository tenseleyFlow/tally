#ifndef TAL_MT_H
#define TAL_MT_H

#include <stddef.h>

typedef unsigned long long (*tal_nl_fn)(const unsigned char *, size_t);

/* Threaded newline count over an eligible fd (regular file, at least
 * TAL_MT_MIN bytes past the current offset, threads at least 2). Returns -1 when
 * not eligible — the caller falls back to the serial paths — else 0 or an
 * errno with *lines and *bytes accumulated and the fd offset advanced past the
 * counted span. */
int tal_count_lines_mt(int fd, int nthreads, tal_nl_fn nl,
		       unsigned long long *lines, unsigned long long *bytes);

#endif /* TAL_MT_H */
