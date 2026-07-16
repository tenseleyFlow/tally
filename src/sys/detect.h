#ifndef TAL_SYS_DETECT_H
#define TAL_SYS_DETECT_H

/* Runtime AVX2 availability: compile-time support AND the host CPU has it.
 * Cached after the first call. Always 0 on non-x86 or without compiler support. */
int tal_cpu_has_avx2(void);
int tal_cpu_has_avx512bw(void);

/* Best counting-kernel ISA on this host: "avx2", "sse2", "neon", or "scalar".
 * Used by --debug reporting and unit tests; kernel dispatch keys off the same
 * predicates directly. */
const char *tal_isa_name(void);

#endif /* TAL_SYS_DETECT_H */
