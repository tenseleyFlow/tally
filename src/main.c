#include <stdio.h>
#include <string.h>

#include "version.h"

/* Sprint 00 stub: the scaffold builds and the harness runs before any wc
 * behavior exists. Real argv handling arrives with src/options.c (sprint 01). */
int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--version") == 0) {
		printf("tally %s\n", TAL_VERSION);
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "--help") == 0) {
		printf("Usage: tally [OPTION]... [FILE]...\n"
		       "Stub build: counting arrives in sprint 01.\n");
		return 0;
	}
	fprintf(stderr, "tally: not implemented (sprint 00 stub)\n");
	return 1;
}
