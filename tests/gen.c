/* gen — deterministic fixture/fuzz byte-stream generator.
 * Usage: gen CLASS SEED NBYTES > out
 * Identical output for identical arguments on every platform: splitmix64 PRNG,
 * integer math only, no libc rand/locale. Shell/awk RNGs differ per box, which
 * would make golden corpora non-portable (audit 04).
 *
 * Classes:
 *   ascii    words of a-z separated by space/newline (prose-like)
 *   utf8     valid UTF-8: ASCII + Cyrillic (D0/D1) + CJK (E4-E9) + spaces/newlines
 *   e2       ascii prose salted with E2 80 9x typography (smart quotes/dashes)
 *   mbws     utf8 prose salted with multibyte whitespace (NBSP, U+2000-200A, U+3000)
 *   binary   uniform random bytes (invalid UTF-8 everywhere)
 *   ws       whitespace-dense ascii (all six POSIX blanks, few letters)
 *   lines    newline-dense: 1-3 byte lines
 *   longline single line of a-z/space, no newline until EOF
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long state;

static unsigned long long next64(void)
{
	unsigned long long z = (state += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

/* Unbiased-enough for fixtures: low 32 bits mod n. */
static unsigned rnd(unsigned n)
{
	return (unsigned)(next64() % n);
}

#define OUTCAP (1u << 16)
static unsigned char outbuf[OUTCAP + 8]; /* +8: multibyte emit slop before flush */
static size_t outlen;
static unsigned long long remaining;

static void flush(void)
{
	if (outlen) {
		fwrite(outbuf, 1, outlen, stdout);
		outlen = 0;
	}
}

/* Emit up to N bytes, truncated at the requested total (a multibyte sequence
 * may be cut mid-way at EOF — deliberate: truncated tails are a fixture class). */
static void emit(const unsigned char *b, size_t n)
{
	if (n > remaining)
		n = (size_t)remaining;
	memcpy(outbuf + outlen, b, n);
	outlen += n;
	remaining -= n;
	if (outlen >= OUTCAP)
		flush();
}

static void emit1(unsigned char c)
{
	emit(&c, 1);
}

static void emit_word(unsigned maxlen)
{
	unsigned n = 1 + rnd(maxlen);

	while (n-- && remaining)
		emit1((unsigned char)('a' + rnd(26)));
}

static void emit_cyrillic(void)
{
	unsigned char b[2];

	b[0] = (unsigned char)(0xD0 + rnd(2));
	b[1] = (unsigned char)(0x80 + rnd(0x30)); /* continuation range subset */
	emit(b, 2);
}

static void emit_cjk(void)
{
	unsigned char b[3];

	b[0] = (unsigned char)(0xE4 + rnd(6));
	b[1] = (unsigned char)(0x80 + rnd(0x40));
	b[2] = (unsigned char)(0x80 + rnd(0x40));
	emit(b, 3);
}

int main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: gen CLASS SEED NBYTES\n");
		return 2;
	}
	const char *cls = argv[1];
	state = strtoull(argv[2], NULL, 0);
	remaining = strtoull(argv[3], NULL, 0);

	if (strcmp(cls, "ascii") == 0) {
		while (remaining) {
			emit_word(9);
			emit1(rnd(8) ? ' ' : '\n');
		}
	} else if (strcmp(cls, "utf8") == 0) {
		while (remaining) {
			switch (rnd(4)) {
			case 0: emit_word(6); break;
			case 1: emit_cyrillic(); break;
			case 2: emit_cjk(); break;
			default: emit1(rnd(6) ? ' ' : '\n'); break;
			}
		}
	} else if (strcmp(cls, "e2") == 0) {
		static const unsigned char ty[4][3] = {
			{ 0xE2, 0x80, 0x98 }, /* U+2018 */
			{ 0xE2, 0x80, 0x99 }, /* U+2019 */
			{ 0xE2, 0x80, 0x93 }, /* U+2013 */
			{ 0xE2, 0x80, 0x94 }, /* U+2014 */
		};
		while (remaining) {
			emit_word(8);
			if (rnd(3) == 0)
				emit(ty[rnd(4)], 3);
			emit1(rnd(9) ? ' ' : '\n');
		}
	} else if (strcmp(cls, "mbws") == 0) {
		static const unsigned char sp3[3][3] = {
			{ 0xE2, 0x80, 0x83 }, /* U+2003 em space */
			{ 0xE3, 0x80, 0x80 }, /* U+3000 ideographic */
			{ 0xE2, 0x80, 0xA9 }, /* U+2029 para sep */
		};
		static const unsigned char nbsp[2] = { 0xC2, 0xA0 };
		while (remaining) {
			switch (rnd(5)) {
			case 0: emit_word(6); break;
			case 1: emit_cjk(); break;
			case 2: emit(nbsp, 2); break;
			case 3: emit(sp3[rnd(3)], 3); break;
			default: emit1(rnd(6) ? ' ' : '\n'); break;
			}
		}
	} else if (strcmp(cls, "binary") == 0) {
		while (remaining)
			emit1((unsigned char)rnd(256));
	} else if (strcmp(cls, "ws") == 0) {
		static const unsigned char blanks[6] = { ' ', '\t', '\n', '\v', '\f', '\r' };
		while (remaining) {
			if (rnd(5) == 0)
				emit1((unsigned char)('a' + rnd(26)));
			else
				emit1(blanks[rnd(6)]);
		}
	} else if (strcmp(cls, "lines") == 0) {
		while (remaining) {
			unsigned n = rnd(3);
			while (n-- && remaining)
				emit1((unsigned char)('a' + rnd(26)));
			emit1('\n');
		}
	} else if (strcmp(cls, "longline") == 0) {
		while (remaining)
			emit1(rnd(7) ? (unsigned char)('a' + rnd(26)) : ' ');
	} else {
		fprintf(stderr, "gen: unknown class '%s'\n", cls);
		return 2;
	}
	flush();
	if (fflush(stdout) != 0 || ferror(stdout))
		return 1;
	return 0;
}
