#ifndef TAL_IO_H
#define TAL_IO_H

#include <stdbool.h>
#include <sys/types.h>

/* Matches GNU wc's IO_BUFSIZE (256 KiB since coreutils 9.6). The counting
 * buffer is 64-byte aligned so SIMD kernels load at full width from offset 0. */
#define TAL_IO_BUFSIZE (256 * 1024)

/* read(2) with EINTR retry. Returns bytes read, 0 at EOF, -1 on error. */
ssize_t tal_read(int fd, void *buf, size_t n);

/* posix_fadvise(SEQUENTIAL) where the platform has it; no-op otherwise. */
void tal_fadvise_seq(int fd);

/* mmap fast path (roadmap P1): the read() copy is ~half of wall time on
 * cached big files. Regular files whose remaining size clears the threshold
 * (default 4 MiB; TAL_MMAP_MIN overrides — the harness sets 1 to drive every
 * golden case through this path) map as ONE span, so per-chunk carry logic
 * disappears. Kernels touching a mapping can fault if the file is truncated
 * mid-count: counting loops arm the SIGBUS guard and convert the fault to a
 * read error — GNU's read() path would silently count the shorter file, an
 * unobservable-in-practice difference under an active truncation race. */
struct tal_map {
	const unsigned char *data;
	size_t len;
	void *base; /* page-aligned mapping start */
	size_t maplen;
	int fd;
};

#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>

bool tal_map_acquire(int fd, struct tal_map *m);
void tal_map_release(struct tal_map *m);

extern sigjmp_buf tal_sigbus_jmp;
extern volatile sig_atomic_t tal_sigbus_armed;
void tal_sigbus_install(void);

#endif /* TAL_IO_H */
