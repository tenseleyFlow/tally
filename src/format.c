#include <stdio.h>

#include "format.h"
#include "util.h"

int compute_number_width(size_t nfiles, const struct fstatus *fst)
{
	int width = 1;

	if (nfiles > 0 && fst[0].failed <= 0) {
		int minimum_width = 1;
		unsigned long long total = 0;

		for (size_t i = 0; i < nfiles; i++) {
			if (fst[i].failed)
				continue; /* failed stats are simply ignored */
			if (!S_ISREG(fst[i].st.st_mode))
				minimum_width = 7;
			else if (fst[i].st.st_size > 0) {
				unsigned long long sz =
					(unsigned long long)fst[i].st.st_size;

				if (total + sz < total) { /* saturate */
					total = (unsigned long long)-1;
					break;
				}
				total += sz;
			}
		}
		for (; total >= 10; total /= 10)
			width++;
		if (width < minimum_width)
			width = minimum_width;
	}
	return width;
}

void write_counts(const struct counts *c, const struct options *o, int width,
		  const char *file)
{
	char nbuf[TAL_U64_BUFSIZE];
	const char *fmt = "%*s";

	if (o->lines) {
		printf(fmt, width, tal_u64tostr(c->lines, nbuf));
		fmt = " %*s";
	}
	if (o->words) {
		printf(fmt, width, tal_u64tostr(c->words, nbuf));
		fmt = " %*s";
	}
	if (o->chars) {
		printf(fmt, width, tal_u64tostr(c->chars, nbuf));
		fmt = " %*s";
	}
	if (o->bytes) {
		printf(fmt, width, tal_u64tostr(c->bytes, nbuf));
		fmt = " %*s";
	}
	if (o->linelength)
		printf(fmt, width, tal_u64tostr(c->linelength, nbuf));
	if (file)
		printf(" %s", file);
	putchar('\n');
}
