# Sprint 00 — Foundations (M0)

Objective: a git repo that builds `tally` with the full build / test / golden / fuzz / perf / CI
scaffold in place and green. No wc behavior yet — `tally` may print a usage stub. The golden
self-test (ref-vs-ref, coreutils 9.11 wc) must pass so every later sprint inherits a trusted
harness. Port ferret's scaffold (`../refs/../ferret`), find→wc. See audits 00 and 04.

## Deliverables

- `git init`, `.gitignore` (build artifacts, `config.h`, `config.mk`, `tests/.work`,
  `bench/.work`, `.docs/`), `.clang-format` (ferret's: LLVM base, 8-tab, 100 col).
- `configure` — hand-rolled probe writing `config.h` (`TAL_*`) + `config.mk`. Probes:
  `<immintrin.h>` compile test (TAL_HAS_SSE2/TAL_HAS_AVX2), `<arm_neon.h>` (TAL_HAS_NEON),
  `__builtin_cpu_supports("avx2")` availability, `<uchar.h>`+`mbrtoc32` (TAL_HAS_MBRTOC32;
  fallback mbrtowc — macOS, audit 04), `sizeof(wchar_t)==4` (wcwidth usable as c32width),
  `posix_fadvise`, `getpagesize`. Kernel files compile per-probe, never per-`#ifdef`-guess.
- `Makefile` — ferret's, retargeted. GNU-make guard, `-include config.mk`, WARN set
  (`-Wall -Wextra -Wpedantic -Wstrict-prototypes -Wshadow -Wconversion -Wwrite-strings`),
  `-std=c11`, `-D_FILE_OFFSET_BITS=64`, explicit `SRC` list, `-MMD -MP`. Targets: `all release
  debug pgo test bench fmt analyze install uninstall clean distclean`. `release` = `-O3 -flto
  -DNDEBUG` + strip; `debug` = `-O0 -g` ASan/UBSan. Per-file `-mavx2` ONLY on the avx2 kernel TU.
- `src/`: `version.h` (TAL_VERSION), `main.c` stub (--help/--version placeholders, "not
  implemented" to stderr otherwise), `util.[ch]` (error helpers emitting `tally: ...` in glibc
  format), `sys/detect.[ch]` (compile-time + runtime ISA report). All compile under WARN set.
- `tests/run.sh`, `tests/unit/test.h` (ferret's), first units (`util_test.c`, `detect_test.c`),
  Makefile SRC-vs-filesystem drift guard.
- `tests/gen.c` — seeded LCG fixture/fuzz generator (deterministic across platforms; audit 04).
- `tests/golden/build-ref.sh` — build wc 9.11 from `.docs/refs/coreutils` out-of-tree into
  `tests/.work/ref/wc-9.11` with `--disable-nls`, version-guarded, cached.
- `tests/golden/mkcorpus.sh` — base fixture classes from audit 04 (grow per sprint).
- `tests/golden/run.sh` — phase-1 ref-vs-ref self-test over the case matrix; phase 2 gated on
  `tests/golden/PARITY_ACTIVE` (absent this sprint). Normalization: program-name token only.
- `tests/golden/fuzz.sh` — harness present, differential loop wired, inactive until phase 2.
- Bug-policy plumbing (sprints/README, audit 04): `.docs/deviations.md` stub (policy header,
  empty table) and the run.sh deviations-registry hook (`tests/golden/deviations/`, tracked;
  empty) — present from day one so a genuine wc bug found mid-sprint has a paved path and no
  excuse to be replicated "for parity".
- `bench/run.sh` (hyperfine→CSV), `bench/gate.sh` (ferret's, `TAL_PERF_MARGIN`, min-metric
  option), `bench/mkclasses.sh` (audit 03 classes). Gate inactive until PARITY_ACTIVE.
- `ci/preflight.sh` (nomad + hasu over Tailscale), `.github/workflows/ci.yml` — ubuntu,
  macos-14, FreeBSD (vmactions), Alpine/musl; cache ref build + corpora. `doc/tally.1` stub.

## Parity tests

- Golden phase-1: ref builds, version guard rejects a wrong version, ref-vs-ref over the base
  corpus is byte-identical and deterministic twice in a row.
- Locale probe picks a UTF-8 locale on every CI leg (C.UTF-8 → en_US.UTF-8 fallback) and the
  chosen name is recorded in the run log.

## Pitfalls

- The box `wc` is BSD wc — no script may invoke bare `wc`; always the built ref path (audit 04).
- BSD make silently links garbage from `$(wildcard)`: test that `bmake` errors loudly.
- coreutils configure needs several minutes: build-ref MUST cache, CI must cache, and the
  version guard must invalidate stale caches.
- `--disable-nls` on the ref is load-bearing (untranslated "total"/diagnostics = parity target).
- Corpus generation must come from gen.c or literal bytes — shell/awk RNG and even `printf`
  octal-escape behavior differ across platforms.
- `-mavx2` on all TUs would let the compiler autovectorize scalar paths into illegal
  instructions on SSE2-only hosts: isolate ISA flags per kernel TU now.

## Performance notes

`gmake release` produces a stripped LTO binary. hyperfine presence checked (skip loudly if
absent). Nothing to gate yet; bench/run.sh must already produce a parseable CSV against the
stub (e.g. --version) so gate.sh plumbing is proven before it matters.

## Exit criteria

`./configure && gmake` green with `-Werror` on FreeBSD; `gmake test` runs units + golden
phase-1; `gmake debug` ASan/UBSan clean; CI workflow validates on all four legs; preflight
script runs the same pipeline on nomad/hasu. Commit in chunks (repo skeleton, configure,
Makefile, src stub, test harness, golden, bench, CI).
