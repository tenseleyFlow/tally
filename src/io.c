#include <errno.h>
#include <fcntl.h>
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
