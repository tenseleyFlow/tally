#include "simd.h"

/* Parity oracle and last-resort fallback. Every SIMD kernel must equal this
 * function on every input (unit + fuzz enforced). */
unsigned long long tal_nlcount_scalar(const unsigned char *p, size_t n)
{
	unsigned long long lines = 0;

	for (size_t i = 0; i < n; i++)
		lines += p[i] == '\n';
	return lines;
}
