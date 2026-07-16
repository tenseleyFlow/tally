# Audit 01 — wc 9.11 parity map

Source: `.docs/refs/coreutils/src/wc.c` (line refs), `src/wc.h`, `src/system.h`,
`src/ioblksize.h`. Empirical probes on the built ref, FreeBSD 15, C.UTF-8. This is the
behavioral contract sprints 01-05 implement.

## Dispatch tree (wc.c:384-674)

Per file, wc() picks one of four paths from the requested counters and MB_CUR_MAX:

1. bytes only (-c; or -m in a single-byte locale, since chars≡bytes there, wc.c:389-393):
   fstat/lseek shortcut, else read-and-discard loop. See "-c fast path" below.
2. lines and/or bytes only (-l, -lc): wc_lines() — AVX512 > AVX2 > NEON > scalar
   (wc.c:307-327). Scalar flips between a `*p == '\n'` loop and rawmemchr when average line
   length in the previous block ≥ 15 (wc.c:343-364).
3. multibyte locale general loop (wc.c:475-618): ASCII bytes bypass mbrtoc32 unless mid-sequence
   (wc.c:503-510); non-ASCII decoded by mbrtoc32 with carry of incomplete sequences across
   buffers (prev/memmove, wc.c:519-529; a sequence filling the whole buffer is an error).
4. single-byte locale general loop (wc.c:619-674): byte tables only.

All read paths use 256 KiB reads (IO_BUFSIZE, ioblksize.h:79) and posix_fadvise SEQUENTIAL
(wc.c:396-398).

## Per-character semantics (both general loops)

| char | lines | words | -L (linepos) |
|---|---|---|---|
| `\n` | +1 | ends word | flush linepos→linelength, reset 0 |
| `\r`, `\f` | — | ends word | flush + reset 0 |
| `\t` | — | ends word | round up to multiple of 8 |
| space | — | ends word | +1 |
| `\v` | — | ends word | unchanged |
| other single-byte | — | in word iff !wc_isspace[c] | + wc_isprint[c] (0 or 1) |
| other multibyte | — | in word iff !c32isspace && !nbspace | + max(c32width, 0), only computed if -L requested (wc.c:593-598) |
| encoding error | — | in word (non-space) | +0; chars NOT incremented (wc.c:531-549) |

- Word = space→non-space transition; virtual leading space (in_word=false at start).
- wc_isspace[c] = isspace(c) || c32isnbspace(btoc32(c)) unless POSIXLY_CORRECT (wc.c:866-868).
  nbspace set = {U+00A0, U+2007, U+202F, U+2060} (system.h:158-161).
- linepos flushed at EOF (wc.c:616-617, 672-673) — a final line without '\n' counts for -L.
- State that crosses buffers: in_word, linepos, mbstate + carried bytes. State resets per FILE,
  not per buffer.
- Divergence trap: libcs disagree on iswspace beyond ASCII (FreeBSD C.UTF-8: NBSP is space, NEL
  is not; glibc: reverse). Derive sets from the runtime libc; golden compares same-libc builds.

## -c fast path (wc.c:410-465)

```
if only bytes requested:
  fstat (or reuse the stat from width computation)
  if usable_st_size (REG|SHM|TMO) and st_size >= 0:
    cur = lseek(fd, 0, SEEK_CUR)
    if st_size % page_size != 0:
      bytes = max(0, st_size - cur); lseek(fd, bytes, SEEK_CUR); done, no read
    else:                       # /proc-like files report page-multiple (often 0) sizes
      seek to st_size - st_size % (st_blksize+1); read the tail
  else read whole fd in 256 KiB chunks
```
Probed: `(dd skip=1k count=0; wc -c) < 2k-file` → 1024. Positioned fds and /proc must work.

## Output format (wc.c:261-301, 731-788)

- Column order fixed: lines, words, chars, bytes, maxlinelen, then " filename". Stdin with no
  name prints no filename. Numbers right-aligned to number_width, single space between columns.
- number_width estimated BEFORE any read (compute_number_width, wc.c:759-788):
  - width 1 if: nfiles==0 (streamed --files0-from), or 1 file + 1 counter (no stat done), or
    --total=only (wc.c:931-932).
  - else: any failed stat is ignored; any non-regular file forces minimum 7; regular files
    accumulate st_size into a total; width = max(digits(total), minimum). Saturates at
    UINTMAX_MAX.
  - Probes: stdin default → 7; 12-byte file → 2; two files 12+2 bytes → 2 (digits of 14);
    `wc -l file` → 1; stdin + file → 7.
- Estimator can under-print actual counts, misaligning columns (-L with tabs; huge pipe input
  mixed with regular files; streamed lists). Classified quirk, not bug (tenet 1): a deliberate
  consequence of streaming per-file output the moment each file completes — "fixing" width
  would mean buffering all results and delaying output, a behavioral regression. Replicate.
- Filenames containing '\n' print shell-quoted (quotef, wc.c:296): `'a'$'\n''b'`. Other names
  print raw (probed: spaces unquoted).
- stdout line-buffered (wc.c:812). Write failure → "write error" diagnostic, exit 1 (close_stdout
  atexit + ferror check wc.c:299-300).

## Totals (wc.c:679-680, 1005-1036)

- auto: total line iff >1 name processed (argv_iter count, includes failed/skipped names).
- always: total even for 1 file. never: none. only: per-file lines suppressed (wc.c:679),
  total numbers printed with width 1 and NO "total" label (wc.c:1035).
- Label is `_("total")` — translated under NLS; tally prints "total" always (deviations.md).
- -L total column is the max, not sum (wc.c:687-688).

## --files0-from (wc.c:870-925, 939-977)

- Combined with positional args → "extra operand %s" + "file operands cannot be combined with
  --files0-from" + Try-help, exit 1.
- F regular and st_size ≤ min(10 MiB, physmem/2): slurped; nfiles known; stats drive width.
  Else (pipes, stdin, huge lists): streamed one name at a time; width 1.
- Name "-" inside list F: reads stdin (wc.c:698). Only --files0-from=- + name "-" errors:
  "when reading file names from standard input, no file name of %s allowed", skip, exit 1.
- Zero-length name: argv → "invalid zero-length file name"; from list → "%s:%zu: invalid
  zero-length file name" (record number). Skip, exit 1.
- Unopenable F → "cannot open %s for reading", exit 1 immediately. Read error on stream →
  "%s: read error".
- No names in F at all → nothing read (not stdin), exit 0; --total=always still prints zeros.

## Option parsing (wc.c:816-857)

getopt_long, "clLmw" + longopts. GNU behaviors tally's parser must copy: argv permutation
(`wc f1 -l`), unambiguous long-option abbreviation (`--line`, `--max`, `--tot=only` — all
probed), `--` terminator, bundled shorts (`-lw`), repeated flags idempotent. Diagnostics
(glibc format, program-name-normalized):
- "invalid option -- 'q'" / "unrecognized option '--bogus'" / "option '--total' requires an
  argument", each followed by "Try 'wc --help' for more information.", exit 1.
- --total=bogus → "invalid argument 'bogus' for '--total'" + "Valid arguments are:" + list +
  Try-help, exit 1 (argmatch; quotes are Unicode single quotes under UTF-8 locale — match
  gnulib quote() behavior).
- No flags → default -lwc (wc.c:859-861). --help/--version → stdout, exit 0 (text differs from
  tally's: sanctioned deviation).

## Errors and exit codes

- open/close/read failures: "wc: FILE: strerror" to stderr; ok=false; exit 1; later files still
  processed. Read failure mid-file: partial counts line STILL printed before the diagnostic
  (wc.c:679-691; probed with a directory operand: "0 0 0 /etc" then "Is a directory").
  Open failure: no counts line (probed: permission-denied file).
- Directory operand: open succeeds, read fails EISDIR (Linux and FreeBSD 15).
- stdin read twice (`wc - -`): second gets EOF, prints zeros (probed).
- SIGPIPE: default disposition. No handler anywhere in wc.
- Totals overflow: ckd_add saturation + EOVERFLOW diagnostics (wc.c:1008-1031). Implement the
  saturating math; the diagnostics need 2^64 bytes of input to trigger.

## Golden-suite normalizations (the complete sanctioned list)

1. Program name token on stderr ("wc:" vs "tally:") and in Try-help lines.
2. --help/--version full text.
3. --debug informational stderr lines (hardware-dependent).
4. Translated "total" under NLS locales (tests pin untranslated locales).
5. Registered behavioral deviations — genuine wc bugs tally fixes per the bug policy
   (sprints/README). None exist yet. Each requires a deviations.md entry and a golden
   deviations-registry fixture (audit 04): tally asserted against its own pinned expected
   output, AND the tally-vs-ref diff pinned, so upstream fixes or drift surface loudly.
Everything else — stdout bytes, stderr bytes, exit codes — must be identical. Rule of
classification: quirks (deterministic, defensible design tradeoffs) are parity; only defects
GNU itself would accept as bugs may diverge, and only through the registry.
