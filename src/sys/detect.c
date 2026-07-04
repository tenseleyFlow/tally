#include "config.h"
#include "sys/detect.h"

int tal_cpu_has_avx2(void)
{
#if TAL_HAS_AVX2 && TAL_HAS_CPU_SUPPORTS
	static signed char cached; /* 0 unknown, 1 yes, -1 no */

	if (!cached)
		cached = __builtin_cpu_supports("avx2") ? 1 : -1;
	return cached > 0;
#else
	return 0;
#endif
}

const char *tal_isa_name(void)
{
	if (tal_cpu_has_avx2())
		return "avx2";
#if TAL_HAS_SSE2
	return "sse2";
#elif TAL_HAS_NEON
	return "neon";
#else
	return "scalar";
#endif
}
