#include <errno.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "count.h"
#include "io.h"
#include "simd.h"
#include "sys/detect.h"
#include "util.h"
#include "ws.h"

/* One file processed at a time; a single static aligned buffer serves every
 * read path (no allocation on the hot path). */
static alignas(64) unsigned char buf[TAL_IO_BUFSIZE];

typedef unsigned long long (*nl_fn)(const unsigned char *, size_t);

static nl_fn pick_nl_kernel(bool debug)
{
	nl_fn fn = tal_nlcount_scalar;
	const char *name = "scalar";

#if TAL_HAS_SSE2 && defined(__SSE2__)
	fn = tal_nlcount_sse2;
	name = "sse2";
#elif TAL_HAS_NEON && defined(__ARM_NEON)
	fn = tal_nlcount_neon;
	name = "neon";
#endif
#if TAL_HAS_AVX2
	if (tal_cpu_has_avx2()) {
		fn = tal_nlcount_avx2;
		name = "avx2";
	}
#endif
	/* Mirrors GNU: acceleration info prints only when the lines-only path
	 * consults the kernel (wc.c:137-180); text is a sanctioned deviation.
	 * This is also the hook the CI engagement check greps (audit 04). */
	if (debug)
		fprintf(stderr, "%s: using %s line kernel\n", tal_prog, name);
	return fn;
}

#ifndef DEV_BSIZE
#define DEV_BSIZE 512
#endif

/* coreutils stat-size.h ST_BLKSIZE: trust st_blksize only when sane. */
static unsigned long long st_blksize_sane(const struct stat *st)
{
	if (0 < st->st_blksize &&
	    (unsigned long long)st->st_blksize <= (size_t)-1 / 8 + 1)
		return (unsigned long long)st->st_blksize;
	return DEV_BSIZE;
}

/* GNU usable_st_size: regular files plus POSIX shared-memory/typed-memory
 * objects (macros are 0 on most platforms). */
static bool usable_st_size(const struct stat *st)
{
	return S_ISREG(st->st_mode)
#ifdef S_TYPEISSHM
	       || S_TYPEISSHM(st)
#endif
#ifdef S_TYPEISTMO
	       || S_TYPEISTMO(st)
#endif
		;
}

/* The -c zero-read fast path, wc.c:410-465 ported line for line (audit 00
 * claim 12): st_size is authoritative only when not a page multiple; page-
 * multiple sizes (/proc's 0 included) seek near EOF and read the tail; a
 * pre-positioned fd subtracts SEEK_CUR. */
static int count_bytes_only(int fd, struct fstatus *fst, struct counts *c)
{
	bool skip_read = false;

	if (fst->failed > 0)
		fst->failed = fstat(fd, &fst->st);

	if (!fst->failed && usable_st_size(&fst->st) && fst->st.st_size >= 0) {
		off_t end_pos = fst->st.st_size;
		off_t cur_pos = lseek(fd, 0, SEEK_CUR);
		long page = sysconf(_SC_PAGESIZE);

		if (cur_pos < 0) {
			/* Not seekable (pipe): fall through to the read loop. */
		} else if (page > 0 && end_pos % page) {
			off_t bytes = end_pos < cur_pos ? 0 : end_pos - cur_pos;

			if (bytes && lseek(fd, bytes, SEEK_CUR) >= 0) {
				c->bytes = (unsigned long long)bytes;
				skip_read = true;
			}
		} else {
			off_t hi_pos = end_pos
				- (off_t)((unsigned long long)end_pos
					  % (st_blksize_sane(&fst->st) + 1));

			if (cur_pos < hi_pos &&
			    lseek(fd, hi_pos, SEEK_CUR) >= 0)
				c->bytes = (unsigned long long)(hi_pos - cur_pos);
		}
	}

	if (!skip_read) {
		tal_fadvise_seq(fd);
		for (;;) {
			ssize_t got = tal_read(fd, buf, TAL_IO_BUFSIZE);

			if (got < 0)
				return errno;
			if (got == 0)
				break;
			c->bytes += (unsigned long long)got;
		}
	}
	return 0;
}

static int count_lines(int fd, const struct options *o, struct counts *c)
{
	static nl_fn nl; /* selected once per process */

	if (!nl)
		nl = pick_nl_kernel(o->debug);

	for (;;) {
		ssize_t got = tal_read(fd, buf, TAL_IO_BUFSIZE);

		if (got < 0)
			return errno;
		if (got == 0)
			break;
		c->bytes += (unsigned long long)got;
		c->lines += nl(buf, (size_t)got);
	}
	return 0;
}

/* Word counting (+ fused lines), the audit-02 block driver.
 * L0: the SIMD kernel treats every byte outside the derived separator set as
 * a word constituent — exact for single-byte locales, and exact for UTF-8
 * until a suspect byte (a multibyte-separator lead) appears.
 * L2: a block containing a suspect (or starting with pending decode bytes)
 * re-runs through the scalar oracle with carried state.
 * Non-UTF-8 multibyte locales and pathological byte sets: scalar throughout. */

typedef bool (*lwc_fn)(const unsigned char *, size_t, unsigned,
		       struct lwc_out *, bool);

#define WBLOCK 8192

static lwc_fn pick_lwc_kernel(bool debug)
{
	lwc_fn fn = NULL;
	const char *name = "scalar";

	if (tal_ws.luts_ok && (!tal_ws.multibyte || tal_ws.utf8)) {
#if TAL_HAS_SSE2 && defined(__SSE2__)
		fn = tal_lwc_sse2;
		name = "sse2";
#elif TAL_HAS_NEON && defined(__ARM_NEON)
		fn = tal_lwc_neon;
		name = "neon";
#endif
#if TAL_HAS_AVX2
		if (tal_cpu_has_avx2()) {
			fn = tal_lwc_avx2;
			name = "avx2";
		}
#endif
	}
	if (debug)
		fprintf(stderr, "%s: using %s word kernel%s\n", tal_prog, name,
			fn && tal_ws.multibyte ? " (suspect-gated)" : "");
	return fn;
}

static int count_words(int fd, const struct options *o, struct counts *c)
{
	static bool ws_ready;
	static lwc_fn kern;
	struct wstate st;

	if (!ws_ready) {
		ws_init(&tal_ws);
		kern = pick_lwc_kernel(o->debug);
		ws_ready = true;
	}
	wstate_init(&st);

	for (;;) {
		ssize_t got = tal_read(fd, buf, TAL_IO_BUFSIZE);

		if (got < 0)
			return errno;
		if (got == 0)
			break;
		c->bytes += (unsigned long long)got;

		size_t len = (size_t)got;

		if (!kern) {
			if (tal_ws.multibyte)
				tal_swc_mb(buf, len, c, &st);
			else
				tal_swc_sb(buf, len, c, &st);
			continue;
		}
		if (!tal_ws.multibyte) {
			/* Single-byte locale: the byte set is the whole rule;
			 * no suspects can exist. */
			struct lwc_out out;

			(void)kern(buf, len, !st.in_word, &out, false);
			c->lines += out.lines;
			c->words += out.words;
			st.in_word = !out.last_is_ws;
			continue;
		}
		/* UTF-8: per-block optimistic kernel with scalar re-run. */
		for (size_t off = 0; off < len; off += WBLOCK) {
			size_t blen = len - off < WBLOCK ? len - off : WBLOCK;
			struct lwc_out out;

			if (st.npend == 0 &&
			    kern(buf + off, blen, !st.in_word, &out, true)) {
				c->lines += out.lines;
				c->words += out.words;
				st.in_word = !out.last_is_ws;
			} else {
				tal_swc_mb(buf + off, blen, c, &st);
			}
		}
	}
	if (tal_ws.multibyte)
		tal_swc_mb_finish(c, &st);
	return 0;
}

int count_fd(int fd, const struct options *o, struct fstatus *fst,
	     struct counts *c)
{
	bool bytes_only = o->bytes && !o->lines && !o->words && !o->chars &&
			  !o->linelength;

	memset(c, 0, sizeof *c);

	/* Advise the kernel only if this path will read() (wc.c:396-398);
	 * bytes-only advises inside its own read fallback. */
	if (!bytes_only)
		tal_fadvise_seq(fd);

	if (bytes_only)
		return count_bytes_only(fd, fst, c);
	if (o->words)
		return count_words(fd, o, c);
	return count_lines(fd, o, c); /* chars/-L gated off in main */
}
