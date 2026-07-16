# Audit 04 — test & perf harness plan

Port of ferret's proven scaffold (`.docs/refs/../ferret/tests/golden/*, bench/*`), wc-shaped.
Same two-phase golden design, same gate.sh contract, same PARITY_ACTIVE switch.

## Tests are a first-class deliverable

The harness gets the same rigor as the kernels — a parity claim is only as strong as the
suite that enforces it. Every tier below is a shipping requirement, not a nice-to-have, and
every machine we can reach runs it: dorado (FreeBSD 15, dev), hasu (Linux/glibc/AVX2) and
nomad (macOS arm64/NEON) via ci/preflight.sh, plus four CI legs (ubuntu, macos, FreeBSD VM,
Alpine/musl). The tiers, and what each uniquely catches:

1. Unit (tests/unit/, ASan/UBSan always on): pure functions and kernel-vs-scalar-oracle
   equivalence at adversarial buffer splits. Catches logic bugs at the smallest repro.
2. Golden E2E (tests/golden/run.sh): byte-exact stdout/stderr/exit-code diffs vs the built
   ref over the case matrix × locales. Catches contract violations.
3. Differential fuzz (tests/golden/fuzz.sh): seeded random streams × random flags vs the
   ref, plus SIMD-vs-scalar self-fuzz at randomized buffer sizes. Catches what nobody
   thought to fixture; every failure is minimized into a permanent golden case.
4. Integration — the classes E2E files can't reach:
   - Fragmentation invariance: identical bytes delivered through dribble-fed pipes and pty
     reads (1, 3, 7, 4096-byte writes) must produce identical counts to the whole-file run.
     This is exactly how wc2's wcstream exposed a real macOS wc bug (multibyte state lost
     across read boundaries), and tally's cross-buffer carry (in_word, mbstate, suspect
     overlap) is exactly that risk class. Sprint 02 onward, every counter.
   - PTY behavior: stdin from a pty (short line-sized reads, EIO at hangup), stdout to a
     pty (line buffering observable). A small pty helper joins tests/ when sprint 02 wires
     fragmentation tests; script(1)/openpty both work per-platform.
   - Signals and fd edges: SIGPIPE death (141), EPIPE-when-ignored → write error, closed
     stdout (EBADF), /dev/full on Linux, tight ulimit -n sanity.
   - Engagement checks (ferret's strace-io_uring precedent): assert via --debug that the
     AVX2/NEON kernel actually engages on capable hardware — a silent scalar fallback would
     pass every parity test and quietly forfeit the perf thesis.
5. Sanitizers: ASan/UBSan builds re-run unit + golden subset on every CI leg that supports
   them (musl leg documents the exception); SIMD kernels additionally tested on unaligned
   and page-edge buffers.
6. Perf gate (bench/): hyperfine CSV → gate.sh on every corpus cell; regressions are test
   failures, not footnotes.

Cross-machine is not redundancy: FreeBSD/glibc/musl disagree on locale tables (audit 00
claim 8), macOS exercises the mbrtoc32 fallback and NEON, and the golden ref is always
built same-box so each leg proves parity against its own libc.

## Reference oracle

- `tests/golden/build-ref.sh`: build wc from `.docs/refs/coreutils` (9.11, pinned) out-of-tree
  into `tests/.work/ref/wc-9.11`. configure once (~2-4 min) with `--disable-nls` — NLS off
  makes "total" and diagnostics untranslated regardless of host locale, which IS the parity
  target (audit 01). Version-guard: `--version` must report 9.11, else fail loudly. Cache the
  built binary; CI caches tests/.work/ref keyed on the coreutils version + OS.
- The box `wc` is BSD wc — never invoke bare `wc` in any script (ferret's bare-`find` rule).
  Always `$REF` or `$TALLY`.

## Golden runner (tests/golden/run.sh)

- Phase 1 (always): ref-vs-ref self-test over the case matrix — proves corpus + harness
  determinism before any tally diff is trusted.
- Phase 2 (when tests/golden/PARITY_ACTIVE exists): tally-vs-ref. Per case: identical argv,
  stdin, cwd, env; diff stdout byte-for-byte, diff stderr after the ONE normalization (leading
  program-name token, plus Try-help program token), compare exit codes. Pin a private copy of
  the tally binary (ferret's UUT trick) so concurrent rebuilds can't swap it mid-run.
- Env pinning per case: LC_ALL explicitly C or the box's UTF-8 locale (probe C.UTF-8 →
  en_US.UTF-8 fallback at harness start; macOS lacks C.UTF-8), POSIXLY_CORRECT unset by
  default plus dedicated =1 cases, TZ irrelevant, COLUMNS irrelevant.
- Case matrix axes: flags {(none), -l, -w, -c, -m, -L, -lw, -cm, -mc, -lwmcL, --total=auto/
  always/only/never, --files0-from=list/-, --debug(-l only, stderr-ignored), abbreviations
  (--line, --max, --tot=only), permuted (FILE -l), --, bundled} × inputs {fixture files,
  stdin via <, stdin via pipe, multi-file, missing file, directory, zero-length name, "-"}.
- --help/--version and --debug stderr: excluded from diff (sanctioned deviations, audit 01).
- Deviations registry (`tests/golden/deviations/`, tracked in git — .docs/ is not): one dir
  per genuine-bug fix, keyed to its `.docs/deviations.md` entry. The runner asserts such a
  case against a pinned tally-expected fixture instead of the ref, AND diffs tally-vs-ref
  against a pinned expected-diff — if upstream fixes the bug or our behavior drifts, the case
  fails loudly for reclassification instead of rotting. Empty in stage 1 (audit 00 found no
  genuine bugs); the hook ships in sprint 00 so the first fix has a paved path.

## Corpus (tests/golden/mkcorpus.sh + a seeded C generator)

Deterministic across platforms — awk/shell RNGs differ per libc, so fixtures are either
literal here-docs or produced by `tests/gen.c` (tiny LCG, seeded, built by the Makefile).
Fixture classes:
- empty, 1-byte, newline-only, all-spaces, all-tabs, no-trailing-newline, CRLF, \f/\v/\r mix,
  control-bytes (0x01, 0x7F), NUL bytes.
- UTF-8: NBSP (C2 A0), NEL (C2 85), U+1680, U+2000-200A sampler, U+2028/29, U+202F, U+205F,
  U+2060, U+3000; smart-quote/em-dash prose (E2 80 9x — suspect-path false-positive class);
  Cyrillic and CJK bulk; combining marks; zero-width chars.
- Invalid UTF-8: lone continuation, lone C2/E2/F0, truncated 2/3/4-byte at EOF and mid-file,
  overlongs (C0 80, E0 80 80), surrogates (ED A0 80), F5-FF, random binary.
- Boundary geometry: word split at 256Ki; UTF-8 sequence split at 256Ki (file sized so a lead
  byte lands at offset 256Ki-1); line longer than 256Ki; file of exactly 256Ki and 256Ki±1.
- Filesystem shapes: filename with space, with '\n' (quotef case), UTF-8 filename, unreadable
  file (chmod 000, skipped when root), directory, /dev/null, fifo, positioned fd (dd|wc -c),
  /proc-like page-multiple-size file where available.
- files0-from lists: small regular (slurped, width computed), piped (streamed, width 1), list
  containing "-", containing zero-length records, containing a missing file.

## Differential fuzzer (tests/golden/fuzz.sh)

Seeded gen.c streams: classes ascii / utf8-valid / utf8-broken / ws-dense / E2-dense / binary;
random sizes 0..1 MiB biased to 256Ki±4 boundaries; random flag subsets; random 1-3 files +
stdin mode; run tally vs ref, diff as in phase 2. Failure artifact: seed + flags + files copied
to tests/.work/fuzz-failures/ and the repro line printed. N configurable (CI small, soak big).
Second oracle (sprint 02+): SIMD kernels vs tally's own scalar kernel at randomized buffer
sizes (1,2,3,7,4095,4096,...) — catches cross-buffer state bugs libc-independently.

## Unit tests (tests/unit/)

ferret's test.h. Kernel vectors ASCII-only where counts are hardcoded; any >0x7F expectation
must be computed via the libc at test time (audit 01's libc divergence) or asserted only
through the golden/fuzz oracles. Every kernel test runs each fixture at the adversarial buffer
splits (audit 02 checklist).

## Perf harness (bench/)

- mkclasses.sh: audit 03's classes, generated by gen.c (~200 MB big classes; tiny-many tree).
- run.sh: hyperfine → CSV per (class × flags × locale), `-N`, warmups, cached (prime with a
  cat pass); `env LC_ALL=...` inside the command string (hyperfine -N execs directly — no
  shell env prefixes without `env`, learned empirically).
- gate.sh: ferret's, retargeted (`tally` vs `wc-9.11` row match on basename, `TAL_PERF_MARGIN`,
  mean default, min for cells under 20 ms). Gate inactive until PARITY_ACTIVE; from sprint 02
  the default-invocation cell is the flagship gate.

## CI (.github/workflows/ci.yml) and preflight

Matrix: ubuntu-latest (glibc, AVX2; assume no AVX-512), macos-14 (arm64, NEON — also covers
the mbrtoc32/uchar.h portability probe), FreeBSD via vmactions (mirror of dev box), Alpine
container (musl — its C.UTF-8 iswspace tables differ from glibc's: golden absorbs it, unit
tests must not hardcode). Pipeline per leg: configure → gmake -Werror → unit → golden →
fuzz (small N) → ASan/UBSan build re-running unit+golden subset → perf gate (big classes,
cached, min metric). Cache: ref build + corpora. ci/preflight.sh runs the same legs on nomad
(macOS/NEON) and hasu (Linux/AVX2) over Tailscale before big pushes (working agreement).

## Traps ferret already paid for (do not relearn)

- GNU-make guard in the Makefile: BSD make silently mis-builds `$(wildcard)`-based lists.
- Golden runner `set -f` (glob patterns in case matrices must pass literally).
- Version-guard the ref or a stale cached binary invalidates every green run.
- Corpus and fuzzer must chmod-restore before cleanup (trap does `chmod -R u+rwx`).
- Makefile SRC-list-vs-filesystem drift check in tests/run.sh.
- hyperfine may be absent: bench degrades to a skip with a loud message, never a silent pass.
