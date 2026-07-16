# Audit 03 — measured baselines, gate risks, headroom

All numbers from this box (FreeBSD 15, zen4-class x86_64 with AVX-512, ZFS, page-cache-hot,
hyperfine -w2 -m8) against the built ref wc 9.11. Corpora: `ascii100` = 69,174,040 bytes of
random ASCII words (1.6M lines); `utf8100` = 83,300,000 bytes of Cyrillic+CJK lines (700K
lines, no multibyte whitespace). Prototype = 45-line AVX2 fused lwc kernel (movemask+popcnt
variant, 256 KiB aligned reads), count-verified equal to ref on both corpora.

## Baselines (ref wc 9.11)

| workload | time | rate | notes |
|---|---|---|---|
| default, ascii100, C.UTF-8 | 143.8 ms | 0.48 GB/s | scalar loop, ASCII fast path |
| default, ascii100, LC_ALL=C | 118.0 ms | 0.59 GB/s | UTF-8 locale costs only 1.22x on ASCII |
| default, utf8100, C.UTF-8 | 1361 ms | 61 MB/s | mbrtoc32 per non-ASCII char |
| -w utf8100 | 1333 ms | 62 MB/s | same loop |
| -m utf8100 | 1342 ms | 62 MB/s | (9.11 glibc -m speedup is glibc-only; this is FreeBSD libc) |
| -L utf8100 | 2182 ms | 38 MB/s | + c32width per char |
| -l ascii100 | 7.2 ms | 9.6 GB/s | AVX-512 kernel; 5.4 of 7.2 ms is sys (read copy) |
| -c ascii100 | 1.2 ms | — | fstat path; pure startup+stat |
| --version | 1.0 ms | — | process startup floor |

Prototype: ascii100 8.7 ms (7.9 GB/s), utf8100 9.4 ms (8.9 GB/s) — user time ~4 ms, sys ~5 ms;
even the un-tuned kernel is read()-copy-bound, not compute-bound.

## What the gate can expect

| workload | expected win | basis |
|---|---|---|
| default / -w / -lwc, ASCII | ~16x | 143.8 → 8.7 ms measured |
| default, non-ASCII UTF-8 | ~100-145x | 1361 → 9.4 ms measured |
| -m UTF-8 | ~50-100x | validate+popcount ≈ lwc kernel speed |
| -L | >10x | ASCII screening vs 38 MB/s scalar |
| -l big file | ~1.0-1.2x | BOTH read()-bound; see risk 1 |
| -c regular file | ~1.0-1.2x | both fstat; startup delta only |
| small files / startup | 1.1-2x | see risk 2 |

## Gate risks (fix the gate design now, not after it flakes)

1. -l on big cached files is a near-tie: GNU 7.2±2.1 ms vs prototype 8.7±2.0 ms — σ overlaps
   the delta. tally's -l kernel must match the structure (read into the 64-byte-aligned vector
   buffer, wide accumulation) and will land within noise of GNU. Gate policy: min metric (not
   mean) for workloads under ~20 ms, margin env `TAL_PERF_MARGIN` (ferret precedent), and the
   -l/-c big-file gates assert "not slower" (margin 1.00 on min) rather than "strictly faster
   by mean". The honest -l win comes from startup (below) and shows up in the small-file gate.
2. Startup: ref floor is ~1.0 ms (setlocale + gettext init + dynamic glibc/gnulib). tally links
   no gettext and builds static tables; a lean startup (~0.3-0.5 ms) is the entire margin on
   the tiny-file corpus and on -c/-l big-file ties. Keep main() path allocation-free; defer the
   multibyte whitespace probe until a suspect byte appears (audit 02); setlocale(LC_CTYPE) is
   required for parity and stays.
   Sprint-01 addendum, macOS: the startup edge does not exist there. Measured on nomad-1,
   `tally --version` ties an empty C program at the posix_spawn/dyld floor (~1.0-1.1 ms min,
   sigma 0.4-0.5 ms) while the ref spawns ~0.2 ms under that floor with sigma 0.1 ms — spawn
   and dyld behavior, not our code. Startup-bound cells (smoke, -c fstat) carry an explicit
   0.70 cell margin that beats the runner knob; their tight coverage lives on FreeBSD/Linux.
   The read-bound -l tie cells carry explicit 0.97 (a 1.00 min-margin coin-flips on ~2%
   jitter — observed both directions on dorado). Same macOS runner, -l: 4.26x / 2.17x /
   1.47x over GNU's 9.11 NEON kernel.
3. AVX-512 hosts: GNU -l uses 64-byte vectors (since 9.9); tally v0.1 caps at AVX2. Fine while
   read()-bound (measured: sys time dominates), but the CI perf runner must use cached files
   and the min metric or this gate flakes on Ice-Lake-class runners. Re-evaluate AVX-512
   post-v0.1 (overview non-goal stands).
4. E2-lead-byte text (smart quotes) degrades the L0 fast path (audit 02). The gate corpus must
   include a typography-heavy class so the L1 discriminator's absence in early sprints is
   visible and its addition measurable.
5. FreeBSD's weaker memchr (cw's README note) affects GNU's scalar -l long-line path, not
   tally (own kernels) — don't cite cross-platform GNU numbers measured here as universal;
   preflight on hasu (glibc) before believing a ratio.

## Headroom beyond the overview (priority order)

1. psadbw/byte-lane accumulation instead of per-vector movemask+popcnt (audit 02): the
   prototype already saturates cached read() without it, so its payoff appears on
   memory-resident repeats and wider AVX-512 later. Do it in sprint 02 — it's how fastlwc and
   coreutils' own NEON kernel accumulate, and it removes the port-5 popcnt bottleneck.
2. Read into the vector-aligned buffer (GNU wc_avx2 does; scalar GNU doesn't) — free alignment,
   no realign pass. Already assumed by the kernel design.
3. mmap for large regular files: sys time is ~half of prototype wall time (5 of 8.7 ms). mmap +
   MADV_SEQUENTIAL could approach 2x on big cached files and is the single biggest remaining
   lever on the default-invocation gate. Stays post-v0.1 (overview non-goal — read() is
   universally correct), but sprint 00 wires a bench class so the decision is data-driven.
4. Buffer size: 256 KiB matches GNU (9.6 raised it from 16 KiB for +10%). Try 512 KiB/1 MiB in
   sprint 02 bench; expect diminishing returns; don't ship a difference without a number.
5. fadvise SEQUENTIAL: match GNU (wc.c:397). Cheap, occasionally real on cold files.
6. LTO + PGO release build: ferret precedent (bench/pgo.sh); single-digit % on scalar paths
   and startup. Sprint 06.
7. Threading (fastlwc-mt pattern: pread interleaved blocks, one lookback byte per block seeds
   in_word): 3x more on top of single-thread in fastlwc's data. Correctly stays post-v0.1 —
   ordered multi-file output and pipe semantics make it a real design problem, and the
   single-thread gate already clears every workload.

## Perf gate corpus classes (bench/mkclasses.sh)

big-ascii (~200 MB), big-utf8-cyrcjk (no E2/C2 leads), big-typography (E2 80 quotes/dashes
dense), big-binary (invalid UTF-8), long-lines (>256 KiB single line, exercises rawmemchr-vs-
kernel), newline-dense (1-byte lines), tiny-many (10,000 × 1 KiB, startup+loop), positioned-fd,
pipe-input. Flags per class: default, -l, -w, -m, -L, -c, -lwmcL. Locales: C, C.UTF-8. Gate:
tally faster on every (class × flags × locale) cell, min metric under 20 ms, `TAL_PERF_MARGIN`
override for the two sanctioned near-ties (-l, -c big-file).
