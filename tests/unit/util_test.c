#include "util.h"
#include "tests/unit/test.h"

int main(void)
{
	char buf[TAL_U64_BUFSIZE];

	CHECK_STR("zero", tal_u64tostr(0, buf), "0");
	CHECK_STR("one digit", tal_u64tostr(7, buf), "7");
	CHECK_STR("ten", tal_u64tostr(10, buf), "10");
	CHECK_STR("big", tal_u64tostr(1234567890123ULL, buf), "1234567890123");
	CHECK_STR("u64 max", tal_u64tostr(18446744073709551615ULL, buf),
		  "18446744073709551615");
	CHECK("returns into buf", tal_u64tostr(42, buf) >= buf);
	CHECK("nul at end", tal_u64tostr(42, buf)[2] == '\0');

	void *p = xmalloc(16);
	CHECK("xmalloc non-null", p != 0);
	void *q = xmalloc(0);
	CHECK("xmalloc(0) non-null", q != 0);

	return test_summary("util_test");
}
