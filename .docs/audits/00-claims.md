# Audit 00 — overview.md claims verification

Parity target pinned: GNU coreutils 9.11 (2026-04-20), extracted at `.docs/refs/coreutils`.
Verified three ways: reading `refs/coreutils/src/wc.c` (line refs below), empirical probes against
a wc 9.11 built on this box (FreeBSD 15, C.UTF-8), and web verification of prior-art repos.
Measured numbers are in audit 03. Each claim: VERIFIED or CORRECTED.

## Performance thesis (overview §2)

1. "GNU wc has AVX2/AVX512/NEON for -l only; word path scalar" — VERIFIED for 9.11.
   `wc_avx2.c` (9.0), `wc_avx512.c` (9.9), `wc_neon.c` (9.11). Dispatch at wc.c:307-327 only in
   the lines-only path (wc.c:466-474). Words/chars/-L are scalar loops (wc.c:475-674).
2. "Default invocation decodes with mbrtoc32() per byte; 5-20x slower in UTF-8 locale" —
   CORRECTED. Since at least 9.5 the multibyte loop fast-paths ASCII bytes without calling
   mbrtoc32 (wc.c:503-510) and classifies via byte tables (wc.c:586-587). Measured on this box:
   UTF-8 locale is only 1.22x slower than C locale on ASCII input (143.8ms vs 118.0ms per 69MB).
   The real cliff is non-ASCII input: 61 MB/s (mbrtoc32 per char). The SIMD headroom is real but
   its cause is the per-byte scalar loop, not locale decode: measured 16.5x (ASCII) and 145x
   (non-ASCII) vs an AVX2 prototype (audit 03).
3. "GNU wc uses 256 KB buffered read()" — VERIFIED. IO_BUFSIZE = 256 KiB (ioblksize.h:79),
   raised from 16 KiB in 9.6. The AVX kernels read into a vector-aligned buffer (wc_avx2.c:36).
4. fastlwc "30x single-threaded, 30-94x with threads" — VERIFIED arithmetically (README: 6.55s
   GNU vs 0.22s/0.07s) but measured vs ~2019 coreutils, POSIX locale, page-cache-hot, one machine.
   fastlwc is not parity-exact: single-byte whitespace only, no -m, "target is to behave just
   like Apple's wc". ISAs: SSE2/SSSE3/AVX2/AVX512; no NEON.
5. cw "3.4x on -l, 70x on -m" — CORRECTED. Those figures are vs FreeBSD's BSD wc. Vs GNU wc
   (coreutils 8.30) its README reports 4.17x on -l and 30.2x on -m.
6. wc2 "1.4-20x" — VERIFIED only as a cross-platform composite: 1.43x is the macOS-wc floor,
   20.1x the Linux ceiling (binary input). On uniform input GNU wc ties it (0.284s vs 0.278s).
7. "tally aims for fastlwc-class single-threaded performance" — PLAUSIBLE, measured: a 45-line
   AVX2 prototype fused lwc kernel hit 7.9 GB/s cached vs GNU's 0.48 GB/s (audit 03). Note the
   2026 baseline is faster than the one prior art measured against: AVX512 -l since 9.9, NEON -l
   and a 2.6x-faster glibc -m since 9.11.

## Parity contract (overview §3-4)

8. "Whitespace = iswspace(); the six POSIX bytes cover real-world text" — CORRECTED, three ways.
   (a) NBSP rule: unless POSIXLY_CORRECT is set, U+00A0, U+2007, U+202F, U+2060 are word
   delimiters (system.h:158-161, wc.c:253-256,866-868; behavior since 8.31). NBSP (C2 A0) occurs
   in real text constantly.
   (b) Multibyte locales add Unicode spaces (U+1680, U+2000-200A, U+2028/29, U+205F, U+3000...)
   — and the set is libc-dependent: FreeBSD C.UTF-8 has iswspace(U+00A0) true and iswspace(U+0085)
   false; glibc is the reverse. Probed: `a<C2 A0>b` = 2 words on this box even under
   POSIXLY_CORRECT. The whitespace set must be derived from the runtime libc, never hardcoded.
   (c) Encoding-error bytes count as non-space word constituents (wc.c:539-549, since 9.5).
   Also: unprintable characters count toward words since 9.5 (wc.c:663-666 has no isprint gate)
   — fastlwc/wc2 documentation of GNU's isprint filtering is stale.
9. "-m: count = bytes - continuation bytes" — CORRECTED. Invalid sequences do not increment
   chars (wc.c:531-534): `a FF b` is 2 chars, `FF FF` is 0. Byte-minus-continuation overcounts
   invalid input. Needs UTF-8 validation, not just classification (audit 02 §-m). In single-byte
   locales -m degrades to byte count including invalid bytes (wc.c:390-393,676-677).
10. "-L: width = bytes minus tabs on ASCII lines" — CORRECTED/refined. '\r' and '\f' end the
    measured line (reset linepos, wc.c:562-568); '\v' neither adds nor resets; unprintable ASCII
    adds 0 (wc.c:663); tab rounds up to multiple of 8; last line flushed at EOF without '\n'.
    Probed: `ab\rcde` → 3, `abcd\fef` → 4, `abc\vde` → 5, `ab\x01c` → 3.
11. "Column width buffered over all files before output" — CORRECTED. GNU wc streams per-file
    output; number_width is estimated BEFORE reading from stat (wc.c:731-788): width 1 default;
    1 file + 1 counter → width 1, no stat; any non-regular input → minimum 7; else digits of the
    sum of regular files' st_size. Streamed --files0-from (pipe, or list >10MiB) → width 1;
    slurped list (regular file ≤ min(10MiB, physmem/2), wc.c:898-917) → estimated from stats.
    --total=only → width 1 (wc.c:931-932). tally must replicate the estimator exactly, including
    its misalignments (counts can be wider than the estimate) — classified quirk, not bug; see
    the bug-policy note below.
12. "-c uses lseek(SEEK_END); fstat is equivalent" — CORRECTED. The real algorithm
    (wc.c:410-465): fstat, and if st_size is usable and NOT a multiple of page_size, bytes =
    st_size - lseek(fd,0,SEEK_CUR) and lseek forward (handles pre-positioned fds — probed: dd
    skip=1k of a 2k file then wc -c = 1024). If st_size IS a page multiple (includes /proc's 0),
    seek to one st_blksize before EOF and read the tail. Naive fstat misreports /proc and
    positioned fds.
13. "'-' means stdin as positional arg, not in --files0-from" — CORRECTED. wc_file treats "-" as
    stdin everywhere (wc.c:698-702). Probed: a files0-from FILE containing `-` reads stdin. The
    only special case: --files0-from=- with a name "-" → diagnostic, file skipped, exit 1
    (wc.c:942-950).
14. "-c and -m mutually exclusive; last wins" — CORRECTED. Both coexist; both columns print in
    fixed order. Probed: `wc -cm` ≡ `wc -mc` → "chars bytes".
15. "'total' is locale-independent" — CORRECTED. It is `_("total")`, gettext-translated
    (wc.c:1035). tally ships untranslated English; byte-parity holds only for untranslated
    locales → record in deviations.md.
16. "GNU wc ignores SIGPIPE" — CORRECTED. wc installs no handler; default disposition (dies of
    SIGPIPE, 141). gnulib close_stdout converts EPIPE into "write error" + exit 1 only when
    SIGPIPE is already ignored (inherited). tally: same default disposition + write-error path.
17. Exit codes — VERIFIED: 0 success; 1 for any failed file, bad option, bad --total arg,
    zero-length name, directory operand, unreadable file. --help/--version exit 0. Probed all.
18. "--total behavior" — VERIFIED + refined: 'only' suppresses per-file lines (wc.c:679-680),
    prints no label (wc.c:1035), and uses width 1. 'always' with one file prints total. Probed.
19. Empty input → all zeros — VERIFIED (probed: `0 0 0 0 0` for -lwmcL).
20. Bytes after final '\n' not counted as a line — VERIFIED (wc counts '\n' only; probed).

## Surface omissions in overview §4

- `--debug` is a real, documented flag (wc.c:110, since 9.0): prints acceleration info to stderr
  only when the lines-only path runs (probed: silent on default path). tally needs to accept it;
  its stderr text is a sanctioned deviation (normalize in golden).
- POSIXLY_CORRECT changes word counts (claim 8a) — parity surface, must be pinned in tests.
- GNU option parsing: argv permutation (`wc f1 -l` works), long-option abbreviation (`--line`,
  `--max`, `--tot=only` all probed working), `--` terminator, bundled shorts. tally's parser
  must implement all of it, with glibc-format diagnostics ("unrecognized option '--bogus'",
  "option '--total' requires an argument", "invalid option -- 'q'").
- Filenames containing '\n' are shell-quoted on stdout (wc.c:296, since 8.25). Probed:
  `'a'$'\n''b'`. Diagnostics quote via quotef/quoteaf.
- Counts line still prints (with partial counts) when read fails mid-file (wc.c:679 before
  error at :691); open failures print no counts line. Probed: directory operand → counts line
  then "Is a directory" on stderr, exit 1.
- Read-error diagnostic for stdin uses the translated label "standard input" (wc.c:378).
- stdout is line-buffered for atomic per-file lines (wc.c:812).
- Totals use saturating overflow detection with EOVERFLOW diagnostics (wc.c:682-685,1008-1031)
  — unreachable with real inputs; implement the arithmetic, skip the diagnostics until fuzz says
  otherwise.

## Bug policy applied to these findings (overview tenet 1)

None of the corrections above is a genuine wc bug — they correct what the overview believed
about wc, not wc itself. The two candidates were evaluated and classified quirks: the width
estimator's misalignments (deliberate consequence of streaming per-file output instead of
buffering; counts stay correct and deterministic) and the partial-counts line printed after a
mid-file read error (deliberate report-what-was-counted choice; BSD wc differs, POSIX doesn't
settle it). Both are replicated exactly. The policy machinery still ships from sprint 00
(deviations.md stub, golden deviations registry — audit 04) so the first genuine bug found,
most likely by the sprint-05 fuzz soak, gets fixed and documented rather than replicated.

## Verdict on the plan (overview §5-13)

Architecture, milestones, and non-goals stand. Three plan-level adjustments:
1. The multibyte word kernel needs the suspect-lead-byte design (audit 02) — a plain
   6-byte-whitespace SIMD kernel is NOT parity-correct in UTF-8 locales (claims 8a/8b), and
   UTF-8 locales are the default everywhere.
2. The -l perf gate will be a near-tie, not a win, on cached big files: GNU -l is already
   read()-bound SIMD (7.2ms vs prototype 8.7ms per 69MB — the delta is one extra pass of work
   the prototype does). Gate policy and where tally's -l edge comes from: audit 03.
3. -m needs UTF-8 validation (claim 9); budget it as its own kernel, not a popcount afterthought.
