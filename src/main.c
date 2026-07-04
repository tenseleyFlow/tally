#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Count one input. FILE is NULL for bare stdin, "-" for the stdin operand,
 * else a path. Mirrors wc()/wc_file() (wc.c:372-723): partial counts print
 * BEFORE a read-error diagnostic; open errors print no counts line. */
static bool count_one(const char *file, const struct options *o,
		      struct fstatus *fst, int width)
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
			tal_error(errno, "%s", file);
			return false;
		}
	}

	int err = count_fd(fd, o, fst, &c);

	write_counts(&c, o, width, file);

	if (err) {
		tal_error(err, "%s", diag);
		ok = false;
	}
	if (fd != STDIN_FILENO && close(fd) != 0) {
		tal_error(errno, "%s", file);
		ok = false;
	}
	return ok;
}

/* get_input_fstatus (wc.c:731-751): skip the stat entirely for the
 * single-input single-counter case (width 1); otherwise stat by path
 * (stdin via fstat) so the width estimator and -c fast path share it. */
static void input_fstatus(const struct options *o, const char *file,
			  struct fstatus *fst)
{
	int ncounters = (int)o->lines + (int)o->words + (int)o->chars +
			(int)o->bytes + (int)o->linelength;

	if (ncounters == 1) {
		fst->failed = 1;
		return;
	}
	fst->failed = (!file || strcmp(file, "-") == 0)
			      ? fstat(STDIN_FILENO, &fst->st)
			      : stat(file, &fst->st);
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

	const char *file = o.nfiles ? o.files[0] : NULL;

	if (o.nfiles == 1 && file && !file[0]) {
		/* GNU diagnoses the zero-length name instead of open("") —
		 * wc.c:952-969 — and never counts, so no counter gate applies. */
		tal_error(0, "invalid zero-length file name");
		return 1;
	}

	/* Scaffolding gates, removed as the sprints land (04: multi-file,
	 * --total, --files0-from). */
	if (o.files_from)
		tal_die(2, 0, "--files0-from not implemented yet (sprint 04)");
	if (o.total != TOTAL_AUTO)
		tal_die(2, 0, "--total not implemented yet (sprint 04)");
	if (o.nfiles > 1)
		tal_die(2, 0, "multiple files not implemented yet (sprint 04)");

	bool ok = true;

	{
		struct fstatus fst;

		input_fstatus(&o, file, &fst);
		int width = compute_number_width(1, &fst);

		ok = count_one(file, &o, &fst, width);
	}

	if (have_read_stdin && close(STDIN_FILENO) != 0)
		tal_die(1, errno, "-");
	return ok ? 0 : 1;
}
