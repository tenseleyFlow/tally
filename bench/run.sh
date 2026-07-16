#!/bin/sh
# Benchmark driver: hyperfine tally vs the ref wc over the corpus classes; gate
# that tally is faster (bench/gate.sh). Gates only when tests/golden/PARITY_ACTIVE
# exists (the same point tally produces real output) and only on configs golden
# proves identical. The smoke row always runs so the CSV->gate plumbing is proven
# before it matters.
set -u

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
cd "$root"

REFTAG=${REFTAG:-9.11}
ref="tests/.work/ref/wc-$REFTAG"
TALLY="${TALLY:-./tally}"

if ! command -v hyperfine >/dev/null 2>&1; then
	echo "bench: hyperfine not found — skipping (install it to run the perf gate)"
	exit 0
fi

# Pin a UTF-8 locale: ssh sessions and CI runners often have none, silently
# turning the utf8/mb cells into C-locale measurements of a different
# workload (-m degrades to the fstat path and "benchmarks" process startup).
if [ "$(locale charmap 2>/dev/null)" != "UTF-8" ]; then
	for L in C.UTF-8 en_US.UTF-8 en_US.utf8; do
		if [ "$(LC_ALL=$L locale charmap 2>/dev/null)" = "UTF-8" ]; then
			LANG=$L; export LANG; unset LC_ALL
			break
		fi
	done
fi
echo "bench: locale $(locale charmap 2>/dev/null || echo unknown)"

[ -x "$ref" ] || sh tests/golden/build-ref.sh "$REFTAG" >/dev/null 2>&1 || true
[ -x "$ref" ] || { echo "bench: no reference wc; skipping"; exit 0; }

work=bench/.work
mkdir -p "$work"

rc=0
# bench_one <label> <metric|auto> <margin|-> [args...] — hyperfine both tools
# on identical argv (no shell: -N; env prefixes must go through `env`,
# hyperfine execs directly). Explicit min: the -l/-c big-file near-ties (audit
# 03 risk 1) gate on best-of-N so scheduler jitter can't flip a coin-toss mean.
# An explicit margin encodes cell physics (e.g. the macOS spawn floor for
# startup-bound cells) and beats the TAL_PERF_MARGIN runner knob; "-" inherits.
bench_one() {
	_lbl=$1; _metric=$2; _margin=$3; shift 3
	_csv="$work/m_$_lbl.csv"
	hyperfine -N -w 3 -r 20 --export-csv "$_csv" \
		"${TAL_BENCH_PREFIX:-}$TALLY $*" "$ref $*" >/dev/null 2>&1 || {
		echo "bench: hyperfine failed for $_lbl"; rc=1; return; }
	if [ -n "${TAL_PERF_METRIC:-}" ]; then
		_metric=$TAL_PERF_METRIC
	elif [ "$_metric" = auto ]; then
		# min under 20ms (noise-robust), else mean (audit 03).
		tmean=$(awk -F, 'NR>1 { split($1,w," "); n=split(w[1],q,"/"); b=q[n];
			if (b=="wc" || b ~ /^wc-/) {print $2; exit} }' "$_csv")
		if awk -v t="$tmean" 'BEGIN { exit !(t + 0 < 0.020) }'; then
			_metric=min
		else
			_metric=mean
		fi
	fi
	if [ "$_margin" = vm-info ]; then
		# Gate on real hardware; report-only on ephemeral VM runners
		# (TAL_VM_RUNNER=1 in CI): three runs measured identical code
		# at 0.64x/0.88x/1.21x vs the ref purely by runner draw.
		if [ -n "${TAL_VM_RUNNER:-}" ]; then
			sh bench/gate.sh "$_csv" "$_metric" "$_lbl (info)" || true
		else
			TAL_PERF_MARGIN=0.97 sh bench/gate.sh "$_csv" "$_metric" "$_lbl" || rc=1
		fi
	elif [ "$_margin" != - ]; then
		TAL_PERF_MARGIN=$_margin sh bench/gate.sh "$_csv" "$_metric" "$_lbl" || rc=1
	else
		sh bench/gate.sh "$_csv" "$_metric" "$_lbl" || rc=1
	fi
}

# Plumbing smoke: startup-only row, never gated (informational).
smoke_csv="$work/m_smoke.csv"
if hyperfine -N -w 3 -r 20 --export-csv "$smoke_csv" \
	"$TALLY --version" "$ref --version" >/dev/null 2>&1; then
	sh bench/gate.sh "$smoke_csv" min smoke || true
else
	echo "bench: smoke hyperfine run failed"; rc=1
fi

if [ ! -f tests/golden/PARITY_ACTIVE ]; then
	echo "bench: parity gate inactive (no PARITY_ACTIVE) — smoke plumbing only"
	exit $rc
fi

corpus="$work/corpus"
sh bench/mkclasses.sh "$corpus" >/dev/null

# Prime the page cache so both tools measure compute, not first-touch I/O.
cat "$corpus"/big-* "$corpus"/newline-dense "$corpus"/long-lines >/dev/null 2>&1

# Gated cells activate per sprint (audit 03 corpus x flags matrix; sprint 01
# turns on -l/-c, sprint 02 the default invocation, etc.).
# c_big_ascii is the fstat zero-read path: pure process startup. On macOS,
# tally --version == an empty C program at the posix_spawn floor while the
# ref sits ~0.2ms under it, and shared VMs jitter the floor (CI measured
# 0.69x once, just under a 0.70 margin) — Darwin gates loose; FreeBSD/Linux
# carry the tight coverage.
cmarg=0.90
[ "$(uname -s)" = Darwin ] && cmarg=0.60
bench_one c_big_ascii min "$cmarg" -c "$corpus/big-ascii"
# The flagship (sprint 02): default invocation and -w, where the fused kernel
# meets GNU's scalar word loop. Typography (E2-dense) and binary (random
# suspects) exercise the L1 pattern path.
bench_one default_big_ascii auto - "$corpus/big-ascii"
bench_one default_big_utf8 auto - "$corpus/big-utf8"
bench_one default_big_typography auto - "$corpus/big-typography"
bench_one default_big_binary auto - "$corpus/big-binary"
bench_one default_newline_dense auto - "$corpus/newline-dense"
bench_one default_long_lines auto - "$corpus/long-lines"
bench_one w_big_ascii auto - -w "$corpus/big-ascii"
bench_one w_big_utf8 auto - -w "$corpus/big-utf8"
bench_one w_big_typography auto - -w "$corpus/big-typography"
# -m: the valid-start counting kernel (roadmap P3): chars = valid sequence
# starts, position-independent, so invalid bytes cost nothing extra -- binary
# runs at full vector speed (60x here; the old whole-span validator held it
# to 1.5x by rejecting every span to the scalar oracle).
bench_one m_big_utf8 auto - -m "$corpus/big-utf8"
bench_one m_big_ascii auto - -m "$corpus/big-ascii"
bench_one m_big_binary auto - -m "$corpus/big-binary"
bench_one lm_big_utf8 auto - -lm "$corpus/big-utf8"
# -L: printable-ASCII scanner; specials take oracle windows through the lean
# width walker (roadmap P4: the full oracle in the windows held utf8 -L to a
# glibc tie). Measured after P4: dorado 5.5x, hasu/glibc 1.36x, macOS 4.2x.
bench_one L_big_utf8 auto - -L "$corpus/big-utf8"
bench_one L_big_ascii auto - -L "$corpus/big-ascii"
bench_one L_newline_dense auto - -L "$corpus/newline-dense"
bench_one L_long_lines auto - -L "$corpus/long-lines"
bench_one lwmcL_big_ascii auto - -lwmcL "$corpus/big-ascii"
# tiny-many (sprint 04): 10k files through the multi-file loop — per-file
# dispatch, estimator stats, and open/close costs dominate.
bench_one tiny_many auto - --files0-from="$corpus/tiny.list"
# The -l cells are read()-bound near-ties against GNU's SIMD (audit 03 risk
# 1). Real hardware gates hard at 0.97 and measures >= 1.0 everywhere (dorado
# 1.01-1.05, hasu 1.15-1.36, nomad-1 2.0-7.7, all with the avx512 tier where
# the CPU has it). GitHub's shared-VM fleet is not a measurand: mixed Ice
# Lake / Milan runners scored identical code 0.64x, 0.88x, and 1.21x on
# consecutive runs (2026-07-16) -- so VM runners report these cells without
# gating, and dorado/hasu/nomad-1 keep the hard coverage.
bench_one l_big_ascii min vm-info -l "$corpus/big-ascii"
bench_one l_newline_dense min vm-info -l "$corpus/newline-dense"
bench_one l_long_lines min vm-info -l "$corpus/long-lines"
# Threaded -l (roadmap P5, opt-in): 4 pread workers vs the serial ref. The
# env prefix reaches tally only (wc ignores TAL_THREADS). dorado: the -l tie
# becomes 1.84x; VM runners report-only like the other -l cells.
TAL_BENCH_PREFIX="env TAL_THREADS=4 " bench_one l_mt4_big_ascii min vm-info -l "$corpus/big-ascii"

exit $rc
