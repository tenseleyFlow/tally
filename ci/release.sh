#!/bin/sh
# Release automation: verify -> dist -> tag -> GitHub release with artifacts.
# Version comes from src/version.h (single source of truth). Run from a clean
# tree on the dev box; refuses anything less.
set -eu

cd "$(dirname "$0")/.."

VER=$(sed -n 's/.*TAL_VERSION "\([^"]*\)".*/\1/p' src/version.h)
TAG="v$VER"

[ -z "$(git status --porcelain)" ] || {
	echo "release: working tree not clean" >&2; exit 1; }
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && {
	echo "release: tag $TAG already exists" >&2; exit 1; }

echo "== release $TAG: full verification =="
gmake clean >/dev/null
gmake CFLAGS="-O2 -Werror"
gmake test
benchrc=0
gmake bench > "bench-report-$VER.txt" 2>&1 || benchrc=$?
grep "PERF GATE" "bench-report-$VER.txt"
# Gate on the bench EXIT CODE: it reflects gated cells only. A grep for
# "FAIL" overmatches the informational smoke row (aborted a release once).
[ "$benchrc" = 0 ] || { echo "release: perf gate not clean" >&2; exit 1; }

echo "== dist =="
gmake dist
sha256sum "tally-$VER.tar.gz" > "tally-$VER.tar.gz.sha256" 2>/dev/null ||
	sha256 -r "tally-$VER.tar.gz" > "tally-$VER.tar.gz.sha256"

echo "== clean-room build from the tarball =="
CR=$(mktemp -d)
tar -xzf "tally-$VER.tar.gz" -C "$CR"
( cd "$CR/tally-$VER" && ./configure && gmake CFLAGS="-O2 -Werror" &&
	./tally --version | grep -q "$VER" )
rm -rf "$CR"

echo "== tag + GitHub release =="
git tag -a "$TAG" -m "tally $TAG"
git push origin "$TAG"
gh release create "$TAG" \
	--title "tally $TAG" \
	--notes-file CHANGELOG.md \
	"tally-$VER.tar.gz" "tally-$VER.tar.gz.sha256" \
	"bench-report-$VER.txt"

echo "release: $TAG done. Update packaging/ sha256 pins:"
cat "tally-$VER.tar.gz.sha256"
