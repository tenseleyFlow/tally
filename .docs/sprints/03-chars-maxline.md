# Sprint 03 — Characters + max line length (M3)

Objective: `-m` (validated vectorized character count) and `-L` (screened display-width
scanner), completing the counting surface. All flag combinations select the right kernel via
audit 02's matrix. Audit 00 claims 9/10 are the traps this sprint exists to not fall into.

## Deliverables

- `src/utf8.[ch]` — vectorized UTF-8 validation (Keiser-Lemire lookup algorithm, SSE/AVX2/
  NEON) fused with non-continuation popcount: valid block → `chars += n - continuations`;
  invalid or boundary-spanning block → scalar mbrtoc32 re-walk of that block with carried
  mbstate (each rejected byte consumed singly, chars not incremented — wc.c:531-549 semantics).
  ≤3-byte incomplete-tail carry across buffers; buffer-filling sequence = error path.
- `-m` dispatch: UTF-8 locale → validated kernel; single-byte locale → chars ≡ bytes (reuse -c
  paths including the fstat shortcut when -m is the only counter); non-UTF-8 multibyte →
  scalar oracle. `-cm`/`-mc` print both columns (they coexist — audit 00 claim 14).
- `-L` scanner: vector screen for specials (bytes <0x20, 0x7F, ≥0x80); special-free spans are
  printable-ASCII runs whose widths come from newline positions; specials drop to the scalar
  width loop — tab to next multiple of 8, '\n'/'\r'/'\f' flush+reset, '\v' inert, unprintable
  +0, multibyte via mbrtoc32 + wcwidth (width<0 → +0), computed only when -L requested.
  EOF flushes the final unterminated line. Per-file linepos/maxlinelen state.
- Combined selections: -lwmcL and every subset run one pass with the union of kernels; chars
  and -L never burden the default path (audit 02 matrix).
- `-L` total column semantics: max across files (prep for sprint 04's totals).

## Parity tests

Golden extends: {-m, -cm, -mc, -L, -wL, -lwmcL, --chars, --max-line-length} × {valid CJK/
Cyrillic, combining marks, zero-width chars, NBSP, lone continuation, lone C2/E2/F0, overlongs
(C0 80, E0 80 80), surrogates (ED A0 80), F5-FF, truncated 2/3/4-byte at EOF and at 256Ki,
CRLF, \f/\v/\r mix, control bytes, tab columns (offsets 0,7,8,9), wide-char + tab interleave,
line of exactly 256Ki, no-trailing-newline} × both locales. Probes to reproduce from audit 00:
`a FF b` → -m 2; `FF FF` → 0; `a E2 82` → 1; `ab\rcde` → -L 3; `abcd\fef` → 4; `abc\vde` → 5;
`ab\x01c` → 3; CJK line → 2; LC_ALL=C `a FF b` -m → 3. Fuzz: utf8-broken class weight up;
unit: utf8 kernel ≡ scalar oracle on the split matrix. Integration: fragmentation invariance
for -m and -L (a UTF-8 sequence split across 1-byte pipe reads is the mbstate-carry acid
test; a tab split across reads is the linepos one).

## Pitfalls

- `bytes - continuations` alone overcounts invalid input — validation is what makes the
  formula legal (claim 9). The scalar re-walk must consume rejected bytes one at a time or
  chars drift on garbage runs.
- Validator and libc must agree on rejects (overlong/surrogate/>U+10FFFF): they do on
  glibc/FreeBSD/musl, but the fuzz oracle is the proof per box — don't skip utf8-broken soak.
- '\r' and '\f' END the measured line (reset), '\v' does NOT — three separate golden cases.
- wcwidth(-1) contributes 0, not -1 (clamp); combining marks are width 0 but ARE chars for -m.
- Tab expansion is position-dependent — a tab crossing a vector boundary needs the running
  linepos, not a per-block recompute.
- In single-byte locales `-m` must still take the zero-read fstat path when alone (chars are
  bytes there); breaking that regresses a gate cell.
- mbstate carry interacts with the -L scanner's screening: a multibyte sequence split across
  buffers must decode once, not screen-fast-path once and decode once.

## Performance notes

Gate cells activate: -m on big-utf8 (expect ~50-100x vs 62 MB/s ref), -m on big-ascii, -L on
big-ascii and big-utf8 (expect >10x vs 38 MB/s ref), -lwmcL on big-ascii. -m single-byte
locale must tie -c (fstat path). Record validator throughput standalone; if the fused
validate+count kernel falls under ~4 GB/s, split valid-block screening from counting before
micro-tuning.

LANDED: -m 67.8x utf8 / 13.1x ascii / 1.51x binary (the binary floor forced the oracle onto
a shared inline RFC 3629 decoder — mbrtowc round-trips were losing 1.26x); -L 10.4x ascii /
25x long-lines / 5.2x newline-dense; -lwmcL 6x. utf8 -L needed the lazily built BMP width
table (tal_wcwidth) to clear the gate — a raw wcwidth call per char was a 1.06x tie; the
table gates it at 1.70x. The ">10x" prediction for utf8 -L was wrong: width computation is
inherently per-character; deeper vectorization stays as recorded headroom.

## Exit criteria

Full counting surface golden+fuzz green both locales; unit matrix green; activated gate cells
green with numbers logged; ASan/UBSan clean on utf8-broken soak; `-Werror` clean. Commit in
chunks (utf8 validator, -m dispatch, -L scanner, kernel matrix, golden cases, gate).
