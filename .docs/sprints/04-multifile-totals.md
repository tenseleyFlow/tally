# Sprint 04 — Multi-file + totals (M4)

Objective: N file operands, the full number_width estimator, `--total` in all four modes, and
`--files0-from` with its slurp/stream split. After this sprint tally's argv surface is
complete. Audit 01 §output format, §totals, §--files0-from are the spec — the width estimator
and the "-" semantics are where the overview was wrong (audit 00 claims 11, 13).

## Deliverables

- Multi-file loop: per-file fstatus array (stat before ANY read — width is an estimate, not a
  result), per-file counts line streamed as each file completes, failures diagnosed and
  skipped with ok=false, later files still processed, exit 1 if any failed.
- number_width estimator, complete port (wc.c:731-788): width 1 default; 1 file + 1 counter
  skips stat; failed stats ignored; non-regular → minimum 7; regular sizes summed with
  saturation; --total=only → width 1; streamed --files0-from → width 1.
- Totals: per-counter accumulation with saturating overflow detection; -L total is max;
  total row after all files. Modes: auto (>1 name processed, counting failed/skipped names),
  always, never, only (per-file lines suppressed, numbers width 1, NO "total" label).
- `--files0-from=F`: F regular and st_size ≤ min(10 MiB, physmem/2) → slurp NUL-delimited
  names (final unterminated token counts); else stream one name at a time. `-` = stdin as F.
  Name "-" in a list reads stdin (audit 00 claim 13); only F=stdin + name "-" errors and
  skips. Zero-length names: "%s:%zu: invalid zero-length file name" with 1-based record
  number (argv form without the prefix). Positional args alongside → "extra operand" +
  "file operands cannot be combined with --files0-from" + Try-help, exit 1. Unopenable F →
  "cannot open %s for reading", exit 1. Stream read error → "%s: read error".
  Empty list → read nothing, exit 0; --total=always still prints a zero total.
- stdin operand repeatable (`tally - -`): second read yields zeros; both lines print "-".

## Parity tests

Golden extends: {2, 3, 10 files} × {mixed sizes, mixed missing/dir/unreadable, stdin mixed
with files, duplicate names, name with space, name with '\n' (defer exact quoting to sprint
05 if not yet ported — activate the case then), UTF-8 name} × {--total=auto/always/only/
never} × flag sets from sprints 01-03; files0-from {small regular list, piped list, list
with "-" (stdin data), F=- with "-" name, zero-length records, missing file in list, empty
list, list with trailing unterminated name}; width probes from audit 01 (stdin → 7; 12-byte
file → 2; 12+2 files → 2; `-l file` → 1; stdin+file → 7; piped list → 1; --total=only → 1).
Fuzz: multi-file mode with 1-3 generated files + random --total.

## Pitfalls

- Width comes from st_size BEFORE reading — do not "fix" misalignment when counts outgrow
  the estimate, and do not re-stat after reading. Classified quirk of the streaming design,
  not a bug (audit 01); the bug policy does not license diverging here.
- total=auto triggers on names PROCESSED (including failures and skipped "-"), not successes.
- The slurp/stream boundary changes user-visible width: a piped list prints width 1 even for
  two regular files. Both branches need golden coverage.
- The estimator ignores failed stats entirely (no minimum-7 bump) — a missing file among
  regulars leaves width = digits of the survivors' sum (probed, audit 00 claim 11).
- readtokens0 semantics: names are NUL-terminated but a final name without NUL still counts;
  an empty F yields zero names, not one empty name.
- fstatus is reused by the -c fast path — stat once, not per concern; for streamed lists,
  fstat happens per file inside wc() (failed=1 reset each iteration).
- physmem/2 in the slurp threshold: use a plain sysconf-based estimate; do not import a
  physmem library — the 10 MiB arm dominates in practice.

## Performance notes

Gate cell: tiny-many (10,000 × 1 KiB) with N files on the command line — the stat-all-first
pass and per-file dispatch overhead show up here; tally's startup edge must survive the
multi-file loop. No new big-file cells. Watch for accidental O(N) allocations per file.

## Exit criteria

Full argv surface golden+fuzz green; width probe table reproduced exactly; tiny-many gate
green; ASan/UBSan clean; `-Werror` clean. Commit in chunks (multi-file loop, estimator,
totals, files0-from, golden cases).
