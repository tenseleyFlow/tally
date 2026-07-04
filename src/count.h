#ifndef TAL_COUNT_H
#define TAL_COUNT_H

#include <sys/stat.h>

#include "options.h"

struct counts {
	unsigned long long lines, words, chars, bytes;
	unsigned long long linelength; /* max display width seen (-L) */
};

/* stat result cache, GNU wc's struct fstatus (wc.c:85-93): failed > 0 means
 * "not stat'ed yet"; 0/-1 is the stat return. Filled by the width estimator,
 * reused by the -c fast path so each file is stat'ed at most once. */
struct fstatus {
	int failed;
	struct stat st;
};

/* Count FD per the requested counters into *c (zeroed here). Returns 0 on
 * success or the read errno; the caller prints the (partial) counts line
 * BEFORE diagnosing the error (audit 01 §errors). Dispatch per audit 01:
 * bytes-only takes the fstat/lseek zero-read path; lines(+bytes) the SIMD
 * newline kernel. Words/chars/-L arrive in sprints 02/03. */
int count_fd(int fd, const struct options *o, struct fstatus *fst,
	     struct counts *c);

#endif /* TAL_COUNT_H */
