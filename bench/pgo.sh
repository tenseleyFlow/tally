#!/bin/sh
# Profile-guided build (clang). Opt-in for packagers: `gmake pgo`. Packaged
# builds stay plain -O3 -flto for reproducibility; this exists to measure
# what PGO buys on the bench corpora. Measured 2026-07 on the dev box: PGO
# was ~10% SLOWER on the SIMD-kernel workloads (hand-structured hot loops
# don't benefit from profile-driven layout) — plain release is the right
# default, not just the reproducible one.
set -eu

cd "$(dirname "$0")/.."

CC=${CC:-cc}
$CC --version | grep -qi clang || {
	echo "pgo: clang required (llvm-profdata)"; exit 1; }
command -v llvm-profdata >/dev/null 2>&1 || {
	echo "pgo: llvm-profdata not found"; exit 1; }

corpus=bench/.work/corpus
sh bench/mkclasses.sh "$corpus" >/dev/null

echo "== instrumented build =="
gmake clean >/dev/null
gmake OPT="-O3 -flto -DNDEBUG -fprofile-generate=bench/.work/pgo" >/dev/null

echo "== profile run (bench corpora) =="
./tally "$corpus/big-ascii" >/dev/null
./tally "$corpus/big-utf8" >/dev/null
./tally -m "$corpus/big-utf8" >/dev/null
./tally -L "$corpus/big-ascii" >/dev/null
./tally -lwmcL "$corpus/big-typography" >/dev/null

llvm-profdata merge -output=bench/.work/pgo.profdata bench/.work/pgo/*.profraw

echo "== optimized build =="
gmake clean >/dev/null
gmake OPT="-O3 -flto -DNDEBUG -fprofile-use=$(pwd)/bench/.work/pgo.profdata" >/dev/null
strip tally ty 2>/dev/null || true
echo "pgo: done — compare with 'gmake bench'"
