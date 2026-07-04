#ifndef TAL_FORMAT_H
#define TAL_FORMAT_H

#include <stddef.h>

#include "count.h"
#include "options.h"

/* GNU wc's pre-read width estimator, compute_number_width (wc.c:759-788),
 * ported exactly — including the cases where actual counts outgrow the
 * estimate and columns misalign (classified quirk, audit 01). Width 1 when
 * no stat was taken (fstatus[0].failed > 0 or nfiles == 0); minimum 7 when
 * any stat'ed input is non-regular; else digits of the summed regular sizes. */
int compute_number_width(size_t nfiles, const struct fstatus *fst);

/* One output row: requested counters right-aligned to WIDTH in fixed order
 * (lines, words, chars, bytes, maxlinelen), then " FILE" when FILE is
 * non-NULL. Filename quoting for '\n' arrives in sprint 05. */
void write_counts(const struct counts *c, const struct options *o, int width,
		  const char *file);

#endif /* TAL_FORMAT_H */
