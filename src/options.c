#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "options.h"
#include "util.h"
#include "version.h"

/* Diagnostic layers match the ref (audit 01 §option parsing): getopt-layer
 * messages prefix argv[0] verbatim and use plain apostrophes; argmatch-layer
 * messages prefix the program basename and use locale quotes. */

enum { OPT_DEBUG = 256, OPT_FILES0, OPT_TOTAL, OPT_HELP, OPT_VERSION };

static const struct longopt {
	const char *name;
	bool has_arg;
	int code; /* short char or OPT_* */
} longopts[] = {
	{ "bytes", false, 'c' },
	{ "chars", false, 'm' },
	{ "lines", false, 'l' },
	{ "words", false, 'w' },
	{ "debug", false, OPT_DEBUG },
	{ "files0-from", true, OPT_FILES0 },
	{ "max-line-length", false, 'L' },
	{ "total", true, OPT_TOTAL },
	{ "help", false, OPT_HELP },
	{ "version", false, OPT_VERSION },
};
#define NLONG (sizeof longopts / sizeof longopts[0])

static void try_help(const char *argv0)
{
	fprintf(stderr, "Try '%s --help' for more information.\n", argv0);
	exit(1);
}

static void usage_ok(void)
{
	printf("Usage: tally [OPTION]... [FILE]...\n"
	       "  or:  tally [OPTION]... --files0-from=F\n"
	       "Print newline, word, and byte counts for each FILE, and a total line if\n"
	       "more than one FILE is specified.  A word is a nonempty sequence of non white\n"
	       "space delimited by white space characters or by start or end of input.\n"
	       "\n"
	       "With no FILE, or when FILE is -, read standard input.\n"
	       "\n"
	       "The options below may be used to select which counts are printed, always in\n"
	       "the following order: newline, word, character, byte, maximum line length.\n"
	       "  -c, --bytes            print the byte counts\n"
	       "  -m, --chars            print the character counts\n"
	       "  -l, --lines            print the newline counts\n"
	       "      --debug            indicate what line count acceleration is used\n"
	       "      --files0-from=F    read input from the files specified by\n"
	       "                           NUL-terminated names in file F;\n"
	       "                           If F is -, read names from standard input\n"
	       "  -L, --max-line-length  print the maximum display width\n"
	       "  -w, --words            print the word counts\n"
	       "      --total=WHEN       when to print a line with total counts;\n"
	       "                           WHEN can be: auto, always, only, never\n"
	       "      --help             display this help and exit\n"
	       "      --version          output version information and exit\n"
	       "\n"
	       "tally is a drop-in reimplementation of GNU wc(1).\n");
	exit(0);
}

static void set_counter(struct options *o, int code)
{
	switch (code) {
	case 'c': o->bytes = true; break;
	case 'm': o->chars = true; break;
	case 'l': o->lines = true; break;
	case 'w': o->words = true; break;
	case 'L': o->linelength = true; break;
	default: break;
	}
}

static void parse_short(struct options *o, const char *argv0, const char *arg)
{
	for (const char *p = arg + 1; *p; p++) {
		switch (*p) {
		case 'c': case 'm': case 'l': case 'w': case 'L':
			set_counter(o, *p);
			break;
		default:
			fprintf(stderr, "%s: invalid option -- '%c'\n", argv0, *p);
			try_help(argv0);
		}
	}
}

/* gnulib-argmatch semantics: exact match wins; a prefix matching several
 * candidates is ambiguous only if they map to distinct values. */
static void parse_total(struct options *o, const char *argv0, const char *val)
{
	static const char *const names[] = { "auto", "always", "only", "never" };
	static const enum tal_total vals[] = { TOTAL_AUTO, TOTAL_ALWAYS,
					       TOTAL_ONLY, TOTAL_NEVER };
	size_t len = strlen(val);
	int hit = -1;
	bool ambiguous = false;

	for (size_t i = 0; i < 4; i++) {
		if (strncmp(names[i], val, len) != 0)
			continue;
		if (names[i][len] == '\0') { /* exact */
			hit = (int)i;
			ambiguous = false;
			break;
		}
		if (hit < 0)
			hit = (int)i;
		else if (vals[hit] != vals[i])
			ambiguous = true;
	}
	if (hit >= 0 && !ambiguous) {
		o->total = vals[hit];
		return;
	}
	fprintf(stderr, "%s: %s argument %s%s%s for %s--total%s\n", tal_prog,
		ambiguous ? "ambiguous" : "invalid", tal_qs(), val, tal_qe(),
		tal_qs(), tal_qe());
	fprintf(stderr, "Valid arguments are:\n");
	for (size_t i = 0; i < 4; i++)
		fprintf(stderr, "  - %s%s%s\n", tal_qs(), names[i], tal_qe());
	try_help(argv0);
}

static void dispatch(struct options *o, const char *argv0, int code, const char *val)
{
	switch (code) {
	case 'c': case 'm': case 'l': case 'w': case 'L':
		set_counter(o, code);
		break;
	case OPT_DEBUG:
		o->debug = true;
		break;
	case OPT_FILES0:
		o->files_from = val;
		break;
	case OPT_TOTAL:
		parse_total(o, argv0, val);
		break;
	case OPT_HELP:
		usage_ok();
		break;
	case OPT_VERSION:
		printf("tally %s\n", TAL_VERSION);
		exit(0);
	default:
		break;
	}
}

/* Returns the (possibly advanced) argv index. */
static int parse_long(struct options *o, int argc, char **argv, int i)
{
	const char *argv0 = argv[0];
	const char *arg = argv[i] + 2; /* past "--" */
	const char *eq = strchr(arg, '=');
	size_t namelen = eq ? (size_t)(eq - arg) : strlen(arg);
	const struct longopt *hit = NULL;
	size_t nhits = 0;

	for (size_t k = 0; k < NLONG; k++) {
		if (strncmp(longopts[k].name, arg, namelen) != 0)
			continue;
		if (longopts[k].name[namelen] == '\0') { /* exact */
			hit = &longopts[k];
			nhits = 1;
			break;
		}
		hit = &longopts[k];
		nhits++;
	}
	if (nhits > 1) {
		/* Unreachable with the current table (all prefixes unique);
		 * glibc format kept for safety. */
		fprintf(stderr, "%s: option '%s' is ambiguous; possibilities:",
			argv0, argv[i]);
		for (size_t k = 0; k < NLONG; k++)
			if (strncmp(longopts[k].name, arg, namelen) == 0)
				fprintf(stderr, " '--%s'", longopts[k].name);
		fputc('\n', stderr);
		try_help(argv0);
	}
	if (nhits == 0) {
		fprintf(stderr, "%s: unrecognized option '%s'\n", argv0, argv[i]);
		try_help(argv0);
	}

	const char *val = NULL;
	if (hit->has_arg) {
		if (eq)
			val = eq + 1;
		else if (i + 1 < argc)
			val = argv[++i];
		else {
			fprintf(stderr, "%s: option '--%s' requires an argument\n",
				argv0, hit->name);
			try_help(argv0);
		}
	} else if (eq) {
		fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
			argv0, hit->name);
		try_help(argv0);
	}
	dispatch(o, argv0, hit->code, val);
	return i;
}

void options_parse(struct options *o, int argc, char **argv)
{
	bool no_more = false;
	bool posix = getenv("POSIXLY_CORRECT") != NULL;

	memset(o, 0, sizeof *o);
	o->total = TOTAL_AUTO;
	o->files = xmalloc((size_t)(argc > 1 ? argc : 1) * sizeof *o->files);

	for (int i = 1; i < argc; i++) {
		char *arg = argv[i];

		if (no_more || arg[0] != '-' || arg[1] == '\0') {
			o->files[o->nfiles++] = arg;
			if (posix)
				no_more = true; /* POSIX: first operand ends options */
			continue;
		}
		if (strcmp(arg, "--") == 0) {
			no_more = true;
			continue;
		}
		if (arg[1] == '-')
			i = parse_long(o, argc, argv, i);
		else
			parse_short(o, argv[0], arg);
	}

	if (!(o->lines || o->words || o->chars || o->bytes || o->linelength))
		o->lines = o->words = o->bytes = true;
}
