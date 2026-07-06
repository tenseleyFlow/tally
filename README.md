# tally

[![ci](https://github.com/tenseleyFlow/tally/actions/workflows/ci.yml/badge.svg)](https://github.com/tenseleyFlow/tally/actions/workflows/ci.yml)

A from-scratch C reimplementation of GNU `wc(1)`. Byte-identical output
(parity target: coreutils 9.11), faster on every workload measured. Also
installs as `ty`, which is easier to type than `wc`.

## Why

GNU wc counts lines with SIMD but counts words with a scalar per-character
loop through the locale machinery. On the default invocation — the one every
user runs — that loop is the whole cost. tally fuses line, word, and byte
counting into one branchless SIMD pass (SSE2/AVX2 on x86-64, NEON on arm64)
that classifies UTF-8 whitespace without decoding, and validates UTF-8 for
`-m` structurally instead of one `mbrtoc32()` call per character.

Parity is enforced, not aspired to: a golden suite diffs tally against a
pinned GNU wc built from source — stdout, stderr, and exit codes, across
locales and `POSIXLY_CORRECT` — plus a differential fuzzer and a CI perf
gate that fails if tally is slower anywhere.

## Measured

hyperfine, 200 MB corpora, page-cache-hot. FreeBSD 15 box: Ryzen-class
x86-64 with AVX-512 (which GNU wc uses; tally caps at AVX2). macOS box:
Apple Silicon.

| workload | vs wc 9.11 (x86-64) | vs wc 9.11 (arm64) |
|---|---|---|
| `wc FILE` (ASCII) | 22x | 14x |
| `wc FILE` (Cyrillic/CJK) | 123x | 73x |
| `wc FILE` (smart-quote prose) | 15x | 5.7x |
| `wc FILE` (binary) | 39x | 27x |
| `-m` (UTF-8) | 68x | 38x |
| `-m` (binary, worst case) | 1.5x | 1.3x |
| `-L` (ASCII) | 10x | 5.2x |
| `-L` (UTF-8) | 1.8x | 1.4x |
| `-l` (big file) | 1.0x (read-bound tie) | 2.8-6.1x |
| 10,000 small files | 1.5x | 1.2x |

## Install

Homebrew (macOS, Linuxbrew):

    brew install tenseleyflow/tap/tally

Arch: a `PKGBUILD` ships in `packaging/`. Release tarballs with checksums
are on the [releases page](https://github.com/tenseleyFlow/tally/releases).

## Build from source

    ./configure && gmake        # BSD: gmake; Linux/macOS: make works too
    gmake test                  # unit + golden parity + fuzz (builds GNU wc
                                # 9.11 from source once as the oracle)
    gmake install               # tally + ty + man pages

Requires a C11 compiler and GNU make. Runs on FreeBSD, Linux (glibc and
musl), and macOS. SIMD is compiled per translation unit and selected at
runtime; the binary never executes instructions the host lacks.

## Parity notes

Same counts, column widths, diagnostics, and exit codes as GNU wc, with
these documented exceptions (`doc/tally.1` DEVIATIONS): the program-name
token, `--help`/`--version` text, `--debug` output, untranslated "total",
and one behavioral fix — GNU wc 9.11 miscounts words when adjacent
multibyte spaces split across read boundaries (reported upstream); tally's
counts are independent of read fragmentation.

## License

GPL-3.0-or-later, like the tool it reimplements.
