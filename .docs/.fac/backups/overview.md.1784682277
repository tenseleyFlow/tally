# tally — overview

A from-scratch C reimplementation of GNU `wc(1)`. Parity target: coreutils wc (latest stable).
Output matches GNU wc byte for byte; it should be faster on every workload measured, substantially
so on the default invocation. Binary: `tally`.

This is part of a family of "same output, fewer cycles" C rewrites under `tenseleyFlow/`:
`aspen` (tree), `ferret` (find), and now `tally` (wc). Each targets byte-identical parity with
the GNU original and a measurable speed win on every workload, enforced by a CI perf gate. The
stack, conventions, and testing philosophy are shared across all three.

Read alongside the stage-1 audits (when written) in `.docs/audits/`. This document is the
load-bearing design; the sprint files in `.docs/sprints/` implement it incrementally.

---

## 1. Design tenets

1. Parity first, fast second — but never slow. tally is a drop-in for GNU wc. Stdout, stderr,
   and exit codes match coreutils wc for every supported invocation. Speed never regresses parity.
   Where GNU wc has a genuine bug (deterministic but wrong), tally does the correct thing and
   documents the deviation in `.docs/deviations.md`.
2. Beat wc on every workload. The CI perf gate enforces this. The default invocation (lines +
   words + bytes) is the most important benchmark — it is the slowest path in GNU wc and the one
   every user runs.
3. SIMD is the thesis, not a polish step. The entire performance story is vectorized counting.
   Without SIMD, tally is just another wc. The counting kernel ships with SIMD from M1.
4. Few deps, small surface. libc only. No autotools, no CMake. The SIMD code uses compiler
   intrinsics (SSE2/AVX2 on x86, NEON on ARM), not external libraries.
5. Portable by construction. Linux (glibc/musl), FreeBSD 15 (primary dev box), macOS arm64/x86_64.
   Platform differences live behind a thin `sys/` abstraction and compile-time feature detection.

## 2. The performance thesis

GNU wc is fast at counting lines alone (`-l`): it already uses AVX2/AVX512/NEON SIMD. But the
default invocation (`wc file`, which counts lines + words + bytes) is slow because word counting
forces a scalar per-byte loop through the locale machinery.

Where GNU wc wastes time, in priority order:

1. Word counting in multibyte locales. The default invocation hits the general path: `mbrtoc32()`
   to decode each byte, then `iswspace()` to classify it, tracking in-word/out-of-word state
   transitions. In a UTF-8 locale (now universal), this is 5-20x slower than counting in the C
   locale on the same file. The per-byte function-call overhead dominates.
   tally replaces this with a branchless SIMD kernel: vectorize whitespace classification (space,
   `\t`, `\n`, `\r`, `\v`, `\f` — the six POSIX whitespace bytes), shift to get previous-byte
   state, AND to find word-start transitions (non-whitespace preceded by whitespace), popcount.
   This counts words at memory bandwidth, not at function-call speed.

2. Three-pass decomposition. GNU wc has fast paths for `-l` alone and `-c` alone, but the default
   combined mode uses the slow general loop. tally fuses line, word, and byte counting into one
   SIMD pass: the newline comparison, whitespace classification, and byte accumulation all share
   the same loaded vector with near-zero marginal cost per additional counter.

3. Character counting (`-m`). GNU wc calls `mbrtoc32()` per byte to count Unicode characters.
   tally uses a vectorized UTF-8 leading-byte test: `(byte & 0xC0) != 0x80` identifies
   continuation bytes; count = total bytes minus continuation bytes. No multibyte state machine.

4. Max line length (`-L`). GNU wc calls `c32width()` per character for display-width measurement.
   tally fast-paths the ASCII-only case (>99% of lines in practice): if no byte in a line is
   outside 0x20..0x7E, the display width equals the byte count minus tabs (each tab rounds up to
   the next multiple of 8). The multibyte path runs only when a non-ASCII byte is found.

5. I/O layer. GNU wc uses 256 KB buffered `read()`. tally uses the same buffer size (or larger
   if measurement shows a win). `mmap` is an option for large regular files (avoids the
   kernel-to-user copy) but `read()` is the safe default for pipes, sockets, and `/dev/stdin`.
   `-c` alone on a regular file uses `fstat` (zero reads), matching GNU wc's existing fast path.

### The SIMD word-counting kernel (fastlwc approach)

The core technique, proven by `expr-fi/fastlwc` at 30x over GNU wc:

```
for each 16/32/64-byte vector V:
    ws   = classify(V, whitespace_set)    // pcmpeqb x6 + OR, or vpshufb lookup
    prev = shift_right_1(ws, carry)       // previous byte's whitespace status
    starts = AND(NOT(prev), NOT(ws))      // wrong: word start = non-ws preceded by ws
```

Correction: word start = `AND(NOT(ws), prev)` — current byte is non-whitespace AND previous byte
was whitespace. Popcount the resulting mask to get the word count for this vector. Accumulate
newline count from the same loaded data (compare against `'\n'`, popcount). Byte count is just
the number of bytes read.

The six POSIX whitespace bytes (`0x09`-`0x0D`, `0x20`) fit in a 4-bit lookup table, so
`vpshufb` (AVX2) or `vtbl1q_u8` (NEON) classifies 32/16 bytes per instruction. The entire kernel
is branchless and runs at near-memory-bandwidth.

### Prior art

- `fastlwc` (C, expr-fi): 30-94x over GNU wc using SIMD + threading. Single-threaded: 30x.
- `cw` (Rust): 3.4x on `-l`, 70x on `-m`.
- `wc2` (C, robertdavidgraham): 1.4-20x via state-machine parser, no SIMD.
- GNU wc itself has AVX2/AVX512/NEON for `-l` only. The word path is scalar.

tally aims for fastlwc-class single-threaded performance with GNU-wc-parity output.

## 3. Parity contract

Given the same input and flags, tally produces the same output as GNU coreutils wc:

- Same numeric values for lines, words, bytes, characters, max-line-length.
- Same output format: right-aligned columns, same spacing, same column order
  (lines, words, chars, bytes, max-line-length, filename).
- Same column width computation (determined by the largest value across all files).
- Same stderr diagnostic text (program name normalized: `tally:` vs `wc:`).
- Same exit codes. GNU wc: 0 = success; 1 = error on any file.
- Same `--total` behavior (auto/always/only/never).
- Same `--files0-from` behavior (NUL-terminated filenames from a file or stdin).
- Same behavior on stdin, pipes, and special files.
- `-c` on a regular file reports `st_size` without reading (GNU wc does this via lseek; tally
  uses fstat).
- A line is a string terminated by `'\n'`. Bytes after the final `'\n'` are not counted as a
  line (matches GNU wc and POSIX).
- A word is a maximal sequence of non-whitespace characters. Whitespace is defined by
  `iswspace()` in the current locale — in practice, the six POSIX bytes (0x09-0x0D, 0x20) cover
  all real-world text. The SIMD kernel handles these; if a locale defines additional whitespace
  code points (rare), a scalar fallback runs for correctness.

The golden suite compares tally vs GNU wc on a generated corpus. The only normalized difference
is the program-name token on stderr diagnostics.

## 4. Parity surface (GNU coreutils wc)

### Counting flags

| Short | Long | Meaning |
|---|---|---|
| `-c` | `--bytes` | byte count |
| `-m` | `--chars` | character count (multibyte-aware) |
| `-l` | `--lines` | newline count |
| `-w` | `--words` | word count |
| `-L` | `--max-line-length` | display width of longest line |

Default (no flags): `-lwc` (lines, words, bytes).

`-c` and `-m` are mutually exclusive; the last one wins (GNU wc behavior).

### Control flags

| Long | Meaning |
|---|---|
| `--files0-from=F` | read NUL-delimited filenames from F; `-` = stdin |
| `--total=WHEN` | `auto` (default: show total if >1 file), `always`, `only`, `never` |

### Informational

`--help`, `--version`

### Output format

Per-file line: right-aligned columns for each requested counter, then the filename (or nothing
for stdin). Column width is the width of the largest value across all files. Order is always:
lines, words, chars, bytes, max-line-length, filename — regardless of flag order on the command
line. A `total` line appears when there are multiple files (or `--total=always`).

### Parity gotchas

- Column width is computed over ALL files before any output. GNU wc doesn't stream results — it
  buffers all counts, computes the max width, then prints. tally must do the same for multi-file
  invocations (single-file can stream).
- `-c` on a regular file: report `st_size` without reading the file. On a pipe or device: read
  and count. GNU wc uses `lseek(SEEK_END)` then `lseek(SEEK_SET)`; tally can use `fstat`.
- `--files0-from=-` reads filenames from stdin. If a counting flag also reads stdin, GNU wc
  errors: "when reading file names from stdin, no file name of '-' allowed."
- A file named `-` means stdin when it appears as a positional argument, not when it appears in
  `--files0-from`.
- Empty input (zero bytes): 0 lines, 0 words, 0 bytes, 0 chars. Not "1 line."
- The `total` label is literally the string `"total"` — locale-independent in GNU wc.
- Exit code 1 if ANY file fails to open, even if others succeed. Counts for successful files
  are still printed.
- SIGINFO / SIGPIPE handling: GNU wc ignores SIGPIPE (controlled by coreutils infrastructure).
  tally should handle SIGPIPE gracefully (exit silently, not crash).

## 5. Architecture sketch

```
src/
  main.c          # argv -> options -> dispatch -> count -> format -> output
  options.[ch]    # option struct + GNU-compatible parser
  count.[ch]      # the counting engine: dispatch to SIMD or scalar kernel
  simd.[ch]       # SIMD kernels: SSE2, AVX2, NEON (compile-time + runtime detect)
  scalar.[ch]     # scalar fallback for platforms without SIMD, and edge cases
  format.[ch]     # output formatter: column width computation, number printing
  io.[ch]         # I/O layer: buffered read, fstat shortcut for -c
  utf8.[ch]       # UTF-8 leading-byte counter, display-width helpers
  util.[ch]       # xmalloc, error helpers
  version.h
  sys/
    detect.[ch]   # compile-time + runtime SIMD capability detection
```

The counting engine (`count.c`) is the core. It selects a kernel based on:
1. What counters are requested (lines only? words? chars? max-line-length?)
2. Available SIMD width (AVX2 > SSE2 > NEON > scalar)
3. Locale (ASCII-fast-path vs multibyte fallback)

Each kernel is a function that processes a buffer and updates a `struct counts`:

```c
struct counts {
    unsigned long long lines;
    unsigned long long words;
    unsigned long long bytes;
    unsigned long long chars;
    unsigned long long maxlinelen;
    unsigned long long curlinelen; /* running: current line's display width */
    int in_word;                   /* running: inside a word? (for cross-buffer state) */
};

typedef void (*count_fn)(struct counts *c, const unsigned char *buf, size_t len);
```

The top-level loop reads buffers and calls the selected kernel. Cross-buffer state (in_word,
curlinelen, incomplete multibyte sequence) is carried in `struct counts`.

## 6. SIMD architecture

### Tier 1: SSE2 (x86_64 baseline, always available)

16 bytes per iteration. `pcmpeqb` for newline detection, whitespace classification via 6x
`pcmpeqb` + `por` (or a `pshufb` nibble-lookup if SSSE3 is available). `pmovmskb` to extract
a 16-bit mask, `__builtin_popcount` to count.

### Tier 2: AVX2 (x86_64 with AVX2, runtime-detected)

32 bytes per iteration. Same algorithm, wider registers. `vpshufb` for whitespace classification
(the six whitespace bytes map cleanly to a 4-bit nibble lookup). `vpmovmskb` + `popcnt`.

### Tier 3: NEON (ARM, always available on aarch64)

16 bytes per iteration (4x unrolled to 64 bytes). `vceqq_u8` for comparisons, `vtbl1q_u8` for
nibble lookup. Accumulate into `int8x16_t` (handles 255 iterations before overflow; reduce with
`vpaddl` cascade every 8 KB).

### Scalar fallback

Byte-at-a-time with `isspace()` (C locale) or `mbrtoc32()` + `iswspace()` (multibyte locale).
Used when no SIMD is available, or for the tail bytes that don't fill a vector.

### Detection

Compile-time: `configure` probes for `<immintrin.h>` (x86) and `<arm_neon.h>` (ARM). Writes
`TAL_HAS_SSE2`, `TAL_HAS_AVX2`, `TAL_HAS_NEON` to `config.h`.

Runtime (x86 only): `__builtin_cpu_supports("avx2")` gates the AVX2 path. An SSE2 binary that
runs on a machine without AVX2 must not crash. NEON is always available on aarch64 — no runtime
check needed.

## 7. Testing strategy

- Golden parity tests (`tests/golden/`): build GNU wc from coreutils source (pinned version in
  `.docs/refs/coreutils`), generate fixture files (empty, one-byte, all-newlines, all-spaces,
  binary, large, UTF-8 multibyte, mixed, pipe via process substitution), run tally and wc with
  every flag combination, diff stdout/stderr/exit code. A ref-vs-ref self-test runs first.
- Differential fuzzer (`tests/golden/fuzz.sh`): seeded random byte sequences (ASCII, UTF-8,
  binary, mixed) x random flag sets, diffed against GNU wc.
- Unit tests (C): SIMD kernels tested against the scalar fallback on known inputs. Edge cases:
  buffer-boundary word splits, incomplete UTF-8 at buffer end, all-whitespace, no-whitespace,
  single-byte files, lines longer than the buffer, files without a trailing newline.
- Sanitizers: ASan/UBSan on all paths. SIMD kernels tested with aligned and unaligned inputs.
- Perf gate: hyperfine tally vs GNU wc on the fixture corpus (small, medium, large files; C and
  UTF-8 locales; each flag combination). Fail if tally is not faster.

## 8. CI and the performance gate

GitHub Actions matrix: ubuntu (x86_64, AVX2), macos (arm64, NEON), freebsd, musl/Alpine.

Pipeline: build (`-Werror`) -> unit -> golden parity -> fuzzer -> sanitizer build -> perf gate.

Perf gate: hyperfine tally vs GNU wc. Corpus includes a large file (100 MB+) to exercise the
SIMD path and a set of small files to test startup overhead. Fail if tally is not faster on any
workload. The default invocation (lines + words + bytes) is the most important gate — this is
where GNU wc is slowest and where SIMD word counting wins biggest.

Local pre-flight (`ci/preflight.sh`) over Tailscale on `nomad` (macOS arm64, NEON) and `hasu`
(Linux x86_64, AVX2) before big pushes. FreeBSD 15 is the dev box.

## 9. Milestones

- M0 — Skeleton: repo, Makefile, configure probe (SIMD detection), CI, test + golden + perf
  harnesses. Builds `tally`, no behavior yet.
- M1 — Core counting: buffered I/O + SIMD line counting + byte counting + `-c`/`-l` flags +
  single file + stdin. `-c` alone uses fstat (zero reads). Already beats wc on `-l`.
- M2 — Word counting: the SIMD word-counting kernel (the big win). Default `-lwc` invocation
  fused into one pass. Beats wc on the default invocation.
- M3 — Characters and max-line-length: `-m` (vectorized UTF-8 leading-byte count), `-L`
  (display-width with ASCII fast path + multibyte fallback). Tab expansion for `-L`.
- M4 — Multi-file and totals: multiple file arguments, `--total` (auto/always/only/never),
  `--files0-from`, column-width computation, the `total` line.
- M5 — Polish: output format parity (exact column widths, spacing), error handling (missing
  files, permission denied, mixed success/failure exit code), SIGPIPE, edge cases from fuzzer.
- M6 — Portability and release: man page (`doc/tally.1`), packaging (AUR, Homebrew, tarball),
  macOS/FreeBSD/musl edge cases, release automation.

## 10. Sprint index (`.docs/sprints/`)

Each sprint file is self-contained: objective, targets, parity tests, pitfalls, performance
notes, exit criteria. They build on one another in order.

| # | Sprint | Milestone | Delivers |
|---|---|---|---|
| 00 | Foundations | M0 | Makefile, configure probe (SIMD detect), CI, test + golden + perf harnesses |
| 01 | Core I/O + line counting | M1 | buffered read, fstat shortcut, SIMD newline count, `-c`/`-l`, stdin |
| 02 | SIMD word counting | M2 | the fastlwc kernel, fused `-lwc` default, the big win over GNU wc |
| 03 | Characters + max-line-length | M3 | vectorized `-m`, ASCII-fast `-L`, tab expansion, multibyte fallback |
| 04 | Multi-file + totals | M4 | N files, `--total`, `--files0-from`, column-width computation |
| 05 | Output format + edge cases | M5 | exact column parity, error handling, SIGPIPE, fuzzer coverage |
| 06 | Portability + release | M6 | man page, AUR, Homebrew, tarball, cross-platform fixes |

## 11. Stack and constraints

- C11, single Makefile, hand-rolled `configure` probe (writes `config.h` / `config.mk`). No
  autotools, no CMake. GNU make required (`gmake` on FreeBSD).
- Deps: libc only. SIMD via compiler intrinsics (`<immintrin.h>`, `<arm_neon.h>`), not external
  libraries.
- Platforms: Linux x86_64 (`ssh hasu`), macOS arm64 (`ssh nomad`), FreeBSD 15 (this dev box),
  musl/Alpine.
- The SIMD code must compile on all platforms. On platforms without a given SIMD extension, the
  code is `#ifdef`'d out and the scalar fallback runs. The binary is never slower than GNU wc.

## 12. Performance budget (per buffer of N bytes, vs GNU wc)

| Cost | GNU wc (default -lwc, UTF-8 locale) | tally |
|---|---|---|
| decode | N x `mbrtoc32()` | 0 (SIMD on raw bytes) |
| classify whitespace | N x `iswspace()` | N/32 x `vpshufb` + mask (AVX2) |
| count newlines | (in the mbrtoc32 loop) | N/32 x `vpcmpeqb` + `popcnt` |
| count word starts | N x branch (in_word state) | N/32 x shift + AND + `popcnt` (branchless) |
| count bytes | trivial (= bytes read) | trivial |
| I/O | 256 KB `read()` | 256 KB `read()` (or mmap for large files) |

The per-buffer decode + classify cost drops from O(N) function calls to O(N/32) SIMD ops. On a
1 GB file in a UTF-8 locale, GNU wc spends ~5s in `mbrtoc32`+`iswspace`; tally should spend
<0.3s in the SIMD kernel (memory-bandwidth-bound).

## 13. Non-goals (v0.1)

- No new flags beyond GNU wc's surface. Extensions gate behind `--tally-*` flags.
- No parallel counting (threading) in v0.1. Single-threaded SIMD is the thesis; threading is a
  post-release extension if measurement demands it.
- No AVX-512 in v0.1. AVX2 covers >95% of x86_64 machines in service. AVX-512 is a post-release
  extension.
- No mmap in v0.1. Buffered `read()` is correct for all inputs (pipes, devices, regular files).
  mmap is a post-release optimization for large regular files if measurement shows a win.

## 14. Working agreement

- Commit often, in chunks, terse imperative messages (<250 chars). Never co-author or add
  "Generated with" trailers.
- Tests + CI + perf gate block merges. Every behavioral change needs a golden test; every perf
  change needs a before/after benchmark number.
- Pre-flight big changes on `nomad` (macOS) and `hasu` (Linux) over Tailscale before pushing.
- Build with the full `-W` set as `-Werror`; keep ASan/UBSan clean.
- Write prose that a terse engineer would approve of. No boldface for emphasis, no hype, no
  marketing speak. State facts. See `~/.claude/CLAUDE.md` for the full anti-slop guidelines.
- Every choice: parity first, then fastest.
