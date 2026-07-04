#ifndef TAL_OPTIONS_H
#define TAL_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>

enum tal_total {
	TOTAL_AUTO,   /* default: total line iff >1 name processed */
	TOTAL_ALWAYS,
	TOTAL_ONLY,   /* per-file lines suppressed, width 1, no label */
	TOTAL_NEVER
};

struct options {
	bool lines, words, chars, bytes, linelength;
	bool debug;
	const char *files_from; /* --files0-from=F, or NULL */
	enum tal_total total;
	char **files;  /* operands in command-line order (pointers into argv) */
	size_t nfiles;
};

/* Parse argv GNU-getopt-style: argument permutation (disabled when
 * POSIXLY_CORRECT is set: first operand ends option scanning), unambiguous
 * long-option abbreviation, bundled shorts, "--", "=arg" and next-arg forms.
 * Diagnostics match glibc getopt / gnulib argmatch byte-for-byte modulo the
 * program-name token (audit 01). Exits 1 on any parse error (after printing
 * the Try-help line); handles --help/--version (exit 0). On return, o->files
 * holds the operands and the default -lwc has been applied if no counter flag
 * was given. */
void options_parse(struct options *o, int argc, char **argv);

#endif /* TAL_OPTIONS_H */
