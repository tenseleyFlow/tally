# Post-v0.1 roadmap

Everything below is recorded headroom from the audits — v0.1.0 already beats the ref on all
24 gated cells, so each item must justify itself with a measured before/after and cannot
regress parity (the suite is the referee, as always). Ordered by value/effort. Baselines in
audit 03 and the v0.1.0 bench report.

## P1 — mmap for large regular files — LANDED 2026-07-05, verdict mixed

Measured: Linux +30% (30->23 ms default cell), macOS +18%, FreeBSD/ZFS 2.6x SLOWER (ZFS ARC
pages aren't shared with the page cache; mapping double-copies). Default ON (4 MiB
threshold) on Linux/macOS, OFF on FreeBSD; TAL_MMAP_MIN overrides, and the golden
parity-mmap phase + fuzz mixing exercise the path on every platform. The audit's "~2x"
projection assumed the copy was pure waste — page-fault cost and the ZFS interaction say
otherwise. SIGBUS truncation guard included; mapped fds advance their offset on release
(wc - - double-counted before the parity-mmap phase caught it, day one).

## P1 (original notes) — mmap for large regular files (audit 03 headroom 3)

The single biggest lever: sys time (the read() copy) is ~half of wall on cached big files
(prototype: 5 of 8.7 ms per 69 MB; v0.1 default cell: ~10 of 20 ms per 200 MB). mmap +
MADV_SEQUENTIAL could approach 2x on every big-file cell at once.
- Gate: S_ISREG && st_size above a measured threshold (mmap setup costs ~10-50us; small
  files stay on read()). Pipes/devices/stdin keep read() unconditionally.
- The hard part is SIGBUS: a file truncated mid-count faults. Options: sigsetjmp guard
  around the counting loop (gnulib-style), or mmap only up to the fstat size and accept the
  race window with a documented recovery. GNU avoids mmap partly for this — get it right or
  don't ship it.
- Kernels are already pointer+length; only count_fd's chunk loop changes. Cross-chunk carry
  logic disappears for the mmap path (one contiguous span).
- Expected: default_big_* cells 22x -> ~35-40x; -l big-file tie -> a real win.

## P2 — build hygiene: split objdirs (small, do first)

debug/release/plain builds share src/*.o and stale sanitizer objects broke links twice
during development. obj/{release,debug}/ trees end the class. Half a day with the SRC
guard updated.

## P3 — binary -m granularity — LANDED 2026-07-07, exceeded target

Reformulated instead of bisected: under wc's resync semantics a valid sequence start can
never sit inside another valid sequence (interiors are continuation bytes, never valid
leads), so chars = count of valid-start positions and lines = count of 0x0A bytes — both
position-independent, both pure vector work, no span validation or rejection at all. The
kernels went from whole-span validate-or-reject to a 3-byte-lookahead valid-start popcount;
the scalar fallback now only sees the <= 34-byte lookahead tail and chunk-boundary pends
(tal_u8scalar, a lean chars-only stepper added en route — it alone took binary from 1.5x
to 2.0x before the reformulation obsoleted the fallback entirely). SSE2 gained a -m kernel
too (no pshufb needed), so baseline x86-64 no longer drops to scalar. Measured on dorado:
binary -m 1.5x -> 60.2x, utf8 -m 39x -> 51x, lm 53x. Unit oracle changed contract: kernel
prefix + scalar suffix must equal the scalar whole (compositional, matches chars_chunk).

## P4 — utf8 -L deepening — LANDED 2026-07-16, target hit

tal_lwalk: width-only walker (classify width rules + oracle pend carry, no word/char
machinery) replaces the full oracle inside the -L special windows for utf8 locales.
Same shape as P3's tal_u8scalar win. Measured: dorado 1.72x -> 5.53x, glibc/hasu
0.97x TIE -> 1.36x, macOS 1.43x -> 4.23x. The 0.90 near-tie margin on L_big_utf8 is
retired (auto). Batch/vector decode was not needed to hit the 4-6x projection.

## P4 (original notes) — utf8 -L deepening (sprint 03 landed-note)

Platform-dependent today: 1.8x FreeBSD, 1.4x macOS, ~0.97x on glibc — glibc's c32width is
fast enough that the cell is a genuine TIE there (caught by ubuntu CI 2026-07-06; the cell
carries an explicit 0.90 margin until this item closes). The per-char decode loop is the
bottleneck everywhere. Batch decode via the u8 validator + width-class lookup on code
points (the BMP table already exists) could reach ~4-6x on the slow-ref platforms and a
real win on glibc. Width is inherently per-character — don't chase ASCII-class multiples.

## P5 — threading, -l slice LANDED 2026-07-16 (opt-in); -w/-m remain

--tally-threads=N / TAL_THREADS (exact-match extension flags, outside the GNU abbreviation
table so --t still means --total). Interleaved pread stripes, sum merge, regular files
>= 8 MiB (TAL_MT_MIN), serial fallback everywhere else, offset semantics mirror mmap.
dorado -l: 23.4 -> 13.1 ms with 4 threads — the read-bound tie becomes 1.84x. auto caps
at 8 (16 threads measured SLOWER than 4 on ZFS: I/O saturates before cores). Golden
check_threads diffs threaded vs serial output; l_mt4_big_ascii gates it. Next slices:
-w/-m need the 2-byte lookback seeding below; -L stitching only if demanded.

## P5 (original notes) — threading (audit 03 headroom 7, fastlwc-mt pattern)

pread over interleaved blocks, one lookback byte seeds in_word per block; L1 suspect holds
need a 2-byte lookback rule at joins. Merge is trivial for -l/-c, mechanical for -w/-m,
genuinely fiddly for -L (per-block first/internal/last partial-line widths stitched).
fastlwc measured ~3x on top of single-thread. Ship order: -l/-c, then -w/-m, -L only if
demanded. Opt-in first (--tally-threads=N per overview §13 extension namespace), default-on
only after pipes/ordering semantics prove clean on all boxes.

## P6 — AVX-512 tier (-l slice LANDED 2026-07-16; lwc/u8 kernels remain)

The -l kernel shipped early to close a red CI leg: mask compares + scalar popcnt, own TU
with -mavx512f/-mavx512bw, runtime-gated. Ice Lake runners flipped 0.91x -> 1.09-1.21x;
dorado (Zen, read-bound) stays a 1.02x tie. The mmap diag on the same runners confirmed
P1: mmap on 6.1ms / off 7.2ms / ref 7.3ms. Remaining: the fused lwc and u8 kernels below.

## P6 (original notes) — AVX-512 tier (overview non-goal, now unblocked; priority raised)

GNU's -l uses it (9.9+); our AVX2 ties read-bound. Ice Lake CI runners measure the gap
directly: 0.91-0.92x on the -l cells (ubuntu, 2026-07-16), so those margins sit at 0.90
until this lands, then go back to 0.97. A 64-byte lwc kernel mostly narrows
user-time on the default cells (~9-11 ms of 20 on the dev box). Needs a new TU +
runtime gate (avx512f+avx512bw) + the vperm2i128 idioms replaced with valignr/permutex2var
(fastlwc shows the shapes). Do after P1 — mmap changes what's bound.

## P7 — buffer-size sweep (audit 03 headroom 4, never finished)

256 KiB matches GNU; 512 KiB/1 MiB were never seriously measured. One bench afternoon;
keep 256 KiB absent a >3% win. Likely moot after P1.

## Not planned

- PGO: measured ~10% slower on the kernels (bench/pgo.sh header).
- macOS startup parity: tally == empty-main at the spawn floor (audit 03 addendum).
- Stateful-charset (ISO-2022) decode carry: no target libc ships such locales.

## Standing chores

- Send .docs/upstream-report.md to bug-coreutils@gnu.org (drafted, unsent).
- AUR publication (packaging/PKGBUILD ready; needs the AUR account push).
- Keep the ref pin current: when coreutils 9.12 lands, re-run stage-1 probes against it
  and bump tests/golden/build-ref.sh + the sha256 pin deliberately.
