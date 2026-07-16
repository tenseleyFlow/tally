# Sprint 05 — Output format + edge cases (M5)

Objective: byte-exact diagnostics and quoting, the write-error and signal story, overflow
arithmetic, and a fuzz soak that earns trust. After this sprint the only sanctioned diffs vs
the ref are audit 01's four normalizations, and `deviations.md` exists with every one of them
recorded.

## Deliverables

- Diagnostics catalog, byte-exact (audit 01): open/read/close errors ("tally: FILE: strerror"),
  argmatch for --total ("invalid argument 'X' for '--total'" + "Valid arguments are:" list +
  Try-help — note gnulib quote() uses Unicode single quotes under UTF-8 locales and ASCII
  quotes under C: match both), stdin read-error label "standard input", getopt messages
  finalized from sprint 01.
- Quoting port (minimal quotearg subset): filenames containing '\n' print shell-quoted on
  stdout (`'a'$'\n''b'` form, wc.c:296); diagnostics use quotef (quote-when-needed) vs
  quoteaf (always) exactly where wc does. No full gnulib port — only the cases wc can emit.
- Write-error path: ferror/fflush/fclose checks on stdout at exit → "tally: write error:
  strerror" (or bare "write error" — match ref byte-for-byte), exit 1. /dev/full test on
  Linux legs.
- SIGPIPE: default disposition (die by signal, 141 observable) — verified GNU behavior, no
  handler (audit 00 claim 16). When inherited-ignored, EPIPE surfaces via the write-error
  path. Both covered by tests.
- Totals overflow: saturating adds + EOVERFLOW diagnostics port (wc.c:1008-1031); unit-tested
  via injected counter values (unreachable through real input).
- `--debug`: accepted; prints tally's own acceleration report on the lines-only path;
  golden-ignored stderr (normalization 3).
- `--help`/`--version` final text (tally's own; normalization 2). Line-buffered stdout
  interleaving test (two tallys writing to one pipe produce whole lines).
- `deviations.md` finalized (stub from sprint 00): program-name token; --help/--version text;
  --debug text; untranslated "total" under NLS locales; plus anything the fuzz soak surfaces
  as a genuine GNU bug — which gets FIXED in tally and registered (deviations.md entry +
  golden deviations-registry fixture per audit 04, pinning tally's output and the tally-vs-ref
  diff), never replicated in the name of parity. Quirk-vs-bug calls follow sprints/README.
- Fuzz soak: overnight-scale N across all classes/locales/flag-sets; every failure minimized
  into a permanent golden fixture before the fix lands.

## Parity tests

Golden extends: every diagnostic case (bad option, bad --total under both locales, missing
arg, extra operand, zero-length name argv + list forms, unreadable, directory, missing,
fifo, /dev/null, write error via closed stdout); quoting cases (newline name, space name,
UTF-8 name, name that IS valid shell syntax); signal cases (SIGPIPE default → 141; SIGPIPE
ignored → write-error exit 1); `tally - -`; permutation/abbreviation edge finals
(`--files0-from` vs `--f` ambiguity — F is unique but test the message anyway).
PTY integration lands here (audit 04 §taxonomy): stdin from a pty (line-sized short reads,
EIO at hangup treated as EOF-or-error exactly as the ref treats it), stdout to a pty (line
buffering observable); /dev/full on Linux legs; ulimit -n sanity.

## Pitfalls

- gnulib quote() switches quote glyphs by locale (probed: Unicode quotes in C.UTF-8) — the
  argmatch message differs between LC_ALL=C and UTF-8 legs; golden must cover both.
- The shell-quoted newline-filename form is quotearg's shell_escape style, not simple
  backslashing — port the exact state machine for the reachable cases only, and fuzz names.
- Exit code 141 is the SHELL's rendering of SIGPIPE death; assert termination-by-SIGPIPE in
  the harness, not the numeric $? alone (portability of reporting).
- "write error" must go to stderr once, not per buffered chunk, and still exit 1 when the
  failure only appears at fclose (full-disk case).
- Do not let the soak's minimized fixtures encode libc-specific counts (audit 04 unit rule) —
  store bytes + flags, let the harness compute expectations from the ref.

## Performance notes

No new gate cells. Re-run the full gate matrix after the quoting/diagnostic work — formatting
sits on the hot path for tiny-many (10k files = 10k write_counts calls); keep it snprintf-free
(integer formatter from sprint 01) and confirm no regression cell-by-cell.

## Exit criteria

Full golden matrix green with only the four sanctioned normalizations active; fuzz soak clean
(or every finding fixed + fixtured + deviations.md'd); signal/write-error tests green on all
CI legs; gate matrix unchanged or better; ASan/UBSan clean. Commit in chunks (diagnostics,
quoting, write/signal, overflow, deviations.md, soak fixtures).
