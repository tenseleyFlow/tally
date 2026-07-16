#!/bin/sh
# tally test driver: unit tests (ASan/UBSan) -> Makefile SRC guard -> golden parity.
# Run from the repo root (gmake test). Exit 0 only if everything passes.
set -u

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
cd "$root"

CC=${CC:-cc}
fail=0
work=$(mktemp -d "${TMPDIR:-/tmp}/taltest.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

# Reuse the platform feature macros the real build computed.
CONF_CFLAGS=""
AVX2_CFLAGS=""
AVX512_CFLAGS=""
[ -f config.mk ] && {
	CONF_CFLAGS=$(sed -n 's/^CONF_CFLAGS = //p' config.mk)
	AVX2_CFLAGS=$(sed -n 's/^AVX2_CFLAGS = //p' config.mk)
	AVX512_CFLAGS=$(sed -n 's/^AVX512_CFLAGS = //p' config.mk)
}

san="-fsanitize=address,undefined -fno-sanitize-recover=all"
[ "${TAL_TEST_SANITIZE:-1}" = 0 ] && san=""

# Unit tests exercise a UTF-8 leg via setlocale(LC_ALL, "") — make sure one is
# actually in the environment (CI runners and ssh sessions often have none;
# a silent C-only run hid a macOS-specific divergence once).
if [ "$(locale charmap 2>/dev/null)" != "UTF-8" ]; then
	for L in C.UTF-8 en_US.UTF-8 en_US.utf8; do
		if [ "$(LC_ALL=$L locale charmap 2>/dev/null)" = "UTF-8" ]; then
			LANG=$L; export LANG; unset LC_ALL
			break
		fi
	done
fi

PTHREAD_FLAGS=""
[ -f config.mk ] && PTHREAD_FLAGS=$(sed -n 's/^PTHREAD_FLAGS = //p' config.mk)
CFLAGS_T="-std=c11 -g -O1 $san $CONF_CFLAGS $PTHREAD_FLAGS -Isrc -I. -D_FILE_OFFSET_BITS=64"

# Library objects = all src/*.c and src/sys/*.c except main.c (tests provide
# main). Compiled per-TU because ISA flags apply to single files only — a
# global -mavx2 would let autovectorization emit illegal instructions for
# SSE2-only hosts (same rule as the Makefile).
echo "== build test objects =="
libobjs=""
for s in src/*.c src/sys/*.c; do
	case "$s" in */main.c) continue ;; esac
	o="$work/$(echo "$s" | tr / _).o"
	extra=""
	case "$s" in
	*/simd_avx2.c) extra="$AVX2_CFLAGS" ;;
	*/simd_avx512.c) extra="$AVX512_CFLAGS" ;;
	esac
	if ! $CC $CFLAGS_T $extra -c -o "$o" "$s" 2>"$work/obj.build"; then
		echo "BUILD FAIL $s"; cat "$work/obj.build"; fail=1
	fi
	libobjs="$libobjs $o"
done

echo "== unit tests =="
for t in tests/unit/*_test.c; do
	[ -e "$t" ] || continue
	name=$(basename "$t" .c)
	if ! $CC $CFLAGS_T -o "$work/$name" "$t" $libobjs 2>"$work/$name.build"; then
		echo "BUILD FAIL $name"; cat "$work/$name.build"; fail=1; continue
	fi
	if ! "$work/$name"; then
		fail=1
	fi
done

echo "== Makefile SRC vs filesystem =="
sed -n '/^SRC = /,/[^\\]$/p' Makefile \
	| sed 's/^SRC = //; s/\\//g' | tr -s ' \t' '\n' | grep '\.c$' | sort >"$work/mk_src"
ls src/*.c src/sys/*.c 2>/dev/null | sort >"$work/fs_src"
if ! diff -u "$work/mk_src" "$work/fs_src" >"$work/src_diff"; then
	echo "SRC GUARD FAIL: Makefile SRC list does not match src/*.c on disk:"
	cat "$work/src_diff"; fail=1
fi

echo "== golden parity =="
if ! sh tests/golden/run.sh; then
	fail=1
fi

echo "== differential fuzz =="
if ! FUZZ_N=${FUZZ_N:-40} FUZZ_SEED=${FUZZ_SEED:-1225} sh tests/golden/fuzz.sh; then
	fail=1
fi

echo "== kernel engagement =="
# A silent scalar fallback would pass every parity test and forfeit the perf
# thesis (audit 04). On hardware the build supports, --debug must not report
# the scalar tiers.
if grep -qE "TAL_HAS_(SSE2|NEON) *1" config.h && [ -x ./tally ]; then
	eng=$(./tally --debug -w /dev/null 2>&1; ./tally --debug -l /dev/null 2>&1)
	if echo "$eng" | grep -q "scalar"; then
		echo "ENGAGEMENT FAIL: SIMD-capable build reports a scalar kernel:"
		echo "$eng"; fail=1
	else
		echo "$eng" | sed 's/^/  /'
	fi
fi

if [ "$fail" = 0 ]; then
	echo "ALL TESTS PASSED"
else
	echo "TESTS FAILED"
fi
exit $fail
