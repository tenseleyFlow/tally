# Sprint 02 — SIMD word counting (M2)

Objective: the thesis sprint. Fused SIMD lines+words+bytes kernel with parity-exact word
semantics in both C and UTF-8 locales; `tally FILE` (default -lwc) and `-w` beat the ref by
an order of magnitude on the flagship gate. Audit 02 is the spec; audit 01 §per-character
semantics is the contract; audit 03 sets the expected numbers (~16x ascii, ~100x non-ascii).

## Deliverables

- `src/scalar.c` — the reference word kernel, written FIRST: byte tables built like wc's
  (isspace + btoc32-nbspace unless POSIXLY_CORRECT, wc.c:863-868), multibyte path with
  mbrtoc32, encoding-errors-are-word-constituents rule, incomplete-sequence carry, in_word
  across buffers. This is the oracle for every SIMD kernel and the permanent fallback for
  non-UTF-8 multibyte locales (GB18030: parity over speed).
- Whitespace-set derivation (lazy, audit 02): on first suspect byte, probe c32isspace over
  U+0080..U+3000 + nbspace set honoring POSIXLY_CORRECT; emit UTF-8 patterns → L1 list +
  lead-byte set. Never hardcode the Unicode list (FreeBSD/glibc/musl disagree).
- Fused lwc kernels (SSE2/AVX2/NEON): pshufb nibble-LUT whitespace classify (SSE2: signed
  range trick), alignr prev-byte shift with cross-vector carry, `starts = ANDNOT(ws, prev)`,
  newline cmpeq from the same load, psadbw/vpaddl byte-lane accumulation, tail padded 0x20.
- Suspect gating: L0 (`V ∈ {C2,E1,E2,E3}` mask zero → C-locale result exact — covers ASCII,
  Cyrillic, CJK); L2 scalar window (first suspect → scalar to buffer end, carried state).
  Then L1: 3-byte shifted-compare discriminator from the derived pattern list, flipping
  matched sequences to space and masking their continuation bytes out of `starts`.
- Dispatch wiring: default/-w/-lw/-wc select the fused kernel per audit 02's matrix; C-locale
  path uses the byte-table-exact kernel (0xA0 nbsp quirk included via the derived table).
- Cross-buffer state in `struct counts`: in_word, prev-space bit, ≤2-byte L1 overlap, mbstate.

## Parity tests

Golden phase 2 extends: {(none), -w, -lw, -wl, -lwc, -cw} × {ascii prose, NBSP (C2 A0), NEL
(C2 85), U+2000-200A sampler, U+2028/29, U+205F, U+2060, U+3000, smart-quote/em-dash prose,
Cyrillic bulk, CJK bulk, control bytes, lone continuations, truncated sequences at EOF and at
256Ki, binary, all-spaces, no-whitespace 1 MiB, word split exactly at 256Ki} × {UTF-8 locale,
LC_ALL=C} × {POSIXLY_CORRECT unset, =1}. Fuzz ON: ascii/utf8-valid/utf8-broken/E2-dense/
ws-dense classes, random flags from the implemented set. Unit: every SIMD kernel ≡ scalar
oracle on all fixtures at adversarial splits (1,2,3,7,4095,4096,256Ki-1,256Ki).
Integration (audit 04 §taxonomy): fragmentation invariance — every corpus class dribble-fed
through a pipe and a pty in 1/3/7-byte writes must count identically to the whole-file run
(the wcstream bug class; this is THE test for in_word/mbstate/suspect-overlap carry).
Engagement check: --debug must show the SIMD kernel actually selected on capable hardware.

## Pitfalls

- The overview's kernel pseudocode has the inverted-AND bug (audit 02) — the correction is
  `ANDNOT(ws, prev_shifted)`; get the carry-in seeded as "space" at file start.
- alignr is lane-local on AVX2: the vperm2i128 0x21 dance, or words vanish at every 16-byte
  lane boundary (fuzz catches this in seconds — trust the fuzzer, not eyeballs).
- Word count must NOT decode UTF-8: invalid bytes are word constituents (since 9.5). Any
  "validate then count" coupling here is both slower and wrong.
- Smart quotes share E2 80 with the space family — without L1 they must still be CORRECT via
  L2 (slow), never miscounted. Golden's typography class exists exactly for this.
- 0x20-padding the tail is safe for words/lines only — never feed padded vectors to future
  -m/-L logic.
- FreeBSD C.UTF-8 iswspace(NBSP) is true even under POSIXLY_CORRECT (probed) — derived tables
  make this automatic; hardcoded glibc assumptions would fail the same-box golden.
- s8 accumulator overflow: flush before 128 word-starts per lane are possible, not 255
  (starts and newlines can both accumulate; budget per-counter).

## Performance notes

Flagship gate cells activate: default on big-ascii (expect ~16x), big-utf8-cyrcjk (expect
~100x+), big-binary, newline-dense, long-lines, tiny-many; -w same. big-typography is gated
only after L1 lands (L2-only will crawl there; measure L2 → L1 delta and record it). Bench
256 vs 512 KiB vs 1 MiB buffer while the harness is hot; keep 256 KiB absent a measured win.
Compare psadbw accumulation vs movemask+popcnt on big-ascii and keep the winner per ISA.

## Exit criteria

Golden + fuzz green across both locales and POSIXLY_CORRECT states on FreeBSD and preflight
boxes; SIMD ≡ scalar on the unit matrix; flagship gate green with double-digit multiples
recorded in the sprint log; ASan/UBSan clean including the L1/L2 boundary paths. Commit in
chunks (scalar oracle, ws derivation, fused kernel per ISA, L0/L2, L1, gate activation).
