#include "config.h"

/* Threaded -l/-c over large regular files (roadmap P5, fastlwc-mt pattern):
 * each worker preads an interleaved round-robin stripe of TAL_IO_BUFSIZE
 * blocks and counts newlines with the same kernel the serial path picked;
 * the merge is a sum (line counting carries no cross-block state). Opt-in
 * (--tally-threads / TAL_THREADS), regular files only — pipes, devices and
 * small files fall back to the serial paths untouched. pread never moves
 * the fd offset, so the offset is advanced once at the end exactly like the
 * mmap path (wc - -). */
#include "mt.h"

#if TAL_HAS_PTHREAD

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "io.h"

struct mt_job {
	int fd;
	off_t start;      /* fd offset when we began (wc - - semantics) */
	off_t len;        /* bytes this run owns (size - start at fstat) */
	int idx, nth;
	tal_nl_fn nl;
	unsigned long long lines, bytes;
	int err;
};

/* pread the whole request unless EOF or error; EINTR retries. */
static ssize_t pread_full(int fd, unsigned char *buf, size_t want, off_t off)
{
	size_t got = 0;

	while (got < want) {
		ssize_t r = pread(fd, buf + got, want - got, off + (off_t)got);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	return (ssize_t)got;
}

static void *mt_worker(void *v)
{
	struct mt_job *j = v;
	unsigned char *buf = malloc(TAL_IO_BUFSIZE);

	if (!buf) {
		j->err = ENOMEM;
		return NULL;
	}
	for (off_t blk = j->idx; blk * (off_t)TAL_IO_BUFSIZE < j->len;
	     blk += j->nth) {
		off_t off = blk * (off_t)TAL_IO_BUFSIZE;
		size_t want = TAL_IO_BUFSIZE;

		if (off + (off_t)want > j->len)
			want = (size_t)(j->len - off);
		ssize_t got = pread_full(j->fd, buf, want, j->start + off);

		if (got < 0) {
			j->err = errno;
			break;
		}
		if (got == 0)
			break; /* file shrank under us: count what exists */
		j->lines += j->nl(buf, (size_t)got);
		j->bytes += (unsigned long long)got;
	}
	free(buf);
	return NULL;
}

/* Returns -1 when the fd is not eligible (caller falls back to the serial
 * paths); otherwise 0 or an errno, with *lines and *bytes summed. */
int tal_count_lines_mt(int fd, int nthreads, tal_nl_fn nl,
		       unsigned long long *lines, unsigned long long *bytes)
{
	static long minbytes = -1;
	struct stat st;

	if (minbytes < 0) {
		const char *e = getenv("TAL_MT_MIN");

		minbytes = (e && *e) ? atol(e) : 8L * 1024 * 1024;
	}
	if (nthreads < 2 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
		return -1;

	off_t start = lseek(fd, 0, SEEK_CUR);

	if (start < 0 || st.st_size <= start)
		return -1;

	off_t len = st.st_size - start;

	if (len < minbytes)
		return -1;

	off_t blocks = (len + TAL_IO_BUFSIZE - 1) / TAL_IO_BUFSIZE;

	if (blocks < nthreads)
		nthreads = (int)blocks;

	struct mt_job jobs[256];
	pthread_t tids[256];
	int spawned = 0;

	if (nthreads > 256)
		nthreads = 256;
	for (int t = 0; t < nthreads; t++) {
		jobs[t] = (struct mt_job){ .fd = fd, .start = start,
					   .len = len, .idx = t,
					   .nth = nthreads, .nl = nl };
		if (pthread_create(&tids[t], NULL, mt_worker, &jobs[t]) != 0)
			break;
		spawned++;
	}
	if (spawned == 0)
		return -1; /* couldn't thread at all: serial fallback */
	/* Workers own stripes idx < spawned; any unspawned stripes run here. */
	for (int t = spawned; t < nthreads; t++) {
		jobs[t].nth = nthreads;
		mt_worker(&jobs[t]);
	}

	int err = 0;
	unsigned long long l = 0, b = 0;

	for (int t = 0; t < spawned; t++)
		pthread_join(tids[t], NULL);
	for (int t = 0; t < nthreads; t++) {
		l += jobs[t].lines;
		b += jobs[t].bytes;
		if (jobs[t].err && !err)
			err = jobs[t].err;
	}
	*lines += l;
	*bytes += b;
	/* Consume the counted span (mirrors tal_map_release). */
	(void)lseek(fd, start + (off_t)b, SEEK_SET);
	return err;
}

#else

int tal_count_lines_mt(int fd, int nthreads, tal_nl_fn nl,
		       unsigned long long *lines, unsigned long long *bytes)
{
	(void)fd; (void)nthreads; (void)nl; (void)lines; (void)bytes;
	return -1;
}

#endif /* TAL_HAS_PTHREAD */
