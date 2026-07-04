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
		"$TALLY $*" "$ref $*" >/dev/null 2>&1 || {
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
	if [ "$_margin" != - ]; then
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
# c_big_ascii is the fstat zero-read path: pure process startup. Measured on
# macOS (nomad-1): tally --version == an empty C program at the posix_spawn
# floor (~1ms, high variance on shared VMs) while the ref sits ~0.2ms under
# it; 0.70 tolerates that floor noise and still catches a real startup
# regression. Tight coverage for this cell comes from FreeBSD/Linux runs.
bench_one c_big_ascii min 0.70 -c "$corpus/big-ascii"
# The flagship (sprint 02): default invocation and -w, where the fused kernel
# meets GNU's scalar word loop. big-typography and big-binary gate after L1
# (audit 02 ship order — L2 scalar fallback dominates those corpora for now).
bench_one default_big_ascii auto - "$corpus/big-ascii"
bench_one default_big_utf8 auto - "$corpus/big-utf8"
bench_one default_newline_dense auto - "$corpus/newline-dense"
bench_one default_long_lines auto - "$corpus/long-lines"
bench_one w_big_ascii auto - -w "$corpus/big-ascii"
bench_one w_big_utf8 auto - -w "$corpus/big-utf8"
# The -l cells are read()-bound ties against GNU's SIMD (audit 03 risk 1): a
# 1.00 margin on min still coin-flips on ~2% run-to-run jitter (observed both
# directions on dorado). 0.97 tolerates the jitter; the 16%-slower kernel the
# gate caught in sprint 01 would still fail loudly.
bench_one l_big_ascii min 0.97 -l "$corpus/big-ascii"
bench_one l_newline_dense min 0.97 -l "$corpus/newline-dense"
bench_one l_long_lines min 0.97 -l "$corpus/long-lines"

exit $rc
