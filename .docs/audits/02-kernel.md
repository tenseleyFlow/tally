# Audit 02 — SIMD kernel design

The fastlwc technique verified from source (github.com/expr-fi/fastlwc, `simd.h`), plus the
parity-preserving extensions tally needs on top of it. Read with audit 01's semantics tables.

## The fastlwc kernel, verified

Per vector V (16/32/64 bytes):
- Whitespace classify: one shuffle + one compare. Low-nibble LUT via pshufb: table maps
  nibble 0→0x20, 9→0x09, A→0x0A, B→0x0B, C→0x0C, D→0x0D, else 0; `pcmpeqb` result against V.
  Bytes ≥ 0x80 shuffle to 0 (pshufb high-bit rule) and never match — UTF-8 lead/continuation
  bytes classify non-space for free. Plain-SSE2 fallback: signed-range trick
  (`add 113, cmpgt 121`) OR cmpeq-space. Our prototype used sub/min/cmpeq — equivalent.
- Word starts: `ws` = whitespace bytemask; `prev` = ws shifted left one byte with carry-in of
  the previous vector's last byte (SSSE3 `palignr(a, b, 15)`; AVX2 `vpalignr` +
  `vperm2i128 0x21`; AVX512 `valignr` + `vpermi2q` — alignr is lane-local on wide vectors).
  `starts = ANDNOT(ws, prev)` = non-space preceded by space. The overview's pseudocode bug
  (`AND(NOT(prev), NOT(ws))`) is real; its own correction below it is right.
- Lines: `pcmpeqb` V, '\n' from the same load.
- Accumulation — the part the overview omits and the biggest kernel-level win: NO per-vector
  movemask+popcount. Subtract the 0xFF/-1 masks from a per-lane s8 accumulator
  (`vcount = sub_epi8(vcount, starts)`), flush every ≤255 iterations via `psadbw`(vcount, 0)
  into u64 lanes. coreutils' own wc_neon.c:46-100 does exactly this (vaddq_s8 + vpaddl cascade
  per 8 KiB). The movemask+popcnt variant (our prototype; fastlwc's SIMD_USE_POPCNT) is the
  fallback where psadbw is awkward.
  Measured in sprint 01: the accumulator alone is NOT enough — a single accumulator
  serializes on the sub dependency chain and lost 16% to GNU's AVX-512 -l kernel. Four
  independent accumulators, 128 B/iteration, merged lane-wise and flushed per ~8 KiB block
  (4*block < 256 or the all-newline worst case wraps u8) put tally ahead on every cell.
  The word kernel inherits this shape.
- State across vectors AND buffers: prev-byte whitespace status seeds as "space" at file start
  (virtual leading space). Tail bytes: pad the final partial vector with 0x20 — spaces create
  no word starts and no newlines; byte count comes from read() returns, so padding is invisible.
- NEON notes: no movemask; use the accumulator scheme + vaddvq/vpaddl reductions; 4x unroll to
  64 B/iter (coreutils wc_neon is the working reference).

## Why the naive kernel is not parity-correct (and the fix)

In every multibyte locale, GNU wc's word-separator set exceeds the six POSIX bytes: the
Unicode spaces per the libc's iswspace, plus the NBSP family {U+00A0, U+2007, U+202F, U+2060}
unless POSIXLY_CORRECT (audit 01). All such characters in UTF-8 begin with a lead byte in
{C2, E1, E2, E3}. Every OTHER non-ASCII character — valid or invalid, lead or continuation —
is a word constituent, exactly like the naive kernel already treats it (encoding errors are
non-space per wc.c:539-549). So word counting needs NO UTF-8 decoding, only detection of the
whitespace-family byte sequences:

- L0 (pure fast path): vector computes `suspect = V ∈ {C2, E1, E2, E3}` alongside ws/nl. If the
  suspect mask is zero for the block — all ASCII text, and all Cyrillic (D0/D1), CJK (E4-E9),
  most of Unicode — the C-locale kernel result is exact. Full speed.
- L1 (discriminator): typography shares the E2 80 prefix with the U+2000 family (curly quotes
  E2 80 98/99, dashes E2 80 93/94), so English prose with smart quotes would suspect-hit
  constantly. Discriminate with shifted compares on the next two bytes: whitespace iff
  C2·{A0,85*}, E1·9A·80, E2·80·{80-8A,A8,A9,AF}, E2·81·{9F,A0}, E3·80·80 (*NEL membership is
  libc-dependent — build the pattern list from the derived set, next section). A position
  matching a whitespace sequence flips that byte run to "space" in the ws mask (lead byte
  carries the space status; continuation bytes of the matched sequence must not create a
  word start — mask them out of `starts`).
- L2 (scalar window): any block where L1 patterns hit at a vector/buffer boundary, or where the
  locale is multibyte-but-not-UTF-8 (GB18030 — mbrtoc32 walk, parity over speed), falls back to
  the scalar reference kernel for a bounded window with carried in_word/mbstate. The scalar
  kernel is the oracle for unit tests; every SIMD path must equal it byte-for-byte on fuzz.

Ship order (sprint 02): L0 + L2 first (L2 = whole rest of buffer on first suspect); add L1
after golden is green. L0 alone covers the perf-gate corpora that matter; L1 recovers
smart-quote prose; measure before optimizing further.
SHIPPED: L1 landed after sprint 02 as designed — kernels return a consumed-byte count and
hold back a 0-2 byte unverifiable tail (suspect lead at n-1, 3-byte lead at n-2) for the
scalar oracle's pend machinery; matches mark all sequence bytes as separators via
carry-shifted masks (equivalence with decode semantics holds because UTF-8 lead bytes never
appear inside another character's encoding, so matches cannot overlap). Measured: typography
15.3x, binary 37.3x (both were scalar-bound), ascii/utf8 cells unchanged thanks to the
per-vector suspect pre-check.

## Deriving the whitespace set at runtime

Never hardcode the Unicode list — libcs disagree (FreeBSD C.UTF-8: NBSP space, NEL not;
glibc: NEL space, NBSP not; wc2's SPEC.md documents a three-way platform matrix). At first
non-ASCII suspect (lazily, not at startup): probe c32isspace() over U+0080..U+3000 plus the
nbspace set, honoring POSIXLY_CORRECT; encode each hit to UTF-8; build the L1 pattern list and
the actual lead-byte set. ~12K iswspace calls ≈ microseconds, amortized once per process.
Known libcs define no space code points above U+3000; the scalar fuzz oracle would catch a
violation. ASCII byte tables are built like wc's own (isspace + btoc32 nbspace check,
wc.c:863-868) at startup — cheap, always needed.

## -m: character counting

chars = number of positions where a valid character starts (invalid bytes consumed one at a
time contribute nothing — audit 01). Kernel: per block, vectorized UTF-8 validation
(Keiser-Lemire "less than one instruction per byte", the simdjson lookup algorithm) fused with
popcount of non-continuation bytes ((b & 0xC0) != 0x80):
- Block validates clean → chars += bytes - continuation_count. This is the overview's formula,
  now guarded by validation.
- Block contains an invalid or boundary-spanning sequence → scalar mbrtoc32 re-walk of that
  block with carried mbstate (matches libc decode edge semantics: overlongs, surrogates,
  > U+10FFFF, truncations — all rejected identically by glibc/FreeBSD and RFC 3629 validators;
  fuzz confirms per box).
- Carry ≤ 3 trailing bytes of an incomplete sequence across buffers; a sequence "filling the
  whole buffer" is an error path (wc.c:519-529) that only pipes can produce.
- Non-UTF-8 multibyte locale: scalar mbrtoc32 for the whole stream. Single-byte locale: -m ≡ -c
  (no read needed beyond what other counters force).

## -L: max line length

Line-structured and stateful (tab stops, CR/FF resets, c32width) — vectorize the scan, not the
width math: vector-classify each block for "specials" (bytes < 0x20, 0x7F, ≥ 0x80). A block
with none is plain printable ASCII: every byte adds 1, so only '\n' positions matter — width
accumulates as run lengths between newlines (positions from the nl mask). Blocks with specials
drop to the scalar loop from the last known position. Real text hits the fast path for nearly
all bytes; GNU's 2.18s/83MB scalar -L (with per-char c32width) is beatable by >10x without
heroics. Do not fuse -L into the lwc kernel; select a separate kernel when -L is requested
(GNU pays c32width only when asked — wc.c:593-598 — and so should we).

## Kernel selection matrix (count.c dispatch)

Selected once per process (counters × locale-class × ISA), not per buffer:

| counters requested | single-byte locale | UTF-8 locale | other multibyte |
|---|---|---|---|
| -c only | fstat path / read-discard | same | same |
| -l (±c) | nl kernel | nl kernel | nl kernel |
| -w, -lw, -lwc (default) | C-locale lwc kernel | lwc + suspect L0/L1/L2 | scalar |
| +(-m) | lwc kernel, chars=bytes | + UTF-8 validate/count | scalar |
| +(-L) | lwc + -L scanner | + -L scanner (c32width) | scalar |

ISA: AVX2 > SSE2 on x86_64 (runtime `__builtin_cpu_supports("avx2")`, baseline SSE2 always
compiled); NEON unconditional on aarch64; scalar elsewhere. AVX-512 deliberately post-v0.1.
The scalar kernels are not an afterthought: they are the parity oracle, written first against
audit 01, and every SIMD kernel is fuzz-diffed against them (then both against GNU wc).

## Cross-buffer state checklist (struct counts)

in_word; linepos (running -L width); pending mbstate/carried partial sequence (≤3 bytes);
suspect-window overlap (L1 patterns are ≤3 bytes: carry the last 2 bytes' classification);
prev-byte-is-space bit for the word kernel. Unit tests must split every fixture at adversarial
offsets (buffer size 1, 2, 3, 4095, 4096, 256Ki-1, 256Ki) — buffer-boundary word splits and
split UTF-8 sequences are where reimplementations die.
