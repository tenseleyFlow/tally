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
# bench_one <label> <metric-auto> [args...] — hyperfine both tools on identical
# argv (no shell: -N; env prefixes must go through `env`, hyperfine execs directly).
bench_one() {
	_lbl=$1; shift
	_csv="$work/m_$_lbl.csv"
	hyperfine -N -w 3 -r 20 --export-csv "$_csv" \
		"$TALLY $*" "$ref $*" >/dev/null 2>&1 || {
		echo "bench: hyperfine failed for $_lbl"; rc=1; return; }
	if [ -n "${TAL_PERF_METRIC:-}" ]; then
		sh bench/gate.sh "$_csv" "$TAL_PERF_METRIC" "$_lbl" || rc=1
		return
	fi
	# Auto metric: min under 20ms (noise-robust), else mean (audit 03).
	tmean=$(awk -F, 'NR>1 { split($1,w," "); n=split(w[1],q,"/"); b=q[n];
		if (b=="wc" || b ~ /^wc-/) {print $2; exit} }' "$_csv")
	if awk -v t="$tmean" 'BEGIN { exit !(t + 0 < 0.020) }'; then
		sh bench/gate.sh "$_csv" min "$_lbl" || rc=1
	else
		sh bench/gate.sh "$_csv" mean "$_lbl" || rc=1
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
bench_one lc_big_ascii      -c "$corpus/big-ascii"
bench_one l_big_ascii       -l "$corpus/big-ascii"

exit $rc
