#!/bin/sh
# Build the reference GNU wc (parity target coreutils 9.11) into tests/.work/ref/.
# Uses the pristine tree in .docs/refs/coreutils when present (dev box); otherwise
# fetches the pinned release tarball (CI / fresh checkouts, where .docs/ is
# gitignored & absent). Builds out-of-tree so the refs copy stays pristine.
# --disable-nls is load-bearing: untranslated "total" and diagnostics ARE the
# parity target (audit 01). Idempotent; honors CC so musl legs compare musl-vs-musl.
set -eu

TAG=${1:-9.11}
TARBALL_URL="https://ftp.gnu.org/gnu/coreutils/coreutils-$TAG.tar.xz"
SHA256_9_11=394024eda0a5955217ceda9cd1201e65dc8fa3aa29c2951135a49521d57c3cc3
OUT=tests/.work/ref
bin="$OUT/wc-$TAG"

mkdir -p "$OUT"
[ -x "$bin" ] && { echo "ref wc $TAG present"; exit 0; }

srcdir=""
if [ -f ".docs/refs/coreutils/src/wc.c" ]; then
	srcdir=".docs/refs/coreutils"
else
	srcdir="tests/.work/coreutils-$TAG"
	if [ ! -f "$srcdir/src/wc.c" ]; then
		tb="tests/.work/coreutils-$TAG.tar.xz"
		fetch_rc=0
		if command -v curl >/dev/null 2>&1; then
			curl -sSL -o "$tb" "$TARBALL_URL" || fetch_rc=$?
		elif command -v fetch >/dev/null 2>&1; then
			fetch -o "$tb" "$TARBALL_URL" || fetch_rc=$?
		else
			wget -qO "$tb" "$TARBALL_URL" || fetch_rc=$?
		fi
		[ "$fetch_rc" = 0 ] || {
			echo "build-ref: FETCH FAILED (rc=$fetch_rc): $TARBALL_URL" >&2
			exit 1; }
		if [ "$TAG" = 9.11 ]; then
			got=$( (sha256sum "$tb" 2>/dev/null || sha256 -q "$tb" | sed 's/$/  x/') | awk '{print $1}')
			[ "$got" = "$SHA256_9_11" ] || {
				echo "build-ref: tarball sha256 mismatch ($got)" >&2; exit 1; }
		fi
		( cd tests/.work && tar xf "coreutils-$TAG.tar.xz" )
	fi
fi

# Out-of-tree (VPATH) build; `make src/wc` pulls in just the lib prerequisites,
# but that shortcut target has raced under high -j on a fresh tree — fall back
# to a full build, then retry the target (logs append so no arm hides another).
# FORCE_UNSAFE_CONFIGURE: coreutils configure balks at uid 0 (vmactions runs as
# root); harmless otherwise.
absrc=$(cd "$srcdir" && pwd)
bld="$OUT/build-$TAG"
njobs=$( (sysctl -n hw.ncpu || nproc || echo 4) 2>/dev/null | head -1 )
MAKE=make
command -v gmake >/dev/null 2>&1 && MAKE=gmake
build_ref() {
	mkdir -p "$bld"
	( cd "$bld" \
		&& { [ -x config.status ] || FORCE_UNSAFE_CONFIGURE=1 "$absrc/configure" \
			--disable-nls --quiet ${CC:+CC="$CC"} >configure.log 2>&1; } \
		&& { "$MAKE" -s -j"$njobs" src/wc >build.log 2>&1 \
		     || "$MAKE" -s -j"$njobs" >>build.log 2>&1 \
		     || "$MAKE" -s -j"$njobs" src/wc >>build.log 2>&1; } )
}
# A cached build dir can hold a config.status from an older OS image whose
# gnulib decisions no longer match the libc (CI VM caches: undefined
# rpl_mbrtoc32, 2026-07-16). One clean-slate retry heals that class.
build_ref || {
	echo "build-ref: build failed; retrying from a clean build dir" >&2
	rm -rf "$bld"
	build_ref || {
		echo "build-ref: coreutils build failed; tail of logs:" >&2
		tail -5 "$bld/configure.log" >&2 2>&1 || echo "  (no configure.log)" >&2
		tail -20 "$bld/build.log" >&2 2>&1 || echo "  (no build.log)" >&2
		exit 1
	}
}
cp "$bld/src/wc" "$bin"

# Guard the build: a stale cache or shared clone can sit on the wrong version.
# Assert the binary reports the requested version before any test trusts it.
got=$("$bin" --version 2>/dev/null | sed -n '1s/.*coreutils) \([0-9.]*\).*/\1/p')
if [ "$got" != "$TAG" ]; then
	echo "build-ref: $bin reports version '$got', expected '$TAG' — wrong build (refusing it)" >&2
	rm -f "$bin"
	exit 1
fi
echo "built ref wc $TAG -> $bin"
