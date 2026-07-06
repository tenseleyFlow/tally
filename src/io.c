#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "config.h"
#include "io.h"

ssize_t tal_read(int fd, void *buf, size_t n)
{
	for (;;) {
		ssize_t got = read(fd, buf, n);

		if (got >= 0 || errno != EINTR)
			return got;
	}
}

void tal_fadvise_seq(int fd)
{
#if TAL_HAS_POSIX_FADVISE
	(void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#else
	(void)fd;
#endif
}

sigjmp_buf tal_sigbus_jmp;
volatile sig_atomic_t tal_sigbus_armed;

static void sigbus_handler(int sig)
{
	if (tal_sigbus_armed) {
		tal_sigbus_armed = 0;
		siglongjmp(tal_sigbus_jmp, 1);
	}
	/* Fault outside a guarded counting loop: restore and re-raise. */
	signal(sig, SIG_DFL);
	raise(sig);
}

void tal_sigbus_install(void)
{
	static bool done;
	struct sigaction sa;

	if (done)
		return;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = sigbus_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NODEFER;
	sigaction(SIGBUS, &sa, NULL);
	done = true;
}

bool tal_map_acquire(int fd, struct tal_map *m)
{
	static long threshold;
	static bool thr_init;
	struct stat st;

	if (!thr_init) {
		const char *e = getenv("TAL_MMAP_MIN");

		thr_init = true;

		if (e) {
			threshold = atol(e);
		} else {
#ifdef __FreeBSD__
			/* Measured 2.6x SLOWER than read() on FreeBSD/ZFS
			 * (ARC pages aren't shared with the page cache, so
			 * mapping double-copies). Linux +30%, macOS +18% —
			 * default on there, off here; TAL_MMAP_MIN overrides
			 * and the golden parity-mmap phase exercises the
			 * path on every platform regardless. */
			threshold = -1; /* disabled */
#else
			threshold = 4L * 1024 * 1024;
#endif
		}
	}
	if (threshold < 0)
		return false;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
		return false;

	off_t cur = lseek(fd, 0, SEEK_CUR);

	if (cur < 0 || cur >= st.st_size)
		return false;

	size_t remain = (size_t)(st.st_size - cur);

	if (remain < (size_t)threshold)
		return false;

	long page = sysconf(_SC_PAGESIZE);

	if (page <= 0)
		return false;

	off_t aligned = cur - cur % page;
	size_t maplen = remain + (size_t)(cur - aligned);
	int flags = MAP_PRIVATE;

	/* Per-page faults on a 200 MB mapping cost more than the read() copy
	 * loop they replace — prefault where the platform can. */
#ifdef MAP_PREFAULT_READ
	flags |= MAP_PREFAULT_READ; /* FreeBSD */
#endif
#ifdef MAP_POPULATE
	flags |= MAP_POPULATE; /* Linux */
#endif
	void *base = mmap(NULL, maplen, PROT_READ, flags, fd, aligned);

	if (base == MAP_FAILED)
		return false;
#ifdef MADV_SEQUENTIAL
	(void)madvise(base, maplen, MADV_SEQUENTIAL);
#endif
#ifdef MADV_WILLNEED
	(void)madvise(base, maplen, MADV_WILLNEED);
#endif
	m->base = base;
	m->maplen = maplen;
	m->data = (const unsigned char *)base + (cur - aligned);
	m->len = remain;
	m->fd = fd;
	tal_sigbus_install();
	return true;
}

void tal_map_release(struct tal_map *m)
{
	/* Mapping never moved the fd offset: advance past the consumed span
	 * so repeated reads of the same fd (wc - -) see EOF like the read()
	 * path does. Caught by the parity-mmap golden phase on day one. */
	(void)lseek(m->fd, (off_t)m->len, SEEK_CUR);
	munmap(m->base, m->maplen);
	m->base = NULL;
	m->data = NULL;
	m->len = m->maplen = 0;
}
