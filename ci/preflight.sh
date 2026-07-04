#!/bin/sh
# Pre-flight a change on the remote boxes over Tailscale before pushing:
# build + test + bench on hasu (Linux/glibc/AVX2) and nomad (macOS arm64/NEON).
# FreeBSD 15 is the local dev box, covered by running `gmake test` here.
# Remote commands go to `sh` via stdin — remote login shells may be fish, which
# rejects POSIX grouping if ssh hands it the command string directly.
set -u

HOSTS=${1:-"hasu nomad"}
USER=${TAL_REMOTE_USER:-mfwolffe}
REMOTE_DIR='.tally-preflight' # relative to $HOME on the remote

rc=0
for host in $HOSTS; do
	echo "== preflight: $host =="
	ssh "$USER@$host" sh <<EOF || { rc=1; continue; }
mkdir -p "\$HOME/$REMOTE_DIR"
EOF
	# --delete prunes stale tracked files; excluded dirs survive, so the built
	# ref oracle in tests/.work is a persistent per-box cache (build-ref's
	# version guard invalidates it when the pin moves).
	rsync -az --delete \
		--exclude '.git' --exclude '.docs' --exclude 'CLAUDE.md' \
		--exclude 'tests/.work' --exclude 'bench/.work' \
		--exclude '*.o' --exclude '*.d' --exclude '/tally' \
		--exclude 'config.mk' --exclude 'config.h' \
		./ "$USER@$host:$REMOTE_DIR/" || { rc=1; continue; }
	ssh "$USER@$host" sh <<EOF || rc=1
set -e
# Non-interactive ssh sh gets a minimal PATH: Homebrew (macOS) and the usual
# local prefixes would be invisible, hiding gmake/hyperfine.
PATH="\$PATH:/opt/homebrew/bin:/usr/local/bin:/usr/local/sbin"
export PATH
cd "\$HOME/$REMOTE_DIR"
MAKE=make
command -v gmake >/dev/null 2>&1 && MAKE=gmake
./configure
"\$MAKE" CFLAGS='-O2 -Werror'
"\$MAKE" test
"\$MAKE" bench
EOF
done
exit $rc
