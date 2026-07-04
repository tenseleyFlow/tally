#ifndef TAL_SIMD_H
#define TAL_SIMD_H

#include <stdbool.h>
#include <stddef.h>

/* Newline counters. Each returns the number of '\n' bytes in p[0..n).
 * Definitions are compiled only where config.h + the compiler support the ISA
 * (the TU is empty otherwise), so callers must guard call sites the same way.
 * All kernels accept unaligned pointers; the byte-lane accumulation scheme
 * (subtract 0xFF masks, flush through psadbw/vpaddl before u8 overflow) is
 * audit 02 §accumulation. */
unsigned long long tal_nlcount_scalar(const unsigned char *p, size_t n);
unsigned long long tal_nlcount_sse2(const unsigned char *p, size_t n);
unsigned long long tal_nlcount_avx2(const unsigned char *p, size_t n);
unsigned long long tal_nlcount_neon(const unsigned char *p, size_t n);

/* Fused lines+words kernels (audit 02). Separator iff the byte is in
 * tal_ws's derived byte set OR part of a multibyte separator sequence
 * matched in-vector via the L1 pattern groups; every other byte (invalid
 * UTF-8 included) is a word constituent — equal to decode semantics for
 * word counting, because UTF-8 lead bytes never occur inside another
 * character's encoding. prev_is_ws is the carry (1 at start of input:
 * virtual leading space). Returns the number of bytes consumed: n, minus a
 * 0-2 byte held-back tail when a potential separator can't be verified
 * locally — the caller routes held bytes through the scalar oracle. */
struct lwc_out {
	unsigned long long lines, words;
	unsigned last_is_ws;
};

size_t tal_lwc_sse2(const unsigned char *p, size_t n, unsigned prev_is_ws,
		    struct lwc_out *out);
size_t tal_lwc_avx2(const unsigned char *p, size_t n, unsigned prev_is_ws,
		    struct lwc_out *out);
size_t tal_lwc_neon(const unsigned char *p, size_t n, unsigned prev_is_ws,
		    struct lwc_out *out);

/* Validated UTF-8 char counting for -m (audit 02 §-m): chars = positions
 * where a valid character starts; any invalid sequence disqualifies its
 * span. Kernels process seq-complete spans of <=8 KiB (a trailing
 * incomplete sequence is held back), commit chars+newlines per clean span,
 * and stop before a span containing an invalid sequence. Returns bytes
 * consumed; the caller walks the rejected/held remainder with the scalar
 * oracle (GNU's one-byte-at-a-time error resync). Strict walker shared by
 * kernel tails and the no-SIMD tier: returns 0 ok / -1 invalid. */
int tal_u8walk(const unsigned char *p, size_t n, unsigned long long *chars,
	       unsigned long long *lines);
size_t tal_u8count_avx2(const unsigned char *p, size_t n,
			unsigned long long *chars, unsigned long long *lines);
size_t tal_u8count_neon(const unsigned char *p, size_t n,
			unsigned long long *chars, unsigned long long *lines);

#endif /* TAL_SIMD_H */
