#!/bin/sh
# Pre-flight a change on the remote boxes over Tailscale before pushing:
# build + test + bench on hasu (Linux/glibc/AVX2) and nomad (macOS arm64/NEON).
# FreeBSD 15 is the local dev box, covered by running `gmake test` here.
set -u

HOSTS=${1:-"hasu nomad"}
USER=${TAL_REMOTE_USER:-mfwolffe}
REMOTE_DIR='~/.tally-preflight'

rc=0
for host in $HOSTS; do
	echo "== preflight: $host =="
	ssh "$USER@$host" "rm -rf $REMOTE_DIR && mkdir -p $REMOTE_DIR" || { rc=1; continue; }
	rsync -az --delete \
		--exclude '.git' --exclude '.docs' --exclude 'CLAUDE.md' \
		--exclude 'tests/.work' --exclude 'bench/.work' \
		--exclude '*.o' --exclude '*.d' --exclude '/tally' \
		--exclude 'config.mk' --exclude 'config.h' \
		./ "$USER@$host:$REMOTE_DIR/" || { rc=1; continue; }
	ssh "$USER@$host" "cd $REMOTE_DIR && \
		./configure && \
		(gmake CFLAGS='-O2 -Werror' || make CFLAGS='-O2 -Werror') && \
		(gmake test || make test) && \
		(gmake bench || make bench)" || rc=1
done
exit $rc
