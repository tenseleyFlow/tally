#ifndef TAL_IO_H
#define TAL_IO_H

#include <sys/types.h>

/* Matches GNU wc's IO_BUFSIZE (256 KiB since coreutils 9.6). The counting
 * buffer is 64-byte aligned so SIMD kernels load at full width from offset 0. */
#define TAL_IO_BUFSIZE (256 * 1024)

/* read(2) with EINTR retry. Returns bytes read, 0 at EOF, -1 on error. */
ssize_t tal_read(int fd, void *buf, size_t n);

/* posix_fadvise(SEQUENTIAL) where the platform has it; no-op otherwise. */
void tal_fadvise_seq(int fd);

#endif /* TAL_IO_H */
