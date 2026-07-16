#ifndef TAL_MT_H
#define TAL_MT_H

#include <stddef.h>

#include <sys/types.h>

typedef unsigned long long (*tal_nl_fn)(const unsigned char *, size_t);

/* Eligibility floor for threaded counting (TAL_MT_MIN, default 8 MiB). */
long tal_mt_min(void);

/* pread the whole request unless EOF or error; EINTR retries. */
ssize_t tal_pread_full(int fd, unsigned char *buf, size_t want, off_t off);

/* Threaded newline count over an eligible fd (regular file, at least
 * TAL_MT_MIN bytes past the current offset, threads at least 2). Returns -1 when
 * not eligible — the caller falls back to the serial paths — else 0 or an
 * errno with *lines and *bytes accumulated and the fd offset advanced past the
 * counted span. */
int tal_count_lines_mt(int fd, int nthreads, tal_nl_fn nl,
		       unsigned long long *lines, unsigned long long *bytes);

/* Run fn over njobs job slots (jobsz bytes apart) on parallel threads;
 * falls back to inline execution when spawning fails. 0 when built without
 * pthreads (caller must run serially). */
int tal_mt_run(int njobs, void *(*fn)(void *), void *jobs, size_t jobsz);

#endif /* TAL_MT_H */
