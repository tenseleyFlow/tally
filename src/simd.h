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

/* Fused lines+words kernels (audit 02). Byte semantics only: separator iff
 * the byte is in tal_ws's byte set; every other byte (all >=0x80 included)
 * is a word constituent — exact for single-byte locales, and exact for UTF-8
 * locales except where multibyte separators occur, which is what the suspect
 * scan catches. prev_is_ws is the carry (1 at start of input: virtual
 * leading space). With scan_suspect, returns false the moment any byte hits
 * tal_ws's suspect set — *out is then invalid and the caller re-runs the
 * block through the scalar oracle (L2). Returns true otherwise. */
struct lwc_out {
	unsigned long long lines, words;
	unsigned last_is_ws;
};

bool tal_lwc_sse2(const unsigned char *p, size_t n, unsigned prev_is_ws,
		  struct lwc_out *out, bool scan_suspect);
bool tal_lwc_avx2(const unsigned char *p, size_t n, unsigned prev_is_ws,
		  struct lwc_out *out, bool scan_suspect);
bool tal_lwc_neon(const unsigned char *p, size_t n, unsigned prev_is_ws,
		  struct lwc_out *out, bool scan_suspect);

#endif /* TAL_SIMD_H */
