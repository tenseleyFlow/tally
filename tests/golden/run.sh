#!/bin/sh
# Golden parity harness. Two phases:
#   phase 1 (always): ref-vs-ref self-test — build the reference wc, run it twice
#           over the case matrix, assert byte-identical. Proves the harness and
#           corpus are deterministic before any tally diff is trusted.
#   phase 2 (only when tests/golden/PARITY_ACTIVE exists): tally-vs-ref parity.
# Sanctioned normalization: the program-name token on stderr and in Try-help
# lines (audit 01; --help/--version/--debug simply aren't in the matrix).
# Deviations registry: tests/golden/deviations/<slug>/ pins intentional
# divergences (genuine wc bugs tally fixes — bug policy, sprints/README).
set -u
set -f # no pathname expansion: case tokens must reach the tools literally

here=$(dirname "$0")
root=$(cd "$here/../.." && pwd)
cd "$root"

REFTAG=${REFTAG:-9.11}
ref="tests/.work/ref/wc-$REFTAG"
TALLY="${TALLY:-./tally}"

work=$(mktemp -d "${TMPDIR:-/tmp}/talgold.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

# Build the reference oracle.
if ! sh tests/golden/build-ref.sh "$REFTAG" >"$work/buildref.log" 2>&1; then
	echo "GOLDEN: could not build reference wc $REFTAG:"; cat "$work/buildref.log"
	exit 1
fi

# Pin a private copy of tally so a concurrent `gmake` can't swap it mid-run.
UUT="$work/tally.uut"
[ -x "$TALLY" ] && cp "$TALLY" "$UUT"

corpus="$work/corpus"
sh tests/golden/mkcorpus.sh "$corpus" >/dev/null

# Case matrix. One case per line:
#   NN [%< FILE] args...
# NN is the sprint that makes the case parity-ready: phase 1 (self-test) runs
# every case; phase 2 runs cases with NN <= the number in PARITY_ACTIVE.
# %< FILE redirects stdin from corpus file FILE (default stdin: /dev/null).
# %C -> corpus root; %S -> the spaced-name file; %E -> an empty argv word
# (both substituted after word-splitting, so they survive IFS). Grows every
# sprint alongside the behavior it locks in.
CASES='
01 -c %C/ascii.txt
01 -l %C/ascii.txt
01 -l %C/lines.txt
01 -l %C/longline.txt
01 -l %C/binary.bin
01 -l %C/empty
01 -l %C/onebyte
01 -l %C/nl
01 -l %C/noeol.txt
01 -l %C/crlf.txt
01 -c %C/empty
01 -c %C/twok
01 -lc %C/ascii.txt
01 -cl %C/utf8.txt
01 -lc %C/longline.txt
01 --lines %C/ascii.txt
01 --line %C/ascii.txt
01 --bytes %C/binary.bin
01 -l -c %C/e2.txt
01 -l %S
01 %E
01 -l %C/nosuchfile
01 -c %C/nosuchfile
01 -l %C
01 -l -
01 %< ascii.txt -l
01 %< utf8.txt -l
01 %< binary.bin -c
01 %< empty -l
01 %< lines.txt -lc
02 %C/empty
02 %C/onebyte
02 %C/nl
02 %C/noeol.txt
02 %C/spaces.txt
02 %C/ascii.txt
02 %C/utf8.txt
02 %C/e2.txt
02 %C/mbws.txt
02 %C/binary.bin
02 %C/lines.txt
02 %C/longline.txt
02 %C/nbsp.txt
02 %C/ctrl.txt
02 %C/crlf.txt
02 -w %C/mbws.txt
02 -w %C/e2.txt
02 -w %C/utf8.txt
02 -lw %C/mbws.txt
02 -w %C/mbsplit.txt
02 -lw %C/mbsplit.txt
02 -w %C/nbspsplit.txt
02 -w %C/nbsp.txt
02 -w %C/ctrl.txt
02 %S
02 %C/nosuchfile
02 %< ascii.txt
02 %< mbws.txt -w
02 %< e2.txt -w
02 %< empty
03 -m %C/binary.bin
03 -m %C/utf8.txt
03 -L %C/ctrl.txt
03 -L %C/crlf.txt
03 -lwmcL %C/utf8.txt
03 -cm %C/utf8.txt
03 -mc %C/utf8.txt
04 %C/ascii.txt %C/utf8.txt
04 %C/ascii.txt %C/utf8.txt %C/binary.bin
04 %C/ascii.txt -l
04 %C/nosuchfile %C/ascii.txt
04 --total=always %C/ascii.txt
04 --total=only %C/ascii.txt %C/utf8.txt
04 --total=never %C/ascii.txt %C/utf8.txt
04 --files0-from=%C/files0.list
'

# The tools report their program name as argv[0] (getopt lines: verbatim; error
# lines: basename). Normalize every form for both binaries, plus Try-help lines.
refbase=$(basename "$ref")
uutbase=$(basename "$UUT")
normprog() {
	sed "s#^$ref: #PROG: #; s#^$refbase: #PROG: #; \
	     s#^$UUT: #PROG: #; s#^$uutbase: #PROG: #; \
	     s#^wc: #PROG: #; s#^tally: #PROG: #; \
	     s#Try '[^']* --help'#Try 'PROG --help'#"
}

# run_case <binary> <case-string> -> writes o.out/o.err/o.rc in $work
run_case() {
	_bin=$1; _case=$2
	_stdin=/dev/null
	_expanded=$(printf '%s' "$_case" | sed "s#%C#$corpus#g")
	# shellcheck disable=SC2086
	set -- $_expanded
	if [ "${1:-}" = "%<" ]; then
		_stdin="$corpus/$2"
		shift 2
	fi
	# Rebuild "$@", mapping post-split placeholders (spaced name, empty word).
	_n=$#; _i=0
	while [ "$_i" -lt "$_n" ]; do
		_arg=$1; shift
		case $_arg in
		"%S") _arg="$corpus/sp ace.txt" ;;
		"%E") _arg="" ;;
		esac
		set -- "$@" "$_arg"
		_i=$((_i + 1))
	done
	"$_bin" "$@" <"$_stdin" >"$work/o.out" 2>"$work/o.err"
	echo $? >"$work/o.rc"
}

# Deviations registry lookup: echoes the registry dir whose `case` file equals
# the case line, if any. Registered cases assert tally against pinned fixtures
# (expect.out/expect.errn/expect.rc) AND pin the tally-vs-ref divergence
# (expect.refdiff) so an upstream fix or drift fails loudly.
deviation_dir() {
	for _d in tests/golden/deviations/*/; do
		[ -f "$_d/case" ] || continue
		[ "$(cat "$_d/case")" = "$1" ] && { printf '%s' "$_d"; return 0; }
	done
	return 1
}

check_deviation() { # $1=dev dir, $2=case; UUT results in a.*, ref results in o.*
	_dev=$1; _c=$2
	_ok=1
	cmp -s "$work/a.out" "$_dev/expect.out" || _ok=0
	cmp -s "$work/a.errn" "$_dev/expect.errn" || _ok=0
	[ "$(cat "$work/a.rc")" = "$(cat "$_dev/expect.rc")" ] || _ok=0
	{
		diff "$work/o.out" "$work/a.out"
		diff "$work/o.errn" "$work/a.errn"
		echo "rc ref=$(cat "$work/o.rc") tally=$(cat "$work/a.rc")"
	} >"$work/refdiff" 2>&1
	cmp -s "$work/refdiff" "$_dev/expect.refdiff" || _ok=0
	if [ "$_ok" = 0 ]; then
		echo "  DEVIATION DRIFT [$_c] (registry: $_dev)"
		echo "$_c" >>"$work/fails"
	fi
}

# Compare two binaries over CASES under one locale. Phase "parity" skips cases
# whose sprint tag exceeds $active. Failures append to $work/fails (the while
# loop is a pipeline subshell — a file escapes it).
phase() {
	_a=$1; _b=$2; _label=$3; _loc=$4
	printf '%s\n' "$CASES" | while IFS= read -r line; do
		[ -n "$line" ] || continue
		tag=${line%% *}
		c=${line#* }
		case "$_label" in
		parity*) [ "$tag" -gt "$active" ] && continue ;;
		esac
		LC_ALL="$_loc" run_case "$_a" "$c"
		cp "$work/o.out" "$work/a.out"; cp "$work/o.err" "$work/a.err"
		cp "$work/o.rc" "$work/a.rc"
		normprog <"$work/a.err" >"$work/a.errn"
		LC_ALL="$_loc" run_case "$_b" "$c"
		normprog <"$work/o.err" >"$work/o.errn"
		case "$_label" in
		parity*)
			if _dev=$(deviation_dir "$c"); then
				check_deviation "$_dev" "$c"
				continue
			fi ;;
		esac
		ok=1
		cmp -s "$work/a.out" "$work/o.out" || ok=0
		cmp -s "$work/a.errn" "$work/o.errn" || ok=0
		[ "$(cat "$work/a.rc")" = "$(cat "$work/o.rc")" ] || ok=0
		if [ "$ok" = 0 ]; then
			echo "  DIFF [$_label/$_loc]: $c (rc a=$(cat "$work/a.rc") b=$(cat "$work/o.rc"))"
			diff "$work/a.out" "$work/o.out" | head -8
			diff "$work/a.errn" "$work/o.errn" | head -8
			echo "$c" >>"$work/fails"
		fi
	done
}

# Integration extras (audit 04 §taxonomy). Both run ref-vs-ref in phase 1 and
# tally-vs-ref in phase 2.
# Positioned fd: the -c fast path must subtract SEEK_CUR (audit 00 claim 12).
check_positioned() {
	_a=$1; _b=$2; _label=$3
	pa=$( { dd bs=1k skip=1 count=0 2>/dev/null; "$_a" -c; } <"$corpus/twok" )
	pb=$( { dd bs=1k skip=1 count=0 2>/dev/null; "$_b" -c; } <"$corpus/twok" )
	if [ "$pa" != "$pb" ]; then
		echo "  DIFF [$_label/positioned-fd]: $pa vs $pb"
		echo "positioned" >>"$work/fails"
	fi
}

# Fragmentation invariance: bytes dribbled through a pipe in small writes must
# count identically to the whole file (the wcstream bug class, audit 04).
# Compare counts, not padding — the width estimator legitimately differs
# between piped (non-regular, width 7) and redirected (regular) stdin.
# The -w rows split multibyte separators across 1-byte reads: the pend-carry
# acid test.
dribble_one() {
	_bin=$1; _label=$2; _file=$3; _flags=$4
	for bs in 1 7 4096; do
		da=$(dd if="$corpus/$_file" bs="$bs" 2>/dev/null \
			| "$_bin" $_flags | tr -s ' ')
		db=$("$_bin" $_flags <"$corpus/$_file" | tr -s ' ')
		if [ "$da" != "$db" ]; then
			echo "  DIFF [$_label/dribble $_file$_flags bs=$bs]: '$da' vs '$db'"
			echo "dribble" >>"$work/fails"
		fi
	done
}

check_dribble() {
	_bin=$1; _label=$2
	dribble_one "$_bin" "$_label" lines.txt " -lc"
	if [ "$active" -ge 2 ] || [ "$_label" = self ]; then
		dribble_one "$_bin" "$_label" utf8.txt " -lw"
		dribble_one "$_bin" "$_label" e2.txt " -w"
	fi
	# mbws (dense multibyte separators) asserts on tally ONLY: GNU 9.11
	# genuinely miscounts fragmented multibyte separators (deviation 1 in
	# .docs/deviations.md; repro: printf 'x\343\200\200\342\200\203y'
	# whole=2 words, 1-byte-fragmented=3). tally is invariant and equals
	# the ref's whole-file counts, which the parity cases pin.
	if [ "$_label" != self ] && [ "$active" -ge 2 ]; then
		dribble_one "$_bin" "$_label" mbws.txt " -w"
	fi
}

# Available locales: C plus a UTF-8 one if the box has it.
utf8=""
for L in C.UTF-8 en_US.UTF-8 en_US.utf8; do
	[ "$(LC_ALL=$L locale charmap 2>/dev/null)" = "UTF-8" ] && { utf8=$L; break; }
done
locales="C"
[ -n "$utf8" ] && locales="$locales $utf8"

active=0
[ -f tests/golden/PARITY_ACTIVE ] && active=$(cat tests/golden/PARITY_ACTIVE)

: >"$work/fails"

# Phase 1: ref vs ref (determinism self-test). Always runs. The
# POSIXLY_CORRECT axis flips the NBSP word rule AND getopt permutation
# (audit 00 claim 8a, audit 01 §option parsing).
for L in $locales; do
	phase "$ref" "$ref" "self" "$L"
	export POSIXLY_CORRECT=1
	phase "$ref" "$ref" "self-pc" "$L"
	unset POSIXLY_CORRECT
done
check_positioned "$ref" "$ref" self
check_dribble "$ref" self
if [ -s "$work/fails" ]; then
	echo "GOLDEN: self-test FAILED — harness or corpus is non-deterministic"
	exit 1
fi

# Phase 2: tally vs ref. Gated on PARITY_ACTIVE.
if [ -f tests/golden/PARITY_ACTIVE ] && [ -x "$UUT" ]; then
	for L in $locales; do
		phase "$UUT" "$ref" "parity" "$L"
		export POSIXLY_CORRECT=1
		phase "$UUT" "$ref" "parity-pc" "$L"
		unset POSIXLY_CORRECT
	done
	check_positioned "$UUT" "$ref" parity
	check_dribble "$UUT" parity
	if [ -s "$work/fails" ]; then
		n=$(wc -l <"$work/fails" | tr -d ' ')
		echo "GOLDEN: parity FAILED ($n diffs vs wc $REFTAG)"
		exit 1
	fi
	echo "GOLDEN: ok (parity vs wc $REFTAG, locales: $locales)"
else
	echo "GOLDEN: ok (self-test only; parity gate inactive — no PARITY_ACTIVE)"
fi
exit 0
