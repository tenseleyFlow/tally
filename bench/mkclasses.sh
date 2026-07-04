#!/bin/sh
# Build deterministic benchmark corpora under <root> (audit 03 classes).
# Idempotent per class (skips if present). TAL_BENCH_BYTES scales the big files
# (default 200 MB; CI sets it smaller). All bytes come from tests/gen.c.
set -eu
root=${1:?usage: mkclasses.sh <root>}
mkdir -p "$root"

here=$(dirname "$0")
top=$(cd "$here/.." && pwd)

GEN="$top/tests/.work/gen"
if [ ! -x "$GEN" ] || [ "$top/tests/gen.c" -nt "$GEN" ]; then
	mkdir -p "$top/tests/.work"
	${CC:-cc} -O2 -o "$GEN" "$top/tests/gen.c"
fi

BIG=${TAL_BENCH_BYTES:-200000000}

mk() { # class seed bytes name
	[ -f "$root/$4" ] || "$GEN" "$1" "$2" "$3" > "$root/$4"
}

mk ascii    7 "$BIG" big-ascii
mk utf8     7 "$BIG" big-utf8
mk e2       7 "$BIG" big-typography
mk binary   7 "$BIG" big-binary
mk lines    7 "$BIG" newline-dense
mk longline 7 "$BIG" long-lines

# tiny-many: 10,000 x 1 KiB files (startup + per-file loop costs).
if [ ! -d "$root/tiny" ]; then
	mkdir -p "$root/tiny"
	"$GEN" ascii 9 10240000 > "$root/tiny/.all"
	( cd "$root/tiny" && split -a 3 -b 1024 .all f && rm -f .all )
fi

echo "$root"
