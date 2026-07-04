#include <stdlib.h>

#include "options.h"
#include "tests/unit/test.h"

/* Happy paths only: error paths print-and-exit and are asserted byte-exact by
 * the golden suite instead. */

static struct options o;

static void parse(char **argv)
{
	int argc = 0;

	while (argv[argc])
		argc++;
	free(o.files);
	options_parse(&o, argc, argv);
}

int main(void)
{
	parse((char *[]){ "tally", "f1", NULL });
	CHECK("default lwc", o.lines && o.words && o.bytes);
	CHECK("default no chars/L", !o.chars && !o.linelength);
	CHECK_SZ("one operand", o.nfiles, 1);
	CHECK_STR("operand", o.files[0], "f1");

	parse((char *[]){ "tally", "-lc", "f1", NULL });
	CHECK("bundle -lc", o.lines && o.bytes && !o.words);

	parse((char *[]){ "tally", "-l", "-c", "f1", NULL });
	CHECK("separate flags", o.lines && o.bytes && !o.words);

	parse((char *[]){ "tally", "--lines", "f1", NULL });
	CHECK("long exact", o.lines && !o.words && !o.bytes);

	parse((char *[]){ "tally", "--line", "f1", NULL });
	CHECK("long abbrev", o.lines && !o.words);

	parse((char *[]){ "tally", "--max", "f1", NULL });
	CHECK("abbrev max-line-length", o.linelength);

	parse((char *[]){ "tally", "f1", "-l", NULL });
	CHECK("permutation", o.lines && !o.words);
	CHECK_SZ("permutation operand", o.nfiles, 1);

	parse((char *[]){ "tally", "--", "-l", NULL });
	CHECK("-- ends options", o.lines && o.words && o.bytes);
	CHECK_STR("-l is operand after --", o.files[0], "-l");

	parse((char *[]){ "tally", "-", NULL });
	CHECK_STR("dash operand", o.files[0], "-");
	CHECK("dash is not an option", o.lines && o.words && o.bytes);

	parse((char *[]){ "tally", "--total=al", "f1", NULL });
	CHECK("argmatch abbrev always", o.total == TOTAL_ALWAYS);

	parse((char *[]){ "tally", "--total=n", "f1", NULL });
	CHECK("argmatch abbrev never", o.total == TOTAL_NEVER);

	parse((char *[]){ "tally", "--tot=only", "f1", "f2", NULL });
	CHECK("long+arg abbrev", o.total == TOTAL_ONLY);
	CHECK_SZ("two operands", o.nfiles, 2);

	parse((char *[]){ "tally", "--files0-from=list", NULL });
	CHECK_STR("files0 =form", o.files_from, "list");

	parse((char *[]){ "tally", "--files0-from", "list", NULL });
	CHECK_STR("files0 next-arg form", o.files_from, "list");
	CHECK_SZ("arg consumed", o.nfiles, 0);

	parse((char *[]){ "tally", "--debug", "-l", "f1", NULL });
	CHECK("debug flag", o.debug && o.lines);

	setenv("POSIXLY_CORRECT", "1", 1);
	parse((char *[]){ "tally", "f1", "-l", NULL });
	CHECK("posix: -l after operand is an operand",
	      !o.lines || o.words); /* default -lwc applied, -l not consumed */
	CHECK_SZ("posix: both are operands", o.nfiles, 2);
	CHECK_STR("posix: second operand", o.files[1], "-l");
	unsetenv("POSIXLY_CORRECT");

	free(o.files);
	return test_summary("options_test");
}
