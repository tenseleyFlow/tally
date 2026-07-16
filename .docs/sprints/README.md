# tally sprints

Self-contained, ordered. Each: objective, deliverables, parity tests, pitfalls, performance
notes, exit criteria. Read with `../overview.md` and `../audits/` (00 claims, 01 parity map,
02 kernel design, 03 perf baselines, 04 harness).

| # | Sprint | Milestone | Delivers |
|---|---|---|---|
| [00](00-foundations.md) | Foundations | M0 | Makefile, configure probe, CI, golden+fuzz+perf harness |
| [01](01-core-io-lines.md) | Core I/O + line counting | M1 | GNU parser, buffered I/O, -c fast path, SIMD -l, stdin |
| [02](02-simd-words.md) | SIMD word counting | M2 | fused lwc kernel, suspect gating, default invocation win |
| [03](03-chars-maxline.md) | Characters + max line length | M3 | validated -m kernel, screened -L scanner, kernel matrix |
| [04](04-multifile-totals.md) | Multi-file + totals | M4 | N files, width estimator, --total, --files0-from |
| [05](05-format-edges.md) | Output format + edge cases | M5 | diagnostics catalog, quoting, write errors, fuzz soak |
| [06](06-portability-release.md) | Portability + release | M6 | macOS/musl/FreeBSD legs, man page, packaging, release |

Parity target: GNU coreutils 9.11 wc, built --disable-nls from `.docs/refs/coreutils`,
program-name token normalized on stderr. Exit codes: 0 ok, 1 any error. Column order fixed:
lines, words, chars, bytes, maxlinelen, filename. Perf gate: hyperfine CSV via bench/gate.sh,
`TAL_PERF_MARGIN`, min metric under 20 ms; tally faster on every corpus cell, near-tie margin
sanctioned only for big-file -l/-c (audit 03). Binary: `tally`. The scalar kernels are the
parity oracle; SIMD kernels must fuzz-equal them, and both must golden-equal the ref.

Tests and CI are first-class deliverables (audit 04 §taxonomy): unit, golden E2E,
differential fuzz, integration (fragmentation invariance over dribble-fed pipes/ptys,
signals, fd edges, kernel-engagement checks), sanitizers, and the perf gate — run on every
reachable machine (dorado, hasu, nomad, four CI legs). A sprint isn't done because the code
works; it's done when the suite that would catch its regressions exists and is green
everywhere. Harness code gets reviewed and polished like kernel code.

Bug policy (overview tenet 1): parity never codifies a genuine wc bug. Genuine bug = output
or semantics GNU itself would accept as a defect (wrong counts, crash, behavior contradicting
wc's own documented intent). Quirk = deterministic, defensible design tradeoff (streaming
width estimate, translated "total", /proc size handling) — quirks are replicated exactly.
A fix requires: a `.docs/deviations.md` entry (repro, classification rationale, tally's
behavior), a golden deviation fixture (audit 04) pinning both tally's output and the
tally-vs-ref diff, and an upstream report when the defect is clear-cut. Ambiguous cases get
classified in deviations.md before code ships either way. Stage 1 found zero genuine bugs —
every audit-00 correction is a fix to our contract, not to wc.
