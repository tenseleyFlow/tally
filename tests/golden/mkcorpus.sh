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

# Filename shapes.
cp "$dir/noeol.txt" "$dir/sp ace.txt"

# NUL-separated name list (absolute paths: cases run from the repo root).
printf '%s\0%s\0' "$dir/ascii.txt" "$dir/utf8.txt" > "$dir/files0.list"

echo "$dir"
