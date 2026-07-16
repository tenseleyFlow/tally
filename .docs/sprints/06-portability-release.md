# Sprint 06 — Portability + release (M6)

Objective: every CI leg green natively (Linux/glibc, macOS/arm64, FreeBSD 15, Alpine/musl),
a real man page, packaging (tarball, AUR, Homebrew tap), and a v0.1.0 release with measured
numbers in the README. Nothing new behaviorally — this sprint is closing platform gaps the
earlier sprints flagged behind configure probes.

## Deliverables

- macOS: mbrtoc32/uchar.h fallback path (mbrtowc + 4-byte wchar_t, probed by configure in
  sprint 00) actually exercised on macos-14; NEON kernels on arm64 verified against the
  scalar oracle there; UTF-8 locale fallback (no C.UTF-8 → en_US.UTF-8) in harness and docs;
  Apple clang quirks fixed under `-Werror`.
- musl/Alpine: golden green with musl's C.UTF-8 (its iswspace tables differ from glibc —
  derived-set design absorbs it, verify); no gnu-isms in scripts (checkbashisms pass);
  static-binary build target documented (`gmake release LDFLAGS=-static` on Alpine).
- FreeBSD: primary box already green; verify a clean-room build from tarball on a fresh
  jail (no repo, no .docs); gmake-vs-make guard message friendly.
- Linux/glibc: preflight on hasu; verify glibc-specific behaviors match ref built there
  (NEL-is-space vs FreeBSD — derived sets again; golden is same-box so this is automatic,
  but run it).
- `doc/tally.1` complete (surface, exit codes, locale behavior, POSIXLY_CORRECT note,
  deviations pointer); `--help` text final and consistent with the man page.
- Packaging: `gmake dist` (tarball from git archive with generated version stamp), AUR
  PKGBUILD, Homebrew formula in the existing tap (`homebrew-tap` repo pattern), install/
  uninstall targets honoring PREFIX/DESTDIR.
- Release automation: tag-driven script (version bump in version.h → tag → tarball →
  checksums → GitHub release with gate report attached); CHANGELOG started.
- README: what/why, parity contract summary, measured gate table from nomad + hasu + this
  box (real numbers, per audit 03 format), build instructions per platform.
- PGO build target exercised (`gmake pgo` using bench corpora as the profile load) — ship
  release binaries plain -O3 -flto; document PGO as a packager option with its measured delta.

## Parity tests

Full golden + fuzz matrix on all four CI legs natively (not just the FreeBSD leg trusted so
far); clean-room tarball build then `gmake test` green; man page examples run as golden cases
(every example in the man page is a real case in the matrix — keep them honest forever).

## Pitfalls

- macOS wchar_t/wcwidth tables differ from glibc — never compare cross-box counts; the golden
  ref is always built on the same box (audit 04).
- vmactions FreeBSD runners are slow: ref-build caching must actually hit or CI times out.
- musl printf lacks some glibc extensions — the integer formatter from sprint 01 avoids the
  issue; verify no %'d style leaks exist.
- `git archive` tarballs must carry a version stamp without .git (gen script writes
  version.h.dist) — a tarball build must not invoke git.
- Homebrew formula tests run `tally --version` in a sandbox — keep it dependency-free and
  fast (it already is; don't add a locale requirement).
- AUR: respect CFLAGS from makepkg; don't force -march=native anywhere (runtime dispatch is
  the whole point).

## Performance notes

Final gate sweep on all preflight boxes + CI legs; archive the CSVs + gate report in the
release. Expected release-note numbers (this box, audit 03): default ~16x ascii / ~100x+
non-ASCII UTF-8, -m ~50-100x, -L >10x, -l/-c parity-or-better with a startup edge. Re-verify
the -l near-tie holds on an AVX-512 host (risk 3) before publishing "faster on every
workload measured".

## Exit criteria

All four CI legs green end-to-end (build, unit, golden, fuzz, sanitizers, gate); clean-room
tarball build works on FreeBSD + Alpine; man page complete and example-tested; v0.1.0 tagged
with release artifacts + gate report; README numbers reproduced by preflight. Commit in
chunks (macOS fixes, musl fixes, man page, packaging, release automation, README).
