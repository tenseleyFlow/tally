# tally deviations from GNU wc 9.11

Policy: `.docs/sprints/README.md` §bug policy. Genuine wc bugs are fixed in tally and
recorded here with a repro, the classification rationale, and tally's behavior; each entry
pairs with a fixture under `tests/golden/deviations/<slug>/` pinning tally's output AND the
tally-vs-ref diff (drift or an upstream fix fails the suite loudly). Quirks are replicated
and do NOT belong here — the sanctioned always-on normalizations live in audit 01 §golden.

## Behavioral deviations (genuine bugs fixed)

### 1. Fragmented multibyte separators miscount words (GNU 9.11)

Found 2026-07-04 by the sprint-02 dribble invariance check (the wcstream bug class the
check exists for). In a UTF-8 locale, GNU wc's word count depends on read fragmentation
when adjacent multibyte separators span read boundaries:

    printf 'x\343\200\200\342\200\203y' | wc -w          # via 1-byte reads: 3
    printf 'x\343\200\200\342\200\203y' > f; wc -w f     # whole file: 2

(U+3000 followed by U+2003; 803 vs 937 vs 1519 words on the 8 KiB mbws fixture at
whole/7-byte/1-byte delivery.) Split multibyte constituents and isolated split separators
resume correctly — the trigger needs adjacent separators fragmented mid-sequence.

Classification: genuine bug — identical bytes must count identically regardless of pipe
chunking; GNU's own docs define words over the byte stream, not over read(2) boundaries.
tally: fragmentation-invariant (raw-pend fresh-state decode), always equal to GNU's
whole-file counts — which is also why no golden CASE diverges: regular-file reads only
fragment at the 256 KiB boundary, where a single carried sequence resumes correctly in
both tools (mbsplit/nbspsplit fixtures agree). Enforcement: the golden dribble check
asserts invariance on tally for the mbws class; the ref is exempted from that row.
Upstream: report to bug-coreutils@gnu.org with the two-liner above — pending.

## Presentational deviations (sanctioned normalizations, for reference)

1. Program-name token on stderr and in Try-help lines (`tally:` vs `wc:`).
2. `--help` / `--version` text.
3. `--debug` informational stderr.
4. "total" stays English; GNU translates it under NLS locales (tally ships no translations;
   the golden ref builds `--disable-nls`).
