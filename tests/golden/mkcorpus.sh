#!/bin/sh
# Build a deterministic base corpus for golden parity tests.
# Usage: mkcorpus.sh <dir>   (idempotent: removes <dir> first)
# Bulk fixtures come from tests/gen.c — shell/awk RNGs and printf escape support
# differ across platforms; the C generator is byte-identical everywhere.
set -eu
dir=${1:?usage: mkcorpus.sh <dir>}

here=$(dirname "$0")
root=$(cd "$here/../.." && pwd)

GEN="$root/tests/.work/gen"
if [ ! -x "$GEN" ] || [ "$root/tests/gen.c" -nt "$GEN" ]; then
	mkdir -p "$root/tests/.work"
	${CC:-cc} -O2 -o "$GEN" "$root/tests/gen.c"
fi

rm -rf "$dir"
mkdir -p "$dir"

# Hand-rolled edge fixtures (octal printf escapes are POSIX-portable).
: > "$dir/empty"
printf 'x' > "$dir/onebyte"
printf '\n' > "$dir/nl"
printf 'abc def' > "$dir/noeol.txt"
printf '   \t\t \n  ' > "$dir/spaces.txt"
printf 'a\302\240b\n' > "$dir/nbsp.txt"
printf 'ab\001\002c\n\177\n' > "$dir/ctrl.txt"
printf 'a\r\nbb\r\n' > "$dir/crlf.txt"

# Generated bulk classes (class, seed, bytes — never change existing triples;
# golden output depends on them).
"$GEN" ascii    42 65536  > "$dir/ascii.txt"
"$GEN" utf8     42 65536  > "$dir/utf8.txt"
"$GEN" e2       42 65536  > "$dir/e2.txt"
"$GEN" mbws     42 8192   > "$dir/mbws.txt"
"$GEN" binary   42 65536  > "$dir/binary.bin"
"$GEN" lines    42 16384  > "$dir/lines.txt"
"$GEN" longline 42 300000 > "$dir/longline.txt" # crosses the 256 KiB buffer
"$GEN" binary   5  2048   > "$dir/twok"         # positioned-fd -c check

# Multibyte separators split exactly across the 256 KiB read boundary — the
# cross-buffer acid test for the suspect/pend carry (audit 02 checklist).
{ "$GEN" longline 9 262143; printf '\343\200\200x y\n'; } > "$dir/mbsplit.txt"
{ "$GEN" longline 11 262143; printf '\302\240z w\n'; } > "$dir/nbspsplit.txt"

# Sprint 03: -m/-L semantics. Sizes deliberately NOT block-multiples — the
# macOS 0xA0 tail bug hid behind 8 KiB-divisible fixtures (memory: kernel
# scalar tails only run on non-aligned sizes).
"$GEN" utf8 43 65543 > "$dir/utf8odd.txt"
"$GEN" mbws 43 8201  > "$dir/mbwsodd.txt"
# Invalid UTF-8 shapes: lone continuations, lone leads, overlongs (C0 80,
# E0 80 80), surrogate (ED A0 80), out-of-range (F5), truncated at EOF.
printf 'a\200b \301\277 c\300\200d\n\340\200\200 x\355\240\200y \365z ok\361\210' > "$dir/invalid.txt"
# Width geometry: tabs at varied columns, wide CJK + tab interplay,
# combining mark (e + U+0301), zero-width space (U+200B), CR/FF/VT mix.
printf 'a\tb\tccc\td\n\344\270\255\t.\n0123456\tx\ne\314\201combo\n' > "$dir/tabs.txt"
printf 'wide \344\270\255\346\226\207 z\342\200\213w\nx\rlonger-tail\fmid\013end\n' > "$dir/wide.txt"

# Filename shapes.
cp "$dir/noeol.txt" "$dir/sp ace.txt"

# NUL-separated name lists (absolute paths: cases run from the repo root).
printf '%s\0%s\0' "$dir/ascii.txt" "$dir/utf8.txt" > "$dir/files0.list"
printf '%s\0-\0%s\0' "$dir/onebyte" "$dir/nl" > "$dir/dash.list"
printf '%s\0\0%s\0' "$dir/onebyte" "$dir/nl" > "$dir/zerolen.list"
printf '%s\0%s\0' "$dir/nosuchfile" "$dir/onebyte" > "$dir/missing.list"
printf '%s' "$dir/onebyte" > "$dir/noterm.list" # final token without NUL
: > "$dir/empty.list"

echo "$dir"
