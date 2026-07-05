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
