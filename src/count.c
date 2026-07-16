#include <errno.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "count.h"
#include "io.h"
#include "mt.h"
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
#if TAL_HAS_AVX512
	if (tal_cpu_has_avx512bw()) {
		fn = tal_nlcount_avx512;
		name = "avx512";
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

/* Guarded regions live in leaf helpers: gcc's -Wclobbered rightly objects
 * to mutable locals sharing a frame with sigsetjmp. */
static int mapped_lines(nl_fn nl, const struct tal_map *m, struct counts *c)
{
	if (sigsetjmp(tal_sigbus_jmp, 1))
		return EIO; /* file truncated under the mapping */
	tal_sigbus_armed = 1;
	c->lines += nl(m->data, m->len);
	c->bytes += m->len;
	tal_sigbus_armed = 0;
	return 0;
}

static int count_lines(int fd, const struct options *o, struct counts *c)
{
	static nl_fn nl; /* selected once per process */
	struct tal_map m;

	if (!nl)
		nl = pick_nl_kernel(o->debug);

	if (o->threads > 1) {
		int r = tal_count_lines_mt(fd, o->threads, nl, &c->lines,
					   &c->bytes);

		if (r >= 0) {
			if (o->debug)
				fprintf(stderr, "%s: counted with %d threads\n",
					tal_prog, o->threads);
			return r;
		}
	}
	if (tal_map_acquire(fd, &m)) {
		int err = mapped_lines(nl, &m, c);

		tal_map_release(&m);
		return err;
	}

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

/* Word counting (+ fused lines), the audit-02 driver. The kernels handle
 * multibyte separators in-vector (L1) and hold back a 0-2 byte tail when a
 * potential separator can't be verified locally; held bytes go through the
 * scalar oracle's pend machinery and the next chunk resumes after a short
 * scalar prefix. Non-UTF-8 multibyte locales and pathological byte sets:
 * scalar throughout. */

typedef size_t (*lwc_fn)(const unsigned char *, size_t, unsigned,
			 struct lwc_out *);

static lwc_fn pick_lwc_kernel(bool debug)
{
	lwc_fn fn = NULL;
	const char *name = "scalar";

	if (tal_ws.luts_ok && (!tal_ws.multibyte ||
			       (tal_ws.utf8 && tal_ws.l1_ok))) {
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
			fn && tal_ws.multibyte ? " (l1 patterns)" : "");
	return fn;
}

typedef size_t (*u8_fn)(const unsigned char *, size_t, unsigned long long *,
			unsigned long long *);
typedef size_t (*lscan_fn)(const unsigned char *, size_t,
			   unsigned long long *, unsigned long long *);

static lscan_fn pick_lscan_kernel(bool debug)
{
	lscan_fn fn = NULL;
	const char *name = "scalar";

	/* The scanner assumes 0x20-0x7E are width-1 printable characters:
	 * true in single-byte locales and UTF-8 (never continuations), false
	 * in shift encodings — those take the oracle. */
	if (!tal_ws.multibyte || tal_ws.utf8) {
#if TAL_HAS_SSE2 && defined(__SSE2__)
		fn = tal_lscan_sse2;
		name = "sse2";
#elif TAL_HAS_NEON && defined(__ARM_NEON)
		fn = tal_lscan_neon;
		name = "neon";
#endif
#if TAL_HAS_AVX2
		if (tal_cpu_has_avx2()) {
			fn = tal_lscan_avx2;
			name = "avx2";
		}
#endif
	}
	if (debug)
		fprintf(stderr, "%s: using %s width scanner\n", tal_prog,
			name);
	return fn;
}

/* One chunk of the -L pass. The scanner handles printable-ASCII spans;
 * special bytes (tabs, CR/FF, multibyte, controls) take a small oracle
 * window, which owns the full width semantics via *lst. Only linelength is
 * taken from this pass; the window's word/char/line counts land in the
 * throwaway *ltmp. */
static void lscan_chunk(lscan_fn lk, const unsigned char *p, size_t len,
			struct counts *ltmp, struct wstate *lst)
{
	size_t off = 0;
	size_t win = 64;

	while (off < len) {
		if (lst->npend == 0) {
			size_t used = lk(p + off, len - off, &lst->linepos,
					 &ltmp->linelength);

			off += used;
			if (off >= len)
				break;
			/* Scanner progress means ASCII text resumed: shrink
			 * the window back. Zero progress (multibyte-dense
			 * text) grows it — a fixed 64B window ping-pongs
			 * scanner<->oracle per CJK char and lost 9% to the
			 * ref on macOS. */
			win = used ? 64 : (win < 8192 ? win * 2 : win);
		}
		size_t step = len - off < win ? len - off : win;

		if (tal_ws.utf8)
			tal_lwalk(p + off, step, ltmp, lst);
		else if (tal_ws.multibyte)
			tal_swc_mb(p + off, step, ltmp, lst);
		else
			tal_swc_sb(p + off, step, ltmp, lst);
		off += step;
	}
}

static u8_fn pick_u8_kernel(bool debug)
{
	u8_fn fn = NULL;
	const char *name = "scalar";

	if (tal_ws.utf8) {
#if TAL_HAS_SSE2 && defined(__SSE2__)
		fn = tal_u8count_sse2;
		name = "sse2";
#endif
#if TAL_HAS_NEON && defined(__ARM_NEON)
		fn = tal_u8count_neon;
		name = "neon";
#endif
#if TAL_HAS_AVX2
		if (tal_cpu_has_avx2()) {
			fn = tal_u8count_avx2;
			name = "avx2";
		}
#endif
	}
	if (debug)
		fprintf(stderr, "%s: using %s char kernel\n", tal_prog, name);
	return fn;
}

/* One chunk of the words path: fused kernel with scalar pend/hold windows. */
static void words_chunk(lwc_fn kern, const unsigned char *p, size_t len,
			struct counts *c, struct wstate *st)
{
	size_t off = 0;

	while (off < len) {
		if (st->npend) {
			/* Resolve carried decode bytes on a short scalar
			 * prefix, then resume the kernel. */
			size_t pre = len - off < 16 ? len - off : 16;

			tal_swc_mb(p + off, pre, c, st);
			off += pre;
			continue;
		}
		struct lwc_out out;
		size_t used = kern(p + off, len - off, !st->in_word, &out);

		c->lines += out.lines;
		c->words += out.words;
		st->in_word = !out.last_is_ws;
		off += used;
		if (off < len) {
			/* Held tail: feed a couple of bytes to the oracle —
			 * it pends an incomplete sequence for the next chunk
			 * or EOF finish. Always advances, so adversarial
			 * all-suspect input degrades to scalar, never loops. */
			size_t hold = len - off < 2 ? len - off : 2;

			tal_swc_mb(p + off, hold, c, st);
			off += hold;
		}
	}
}

/* One chunk of the -m path: validated char kernel; rejected or held spans
 * take the scalar oracle (GNU's byte-at-a-time error resync), whose word
 * counts land in the throwaway *tmp. */
static void chars_chunk(u8_fn u8k, const unsigned char *p, size_t len,
			struct counts *tmp, struct wstate *cst)
{
	size_t off = 0;

	while (off < len) {
		if (cst->npend) {
			size_t pre = len - off < 16 ? len - off : 16;

			tal_u8scalar(p + off, pre, tmp, cst);
			off += pre;
			continue;
		}
		size_t used = u8k(p + off, len - off, &tmp->chars,
				  &tmp->lines);

		off += used;
		if (off < len) {
			/* Only the kernel's lookahead tail (<= 34 bytes) and
			 * chunk-boundary pends land here. */
			size_t step = len - off < 1024 ? len - off : 1024;

			tal_u8scalar(p + off, step, tmp, cst);
			off += step;
		}
	}
}

/* Pass configuration + states bundled so the sigsetjmp frame (the mapped
 * helper) holds no mutable locals (gcc -Wclobbered). */
struct gpasses {
	lwc_fn kern;
	u8_fn u8kern;
	lscan_fn lkern;
	nl_fn nlk;
	bool scalar_mode, words_pass, chars_pass, l_pass, nl_pass;
	struct counts *c;
	struct wstate *st;
	struct counts *ctmp;
	struct wstate *cst;
	struct counts *ltmp;
	struct wstate *lst;
};

/* One contiguous span per pass: no chunk-boundary carries. */
static int mapped_general(const struct gpasses *g, const struct tal_map *m)
{
	if (sigsetjmp(tal_sigbus_jmp, 1))
		return EIO; /* truncated under the mapping */
	tal_sigbus_armed = 1;
	g->c->bytes += m->len;
	if (g->scalar_mode) {
		if (tal_ws.multibyte)
			tal_swc_mb(m->data, m->len, g->c, g->st);
		else
			tal_swc_sb(m->data, m->len, g->c, g->st);
	} else {
		if (g->words_pass)
			words_chunk(g->kern, m->data, m->len, g->c, g->st);
		if (g->chars_pass)
			chars_chunk(g->u8kern, m->data, m->len, g->ctmp,
				    g->cst);
		if (g->l_pass)
			lscan_chunk(g->lkern, m->data, m->len, g->ltmp,
				    g->lst);
		if (g->nl_pass)
			g->c->lines += g->nlk(m->data, m->len);
	}
	tal_sigbus_armed = 0;
	return 0;
}

/* The general path: words, and/or chars (multibyte), and/or -L. Fused
 * lines+words kernel plus (when -m) the validated char kernel as a second
 * pass over the hot chunk; -L and non-UTF-8 charsets take the scalar oracle,
 * which computes everything in one walk. */
static int count_general(int fd, const struct options *o, struct counts *c,
			 bool count_chars)
{
	static bool ws_ready;
	static lwc_fn kern;
	static u8_fn u8kern;
	static lscan_fn lkern;
	static nl_fn nlk;
	struct wstate st, cst, lst;
	struct counts ctmp, ltmp;

	if (!ws_ready) {
		ws_init(&tal_ws);
		kern = pick_lwc_kernel(o->debug);
		if (MB_CUR_MAX > 1 && o->chars)
			u8kern = pick_u8_kernel(o->debug);
		if (o->linelength)
			lkern = pick_lscan_kernel(o->debug);
		nlk = pick_nl_kernel(false);
		ws_ready = true;
	}
	wstate_init(&st);
	wstate_init(&cst);
	wstate_init(&lst);
	memset(&ctmp, 0, sizeof ctmp);
	memset(&ltmp, 0, sizeof ltmp);
	st.width = o->linelength;
	lst.width = true;

	bool words_pass = o->words && kern;
	bool chars_pass = count_chars && u8kern;
	bool l_pass = o->linelength && lkern;
	/* Anything a kernel can't cover sends the whole job to the oracle. */
	bool scalar_mode = (o->words && !words_pass) ||
			   (count_chars && !chars_pass) ||
			   (o->linelength && !l_pass);
	/* Who counts lines: words kernel > chars kernel > a dedicated pass. */
	bool nl_pass = !scalar_mode && o->lines && !words_pass && !chars_pass;

	struct gpasses gp = { kern, u8kern, lkern, nlk, scalar_mode,
			      words_pass, chars_pass, l_pass, nl_pass,
			      c, &st, &ctmp, &cst, &ltmp, &lst };
	struct tal_map m;
	int maperr = 0;

	if (tal_map_acquire(fd, &m)) {
		maperr = mapped_general(&gp, &m);
		tal_map_release(&m);
	} else for (;;) {
		ssize_t got = tal_read(fd, buf, TAL_IO_BUFSIZE);

		if (got < 0)
			return errno;
		if (got == 0)
			break;
		c->bytes += (unsigned long long)got;

		size_t len = (size_t)got;

		if (scalar_mode) {
			if (tal_ws.multibyte)
				tal_swc_mb(buf, len, c, &st);
			else
				tal_swc_sb(buf, len, c, &st);
			continue;
		}
		if (words_pass)
			words_chunk(kern, buf, len, c, &st);
		if (chars_pass)
			chars_chunk(u8kern, buf, len, &ctmp, &cst);
		if (l_pass)
			lscan_chunk(lkern, buf, len, &ltmp, &lst);
		if (nl_pass)
			c->lines += nlk(buf, len);
	}
	if (scalar_mode) {
		tal_swc_finish(c, &st);
	} else {
		if (words_pass)
			tal_swc_finish(c, &st);
		if (chars_pass) {
			tal_swc_finish(&ctmp, &cst);
			c->chars += ctmp.chars;
			if (!words_pass)
				c->lines += ctmp.lines;
		}
		if (l_pass) {
			tal_swc_finish(&ltmp, &lst);
			if (ltmp.linelength > c->linelength)
				c->linelength = ltmp.linelength;
		}
	}
	return maperr;
}

int count_fd(int fd, const struct options *o, struct fstatus *fst,
	     struct counts *c)
{
	/* GNU's counter derivation (wc.c:384-394): in single-byte locales
	 * chars are bytes, so -m rides the byte machinery (including the
	 * zero-read fstat path when nothing else is requested). */
	bool mb = MB_CUR_MAX > 1;
	bool count_bytes = o->bytes || (!mb && o->chars);
	bool count_chars = mb && o->chars;
	bool complicated = o->words || o->linelength;
	int err;

	memset(c, 0, sizeof *c);

	if (count_bytes && !count_chars && !o->lines && !complicated) {
		err = count_bytes_only(fd, fst, c);
	} else {
		/* Advise the kernel only if this path will read()
		 * (wc.c:396-398). */
		tal_fadvise_seq(fd);
		if (!count_chars && !complicated)
			err = count_lines(fd, o, c);
		else
			err = count_general(fd, o, c, count_chars);
	}

	if (o->chars && !mb)
		c->chars = c->bytes; /* wc.c:676-677 */
	return err;
}
