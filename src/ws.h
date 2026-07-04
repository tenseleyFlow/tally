#ifndef TAL_WS_H
#define TAL_WS_H

#include <stdbool.h>
#include <stddef.h>

#include "count.h"

/* Word-separator specification, derived from the RUNTIME libc — never
 * hardcoded Unicode tables (audit 00 claim 8: FreeBSD/glibc/musl disagree;
 * FreeBSD's C locale even makes byte 0xA0 a separator via the NBSP rule).
 * Semantics per audit 01: separator = isspace/iswspace in the locale, plus
 * the NBSP family {U+00A0, U+2007, U+202F, U+2060} unless POSIXLY_CORRECT
 * (wc.c:253-256, 866-868). Encoding errors are word constituents. */
struct ws_spec {
	bool posix_correct;
	bool multibyte; /* MB_CUR_MAX > 1 */
	bool utf8;      /* charset is UTF-8: SIMD word counting possible */

	/* Single-byte classification: in single-byte locales the whole rule;
	 * in multibyte locales the ASCII fast path (GNU's wc_isspace table). */
	unsigned char is_ws[256];

	/* SIMD byte-set machinery (Mula 2-pshufb: bit-per-hi-nibble-group).
	 * ws LUTs classify the separator byte set; suspect LUTs flag lead
	 * bytes of multibyte separators (utf8 mode). luts_ok false => the set
	 * doesn't fit (pathological locale) => scalar only. */
	unsigned char ws_lut_lo[16], ws_lut_hi[16];
	unsigned char sus_lut_lo[16], sus_lut_hi[16];
	unsigned char ws_bytes[16]; /* explicit list for cmpeq-chain kernels */
	int n_ws_bytes;
	unsigned char sus_bytes[16];
	int n_sus_bytes;
	bool luts_ok;

	/* THE kernel separator contract, byte-indexed: exactly the ws_bytes
	 * set. NOT the same as is_ws[] in multibyte locales — macOS's
	 * isspace(0xA0) is true even under UTF-8, but bytes >= 0x80 must be
	 * constituents to the kernels (decode handles them). Kernel vector
	 * paths AND scalar tails classify via this table only. */
	unsigned char kernel_ws[256];

	/* Multibyte separators (utf8 mode), UTF-8 encoded, for the L1
	 * discriminator and the scalar path's suspect handling. */
	struct mbws {
		unsigned char seq[4];
		unsigned char len;
	} mbws[64];
	int nmbws;
	bool suspect[256]; /* lead bytes of mbws sequences */
	bool is3lead[256]; /* lead bytes of len-3 sequences (hold-back rule) */

	/* L1 pattern groups (audit 02): sequences grouped by (lead[, second])
	 * with the final byte as a set — len 2: lead + last-byte set; len 3:
	 * lead + second + last-byte set. Kernels match these in-vector and
	 * mark every byte of a match as a separator, which equals decode
	 * semantics for word counting on valid AND invalid input (UTF-8 lead
	 * bytes never appear inside another character's encoding). */
	struct mbws_group {
		unsigned char lead, second, len;
		unsigned char set_lo[16], set_hi[16]; /* last-byte Mula LUTs */
		unsigned char set_bytes[24];
		int nset;
	} groups[8];
	int ngroups;
	bool l1_ok; /* false: groups don't fit -> mb word counting is scalar */
};

extern struct ws_spec tal_ws;

/* Separator test for a decoded character under tal_ws's rules. */
bool tal_sep_wchar(unsigned long wc);

/* L1 pattern match at p[0] with n bytes visible: returns the matched
 * separator sequence length (2 or 3) or 0. Shared by kernel scalar tails
 * and the unit-test reference. */
int tal_mbws_match(const unsigned char *p, size_t n);

/* Build tal_ws for the current locale + POSIXLY_CORRECT. In multibyte
 * locales probes iswspace over U+0080..U+3000 (no real libc defines space
 * above that; the golden/fuzz oracles would surface a violation). ~12K calls,
 * microseconds, run once at word-kernel selection. Re-runnable (tests switch
 * locales). */
void ws_init(struct ws_spec *w);

/* Scalar word-counting oracle (audit 02: written first; every SIMD kernel
 * must equal it, and it must equal the ref). Streams arbitrary chunk splits:
 * carry is in_word plus <=4 raw undecoded tail bytes decoded with a fresh
 * state per character — equivalent to GNU's mbstate carry (fragmentation
 * invariance probed on the ref, valid and invalid splits). */
struct wstate {
	bool in_word;
	unsigned char pend[8];
	unsigned npend;
};

void wstate_init(struct wstate *st);
/* Single-byte locales: the whole rule is the byte table. */
void tal_swc_sb(const unsigned char *p, size_t n, struct counts *c,
		struct wstate *st);
/* Multibyte locales (any charset): decode + classify. */
void tal_swc_mb(const unsigned char *p, size_t n, struct counts *c,
		struct wstate *st);
/* EOF: pending partial-sequence bytes are encoding errors (constituents). */
void tal_swc_mb_finish(struct counts *c, struct wstate *st);

#endif /* TAL_WS_H */
