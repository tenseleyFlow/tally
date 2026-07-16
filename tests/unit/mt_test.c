#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "mt.h"
#include "simd.h"
#include "tests/unit/test.h"

/* The threaded -l path must equal the scalar kernel over the same bytes for
 * any size/thread mix, honor the starting fd offset (wc - -), advance the
 * offset past the counted span, and decline ineligible fds. */

static unsigned long long state = 7;

static unsigned long long next64(void)
{
	unsigned long long z = (state += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static int make_file(unsigned char **out, size_t n)
{
	char tmpl[] = "/tmp/talmtXXXXXX";
	int fd = mkstemp(tmpl);
	unsigned char *b = malloc(n ? n : 1);

	if (fd < 0 || !b)
		return -1;
	unlink(tmpl);
	for (size_t i = 0; i < n; i++)
		b[i] = (unsigned char)(next64() & 0xFF); /* ~1/256 newlines */
	size_t off = 0;

	while (off < n) {
		ssize_t w = write(fd, b + off, n - off);

		if (w <= 0)
			return -1;
		off += (size_t)w;
	}
	lseek(fd, 0, SEEK_SET);
	*out = b;
	return fd;
}

int main(void)
{
	char msg[96];

	/* Small threshold so unit-sized files exercise the threaded path
	 * (must be set before the first call: the threshold caches). */
	setenv("TAL_MT_MIN", "1", 1);

	static const size_t sizes[] = { 1,           4096,
					262144,      262145,
					262143,      1048576 + 7,
					3 * 262144 + 1 };

	for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
		for (int nth = 2; nth <= 5; nth++) {
			unsigned char *b;
			int fd = make_file(&b, sizes[s]);

			CHECK("mkfile", fd >= 0);
			if (fd < 0)
				continue;

			unsigned long long lines = 0, bytes = 0;
			int r = tal_count_lines_mt(fd, nth,
						   tal_nlcount_scalar,
						   &lines, &bytes);

			snprintf(msg, sizeof msg, "mt n=%zu t=%d", sizes[s],
				 nth);
			CHECK(msg, r == 0);
			CHECK(msg, bytes == sizes[s]);
			CHECK(msg, lines == tal_nlcount_scalar(b, sizes[s]));
			CHECK(msg,
			      lseek(fd, 0, SEEK_CUR) == (off_t)sizes[s]);
			close(fd);
			free(b);
		}
	}

	/* Starting offset owned by a prior reader (wc - -). */
	{
		unsigned char *b;
		int fd = make_file(&b, 800000);

		CHECK("mkfile", fd >= 0);
		if (fd >= 0) {
			unsigned long long lines = 0, bytes = 0;

			lseek(fd, 300000, SEEK_SET);
			CHECK("offset rc",
			      tal_count_lines_mt(fd, 3, tal_nlcount_scalar,
						 &lines, &bytes) == 0);
			CHECK("offset bytes", bytes == 500000);
			CHECK("offset lines",
			      lines == tal_nlcount_scalar(b + 300000, 500000));
			CHECK("offset end",
			      lseek(fd, 0, SEEK_CUR) == 800000);
			close(fd);
			free(b);
		}
	}

	/* Ineligible fds decline: pipes, single thread, exhausted files. */
	{
		int p[2];

		CHECK("pipe", pipe(p) == 0);
		unsigned long long lines = 0, bytes = 0;

		CHECK("pipe declines",
		      tal_count_lines_mt(p[0], 4, tal_nlcount_scalar, &lines,
					 &bytes) == -1);
		close(p[0]);
		close(p[1]);

		unsigned char *b;
		int fd = make_file(&b, 4096);

		CHECK("mkfile", fd >= 0);
		if (fd >= 0) {
			CHECK("one thread declines",
			      tal_count_lines_mt(fd, 1, tal_nlcount_scalar,
						 &lines, &bytes) == -1);
			lseek(fd, 4096, SEEK_SET);
			CHECK("eof declines",
			      tal_count_lines_mt(fd, 4, tal_nlcount_scalar,
						 &lines, &bytes) == -1);
			close(fd);
			free(b);
		}
	}

	return test_summary("mt_test");
}
