#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "count.h"
#include "format.h"
#include "options.h"
#include "util.h"

static bool have_read_stdin;

/* gnulib close_stdout: a write error anywhere (including one only visible at
 * flush/close) must be diagnosed and turn the exit status to 1. _exit avoids
 * re-entering atexit. */
static void close_stdout(void)
{
	int err = 0;

	if (fflush(stdout) != 0 || ferror(stdout))
		err = errno ? errno : -1;
	if (fclose(stdout) != 0 && !err)
		err = errno ? errno : -1;
	if (err) {
		if (err > 0)
			tal_error(err, "write error");
		else
			tal_error(0, "write error");
		_exit(1);
	}
}

/* Count one input. FILE is NULL for bare stdin, "-" for the stdin operand
 * (also honored inside --files0-from lists, audit 00 claim 13), else a path.
 * Mirrors wc()/wc_file() (wc.c:372-723): partial counts print BEFORE a
 * read-error diagnostic; open errors print no counts line; --total=only
 * suppresses the per-file row (wc.c:679). *out receives the counts for
 * totals accumulation. */
static bool count_one(const char *file, const struct options *o,
		      struct fstatus *fst, int width, struct counts *out)
{
	const char *diag = file ? file : "standard input";
	struct counts c;
	int fd;
	bool ok = true;

	if (!file || strcmp(file, "-") == 0) {
		have_read_stdin = true;
		fd = STDIN_FILENO;
	} else {
		fd = open(file, O_RDONLY);
		if (fd < 0) {
			tal_error(errno, "%s", tal_quotef(file));
			memset(out, 0, sizeof *out);
			return false;
		}
	}

	int err = count_fd(fd, o, fst, &c);

	if (o->total != TOTAL_ONLY)
		write_counts(&c, o, width, file);

	if (err) {
		tal_error(err, "%s", tal_quotef(diag));
		ok = false;
	}
	if (fd != STDIN_FILENO && close(fd) != 0) {
		tal_error(errno, "%s", tal_quotef(file));
		ok = false;
	}
	*out = c;
	return ok;
}

/* get_input_fstatus (wc.c:731-751): skip the stat when it cannot matter —
 * unknown name count (streamed lists) or a single input with a single
 * counter (width 1 either way); otherwise stat by path (stdin via fstat) so
 * the width estimator and the -c fast path share one stat. */
static struct fstatus *input_fstatus(const struct options *o, size_t nfiles,
				     char **files)
{
	struct fstatus *fst = xmalloc((nfiles ? nfiles : 1) * sizeof *fst);
	int ncounters = (int)o->lines + (int)o->words + (int)o->chars +
			(int)o->bytes + (int)o->linelength;

	if (nfiles == 0 || (nfiles == 1 && ncounters == 1)) {
		fst[0].failed = 1;
		return fst;
	}
	for (size_t i = 0; i < nfiles; i++) {
		const char *f = files ? files[i] : NULL;

		fst[i].failed = (!f || strcmp(f, "-") == 0)
					? fstat(STDIN_FILENO, &fst[i].st)
					: stat(f, &fst[i].st);
	}
	return fst;
}

static bool add_sat(unsigned long long *a, unsigned long long b)
{
	if (__builtin_add_overflow(*a, b, a)) {
		*a = ULLONG_MAX;
		return true;
	}
	return false;
}

struct run {
	const struct options *o;
	struct fstatus *fst;
	int width;
	struct counts tot;
	bool ovf[4]; /* lines, words, chars, bytes total overflow */
	size_t processed;
	bool ok;
	bool files_from_stdin; /* --files0-from=- */
};

/* One name from argv or a files0 list. IDX indexes the fstatus array for
 * pre-stat'ed inputs; pass with fst[idx].failed reset for streamed names. */
static void run_name(struct run *r, char *name, size_t idx)
{
	r->processed++;

	if (name && r->files_from_stdin && strcmp(name, "-") == 0) {
		/* printf - | wc --files0-from=-  (wc.c:942-950) */
		tal_error(0,
			  "when reading file names from standard input, "
			  "no file name of %s allowed", tal_quoteaf(name));
		r->ok = false;
		return;
	}
	if (name && !name[0]) {
		if (r->o->files_from)
			tal_error(0, "%s:%zu: invalid zero-length file name",
				  tal_quotef(r->o->files_from), r->processed);
		else
			tal_error(0, "invalid zero-length file name");
		r->ok = false;
		return;
	}

	struct counts c;

	if (!count_one(name, r->o, &r->fst[idx], r->width, &c))
		r->ok = false;
	r->ovf[0] |= add_sat(&r->tot.lines, c.lines);
	r->ovf[1] |= add_sat(&r->tot.words, c.words);
	r->ovf[2] |= add_sat(&r->tot.chars, c.chars);
	r->ovf[3] |= add_sat(&r->tot.bytes, c.bytes);
	if (c.linelength > r->tot.linelength)
		r->tot.linelength = c.linelength; /* -L total is the max */
}

/* Read a files0 list into tokens (readtokens0 semantics: NUL-terminated,
 * a final unterminated token still counts, empty input yields none). */
static char **slurp_list(FILE *f, const char *name, size_t size,
			 size_t *ntok, char **bufout)
{
	char *data = xmalloc(size + 1);
	size_t got = fread(data, 1, size, f);

	if (ferror(f))
		tal_die(1, 0, "cannot read file names from %s", tal_quoteaf(name));
	data[got] = '\0';

	size_t n = 0;

	for (size_t i = 0; i < got; i++)
		if (data[i] == '\0')
			n++;
	if (got && data[got - 1] != '\0')
		n++; /* final token without NUL */

	char **tok = xmalloc((n ? n : 1) * sizeof *tok);
	size_t k = 0, start = 0;

	for (size_t i = 0; i <= got && k < n; i++) {
		if (i == got || data[i] == '\0') {
			tok[k++] = data + start;
			start = i + 1;
		}
	}
	*ntok = n;
	*bufout = data;
	return tok;
}

int main(int argc, char **argv)
{
	struct options o;

	setlocale(LC_ALL, "");
	tal_set_program(argv[0]);
	atexit(close_stdout);
	/* Line-buffer stdout so concurrent runs interleave whole rows
	 * (wc.c:810-812). */
	setvbuf(stdout, NULL, _IOLBF, 0);

	options_parse(&o, argc, argv);

	char **files = o.files;
	size_t nfiles = o.nfiles;
	FILE *fstream = NULL;
	char **slurped = NULL;
	char *slurpbuf = NULL;
	bool streamed = false;

	if (o.files_from) {
		if (o.nfiles) {
			/* wc.c:876-884; second line has no program prefix. */
			tal_error(0, "extra operand %s", tal_quoteaf(o.files[0]));
			fprintf(stderr, "file operands cannot be combined "
					"with --files0-from\n");
			fprintf(stderr,
				"Try '%s --help' for more information.\n",
				argv[0]);
			return 1;
		}
		if (strcmp(o.files_from, "-") == 0)
			fstream = stdin;
		else {
			fstream = fopen(o.files_from, "r");
			if (!fstream)
				tal_die(1, errno,
					"cannot open %s for reading",
					tal_quoteaf(o.files_from));
		}

		/* Slurp when the list is a reasonably sized regular file so
		 * names can be stat'ed for the width estimate; else stream
		 * one name at a time with width 1 (wc.c:896-917). */
		struct stat st;
		unsigned long long phys = 0;
		long pages = sysconf(_SC_PHYS_PAGES);
		long psize = sysconf(_SC_PAGESIZE);

		if (pages > 0 && psize > 0)
			phys = (unsigned long long)pages *
			       (unsigned long long)psize;
		unsigned long long cap = 10ULL * 1024 * 1024;

		if (phys / 2 < cap)
			cap = phys / 2;
		if (fstat(fileno(fstream), &st) == 0 && S_ISREG(st.st_mode) &&
		    st.st_size >= 0 && (unsigned long long)st.st_size <= cap) {
			slurped = slurp_list(fstream, o.files_from,
					     (size_t)st.st_size, &nfiles,
					     &slurpbuf);
			/* GNU fcloses the slurped stream even when it is
			 * stdin (wc.c:905) — observable: fstat(STDIN) then
			 * fails for "-" entries, so the width estimator
			 * skips them. Replicate. */
			fclose(fstream);
			fstream = NULL;
			files = slurped;
		} else {
			streamed = true;
			nfiles = 0;
			files = NULL;
		}
	} else if (nfiles == 0) {
		/* Bare stdin behaves as one unnamed input (wc.c:921-924). */
		static char *stdin_only[] = { NULL };

		files = stdin_only;
		nfiles = 1;
	}

	struct run r;

	memset(&r, 0, sizeof r);
	r.o = &o;
	r.fst = input_fstatus(&o, streamed ? 0 : nfiles, files);
	r.ok = true;
	r.files_from_stdin = o.files_from &&
			     strcmp(o.files_from, "-") == 0;
	r.width = o.total == TOTAL_ONLY
			  ? 1 /* no alignment requirement (wc.c:931-932) */
			  : compute_number_width(streamed ? 0 : nfiles, r.fst);

	if (streamed) {
		char *tok = NULL;
		size_t cap = 0;
		ssize_t len;

		while ((len = getdelim(&tok, &cap, '\0', fstream)) != -1) {
			if (len > 0 && tok[len - 1] == '\0')
				tok[len - 1] = '\0'; /* strip delimiter */
			r.fst[0].failed = 1; /* re-stat per streamed name */
			run_name(&r, tok, 0);
		}
		free(tok);
		if (ferror(fstream)) {
			tal_error(errno, "%s: read error", tal_quotef(o.files_from));
			r.ok = false;
		}
		if (fstream != stdin)
			fclose(fstream);
	} else {
		for (size_t i = 0; i < nfiles; i++)
			run_name(&r, files[i], i);
	}

	if (o.total != TOTAL_NEVER &&
	    (o.total != TOTAL_AUTO || r.processed > 1)) {
		/* Saturated totals are diagnosed like GNU (wc.c:1008-1031);
		 * unreachable without 2^64 input bytes, ported for fidelity. */
		static const char *const ovfname[] = {
			"total lines", "total words",
			"total characters", "total bytes"
		};
		for (int i = 0; i < 4; i++)
			if (r.ovf[i]) {
				tal_error(EOVERFLOW, "%s", ovfname[i]);
				r.ok = false;
			}

		struct counts t = r.tot;

		write_counts(&t, &o, r.width,
			     o.total == TOTAL_ONLY ? NULL : "total");
	}

	free(r.fst);
	free(slurped);
	free(slurpbuf);
	free(o.files);

	if (have_read_stdin && close(STDIN_FILENO) != 0)
		tal_die(1, errno, "-");
	return r.ok ? 0 : 1;
}
