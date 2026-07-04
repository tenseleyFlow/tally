#ifndef TAL_SIMD_H
#define TAL_SIMD_H

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

#endif /* TAL_SIMD_H */
