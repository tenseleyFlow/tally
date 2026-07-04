#!/bin/sh
# Differential fuzzer: random gen.c streams x random flag sets, tally vs ref.
# Inactive until tests/golden/PARITY_ACTIVE (needs a tally that produces output).
# Deterministic per seed: FUZZ_SEED fixes the whole run; every failure prints a
# one-line repro. FUZZ_N controls iterations (CI small, soak big).
set -u

here=$(dirname "$0")
root=$(cd "$here/../.." && pwd)
cd "$root"

[ -f tests/golden/PARITY_ACTIVE ] || { echo "FUZZ: inactive (no PARITY_ACTIVE)"; exit 0; }

REFTAG=${REFTAG:-9.11}
ref="tests/.work/ref/wc-$REFTAG"
TALLY="${TALLY:-./tally}"
N=${FUZZ_N:-100}
SEED=${FUZZ_SEED:-$(date +%s)}

sh tests/golden/build-ref.sh "$REFTAG" >/dev/null 2>&1 || { echo "FUZZ: no ref"; exit 1; }
[ -x "$TALLY" ] || { echo "FUZZ: no tally binary"; exit 1; }

GEN=tests/.work/gen
if [ ! -x "$GEN" ] || [ tests/gen.c -nt "$GEN" ]; then
	${CC:-cc} -O2 -o "$GEN" tests/gen.c
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/talfuzz.XXXXXX")
keep=tests/.work/fuzz-failures
trap 'rm -rf "$work"' EXIT INT TERM

# Small shell LCG for picking classes/flags/sizes (fixture BYTES come from
# gen.c's own PRNG, so cross-platform byte-identity only depends on gen).
state=$SEED
rnd() { # rnd <n> -> 0..n-1
	state=$(( (state * 1103515245 + 12345) % 2147483648 ))
	echo $(( state % $1 ))
}

CLASSES="ascii utf8 e2 mbws binary ws lines longline"
FLAGSETS=" |-l|-w|-c|-m|-L|-lw|-cm|-mc|-lwmcL|-wL|--total=always|--total=only"

# Locale: prefer a UTF-8 one, else C.
loc=C
for L in C.UTF-8 en_US.UTF-8 en_US.utf8; do
	[ "$(LC_ALL=$L locale charmap 2>/dev/null)" = "UTF-8" ] && { loc=$L; break; }
done

fails=0
i=0
while [ "$i" -lt "$N" ]; do
	i=$((i + 1))
	c=$(( $(rnd 8) + 1 ))
	class=$(echo "$CLASSES" | cut -d' ' -f"$c")
	fs=$(( $(rnd 12) + 1 ))
	flags=$(echo "$FLAGSETS" | cut -d'|' -f"$fs")
	# Sizes biased toward the 256 KiB buffer boundary.
	case $(rnd 4) in
	0) size=$(( $(rnd 4096) )) ;;
	1) size=$(( 262144 - 8 + $(rnd 16) )) ;;
	2) size=$(( $(rnd 1048576) )) ;;
	*) size=$(( 524288 - 8 + $(rnd 16) )) ;;
	esac
	gseed=$(rnd 1000000)
	"$GEN" "$class" "$gseed" "$size" > "$work/f"

	# shellcheck disable=SC2086
	LC_ALL=$loc "$TALLY" $flags "$work/f" >"$work/a.out" 2>"$work/a.err"; ra=$?
	# shellcheck disable=SC2086
	LC_ALL=$loc "$ref"   $flags "$work/f" >"$work/b.out" 2>"$work/b.err"; rb=$?
	# Normalize: counts lines end in the (differing) file path's basename only
	# when paths match — here both see the same path, so only program tokens
	# on stderr need normalizing.
	sed "s#^[^:]*wc[^:]*: #PROG: #" <"$work/b.err" >"$work/b.errn"
	sed "s#^[^:]*tally[^:]*: #PROG: #" <"$work/a.err" >"$work/a.errn"
	if ! cmp -s "$work/a.out" "$work/b.out" || ! cmp -s "$work/a.errn" "$work/b.errn" \
		|| [ "$ra" != "$rb" ]; then
		fails=$((fails + 1))
		mkdir -p "$keep"
		cp "$work/f" "$keep/f-$class-$gseed-$size"
		echo "FUZZ DIFF: class=$class gseed=$gseed size=$size flags='$flags' loc=$loc rc=$ra/$rb"
		echo "  repro: tests/.work/gen $class $gseed $size > f && LC_ALL=$loc $TALLY $flags f | diff - <(LC_ALL=$loc $ref $flags f)"
		diff "$work/a.out" "$work/b.out" | head -4
	fi
done

if [ "$fails" != 0 ]; then
	echo "FUZZ: $fails/$N diffs (seed $SEED; artifacts in $keep)"
	exit 1
fi
echo "FUZZ: ok ($N iterations, seed $SEED, locale $loc)"
