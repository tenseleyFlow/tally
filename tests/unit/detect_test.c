#include "config.h"
#include "sys/detect.h"
#include "tests/unit/test.h"

int main(void)
{
	const char *isa = tal_isa_name();

	CHECK("isa name non-null", isa != 0);
	CHECK("isa is a known tier",
	      strcmp(isa, "avx512") == 0 ||
	      strcmp(isa, "avx2") == 0 || strcmp(isa, "sse2") == 0 ||
	      strcmp(isa, "neon") == 0 || strcmp(isa, "scalar") == 0);

	int avx2 = tal_cpu_has_avx2();
	CHECK("avx2 is boolean", avx2 == 0 || avx2 == 1);
#if !TAL_HAS_AVX2
	CHECK("no compile support -> no runtime avx2", avx2 == 0);
#endif
#if !TAL_HAS_AVX2 || !TAL_HAS_CPU_SUPPORTS
	CHECK("isa never claims avx2 without support", strcmp(isa, "avx2") != 0);
#endif
	/* Stable across calls (cached). */
	CHECK("idempotent", tal_cpu_has_avx2() == avx2);

	return test_summary("detect_test");
}
