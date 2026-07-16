# tally 0.2.0 (2026-07-16)

Performance release: the post-v0.1 roadmap, plus one parity fix.

- Parity fix: `-w` combined with `-m` overcounted characters when a
  multibyte character straddled a 256 KiB read boundary (the words pass's
  scalar windows leaked char counts). Found by diffing the new threaded
  path against the serial one; GNU agreed with the threaded number.
- `--tally-threads=N` / `TAL_THREADS` extension (opt-in): parallel readers
  for large regular files, covering `-l`, `-w`, and `-m`. Byte-identical
  output; joins are placed only where no multibyte sequence or separator
  can span them. 4 threads on the reference box: default invocation
  30 to 14 ms, `-m` UTF-8 45 to 16 ms per 200 MB.
- mmap fast path for large regular files, default on Linux/macOS (+30%,
  +18%), off on FreeBSD where ZFS makes it slower (`TAL_MMAP_MIN`
  overrides).
- `-m` counts valid UTF-8 sequence starts — position-independent, so
  hostile binary input runs at full vector speed: binary `-m` 1.5x to 99x
  over GNU wc. SSE2 hosts get a vector `-m` kernel too.
- AVX-512 tier for `-l` and `-m` (runtime-gated): `-m` UTF-8 reaches 117x
  over GNU wc on the reference box.
- `-L` display width computes 3-5x faster on UTF-8 text; the glibc tie is
  now a win.
- Per-mode object directories; buffer size measured (256 KiB confirmed).

# tally 0.1.1 (2026-07-05)

- Ship the GPL-3.0 license text (COPYING) — v0.1.0 tarballs lacked it.
- README: install methods (Homebrew tap, AUR, release tarballs), CI badge.
- Homebrew formula live in tenseleyFlow/homebrew-tap.

# tally 0.1.0 (2026-07-04)

First release. A from-scratch reimplementation of GNU wc(1), byte-identical
to coreutils 9.11 and faster on every workload measured.

- Fused SIMD lines+words+bytes kernel (SSE2/AVX2/NEON) with decode-free
  UTF-8 whitespace handling; default invocation 22x (ASCII) to 123x
  (non-ASCII) over GNU wc on the reference x86-64 box.
- Structurally validated `-m` character counting (68x on UTF-8 text).
- Screened `-L` display-width scanner with a cached width table.
- Full GNU surface: `-l -w -c -m -L`, `--total`, `--files0-from`, GNU
  option parsing (permutation, abbreviations), locale and POSIXLY_CORRECT
  semantics, shell-escape filename quoting, byte-exact diagnostics.
- Installs as `tally` and `ty`.
- Documented deviations only (man page DEVIATIONS section), including one
  upstream bug fixed rather than replicated: GNU wc 9.11 miscounts words
  when adjacent multibyte spaces split across read boundaries.
