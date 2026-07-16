# Sprint 01 — Core I/O + line counting (M1)

Objective: `tally -l`, `tally -c`, `tally -lc`, and bare `tally FILE|stdin` argv handling for a
single input, with byte-parity output, the `-c` zero-read fast path, and SIMD newline counting
on all three ISAs. Golden phase 2 activates for the -l/-c case matrix. Audits 01 (dispatch,
-c path, width), 02 (nl kernel), 03 (gate policy for the -l near-tie).

## Deliverables

- `src/options.[ch]` — complete GNU-compatible parser: full option table (all of audit 01's
  surface parses now; -w/-m/-L/--files0-from/--total dispatch to a clear "not implemented"
  stub error until their sprints), argv permutation, unambiguous long-option abbreviation,
  `--`, bundled shorts, `=arg` and separate-arg forms, repeated flags idempotent. Diagnostics
  byte-exact in glibc format + "Try 'tally --help' for more information.", exit 1 (audit 01
  §option parsing). Default flag set -lwc when none given.
- `src/io.[ch]` — 256 KiB reads into a 64-byte-aligned buffer (kernels load aligned from it),
  EINTR retry, posix_fadvise SEQUENTIAL where available, read-error reporting with errno.
- `src/count.[ch]` — `struct counts` (overview §5) + kernel dispatch selected once per process;
  scalar byte-discard and scalar newline kernels.
- `src/simd_sse2.c`, `simd_avx2.c`, `simd_neon.c` — newline-count kernels: aligned loads,
  cmpeq '\n', byte-lane accumulation flushed ≤255 iterations (psadbw / vpaddl cascade, audit
  02 §accumulation; coreutils wc_neon.c is the NEON reference), scalar tail. Runtime AVX2
  detection via sys/detect; SSE2 baseline; NEON unconditional on aarch64.
- `-c` fast path: fstat (reusing the width-computation stat), usable-size check (S_ISREG),
  `st_size % pagesize` branch, SEEK_CUR adjustment, st_blksize tail-read for page-multiple
  sizes — audit 01 §-c pseudo, verbatim.
- Output: `write_counts` port (right-align to number_width, fixed column order, " filename"),
  number_width estimator for the single-input cases (1 file + 1 counter → 1; non-regular → 7;
  regular → digits of st_size). stdout line-buffered.
- Behavior: `-` and no-args read stdin (no filename printed for no-args); counts line still
  printed after a mid-file read error, none after an open error; directory operand → EISDIR
  diagnostic path; exit codes 0/1.

## Parity tests

Golden phase 2 ON, cases: {-l, -c, -lc, -cl, --lines, --bytes, abbreviations, permuted
`FILE -l`, `--`} × {empty, 1-byte, newline-only, no-trailing-newline, binary, 256Ki±1, long
single line, /dev/null, missing file, directory, unreadable file, positioned fd via dd,
stdin <file, stdin pipe, `-`}. Unit: nl kernels vs scalar at adversarial buffer splits;
-c fast path against a /proc-like zero-size file when the platform provides one.
Integration (audit 04 §taxonomy): dribble-fed pipe delivers the ascii fixture in 1/3/7/4096-
byte writes — -l/-c counts must equal the whole-file run (exercises the io.c short-read and
EINTR paths the day they're written).

## Pitfalls

- The estimator stats BEFORE reading; `wc -l FILE` prints width 1 (single value, no padding)
  and estimates can under-print — replicate; classified quirk, not bug (audit 01, bug policy
  in sprints/README).
- `-c` on a positioned fd must subtract SEEK_CUR (`dd skip=1k` probe, audit 01); naive
  fstat breaks /proc (page-multiple sizes, including 0).
- Read errors mid-file still print partial counts BEFORE the diagnostic; open errors print
  nothing. Order: stdout line first, stderr second (line-buffering makes this observable).
- getopt diagnostics: three distinct messages (invalid option / unrecognized option / requires
  an argument) — byte-exact, each + Try-help.
- EINTR on read is real on FreeBSD with signals; loop, don't error.
- Do not print "using avx2..." style output unless `--debug` is given (parse --debug now,
  emit info only on the lines path, golden-ignored).

## Performance notes

Gate cells activated: `-l` and `-c` on big-ascii, `-l` on newline-dense and long-lines (min
metric, margin 1.00 — near-tie sanctioned, audit 03 risk 1), plus the ungated startup smoke
row (the startup edge: keep main() allocation-free, no gettext, static tables). tiny-many
needs multi-file operands and activates with sprint 04. Record numbers vs ref in the sprint
log. Landed on this box: -l 1.02-1.05x, -c 1.03x, startup 1.14x — after the 4x-unroll
rework; the naive single-accumulator kernel LOST to GNU's AVX-512 by 16% (its sub-dependency
chain serializes; audit 02 §accumulation now reflects this).

## Exit criteria

Golden -l/-c matrix green on FreeBSD + preflight boxes; unit kernels equal scalar on fuzz
splits; perf gate green for the activated cells; ASan/UBSan clean; `-Werror` clean. Commit in
chunks (options, io, scalar kernels, SIMD kernels, -c fast path, golden cases).
