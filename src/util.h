#ifndef TAL_UTIL_H
#define TAL_UTIL_H

#include <stddef.h>

/* Buffer big enough for any unsigned 64-bit decimal plus NUL. */
#define TAL_U64_BUFSIZE 21

/* Program basename for diagnostics ("tally" normally; set from argv[0]). */
extern const char *tal_prog;
void tal_set_program(const char *argv0);

/* Locale quote glyphs, gnulib-quote-style: U+2018/U+2019 in UTF-8 locales,
 * apostrophes elsewhere. Call after setlocale(). Used by argmatch-layer
 * diagnostics; getopt-layer messages always use plain apostrophes. */
const char *tal_qs(void);
const char *tal_qe(void);

/* gnulib shell-escape quoting, the subset wc can emit (rules probed from the
 * ref, verified by the golden name fuzzer). quotef: quote only when needed
 * (diagnostics; also the stdout filename when it contains '\n'). quoteaf:
 * always quoted. Returns a static buffer — one live use per message. */
const char *tal_quotef(const char *name);
const char *tal_quoteaf(const char *name);

/* Render V in decimal into BUF (size TAL_U64_BUFSIZE); returns a pointer to the
 * first digit (renders right-aligned, glibc umaxtostr-style). snprintf-free:
 * write_counts sits on the hot path for many-file runs. */
char *tal_u64tostr(unsigned long long v, char *buf);

/* Diagnostics in coreutils error(3) format: "tally: FMT[: strerror(ERRNUM)]\n"
 * to stderr. errnum 0 suppresses the strerror suffix. Does not exit. */
void tal_error(int errnum, const char *fmt, ...);

/* tal_error, then exit(status). */
void tal_die(int status, int errnum, const char *fmt, ...);

/* malloc that dies with a diagnostic instead of returning NULL. */
void *xmalloc(size_t n);

#endif /* TAL_UTIL_H */
