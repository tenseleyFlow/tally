# Draft: bug-coreutils@gnu.org report (deviation 1)

Ready to send; adjust sender details. Found 2026-07-04 by tally's golden fragmentation
check; full analysis in `.docs/deviations.md` entry 1.

---

Subject: wc: word count depends on read fragmentation with adjacent multibyte spaces

In a UTF-8 locale, wc's word count changes with how the input is chunked
across read() calls when adjacent multibyte space characters span a read
boundary. Same bytes, different counts:

    $ printf 'x\343\200\200\342\200\203y' > f       # x U+3000 U+2003 y
    $ wc -w f
    2 f
    $ dd if=f bs=1 2>/dev/null | wc -w              # 1-byte reads
    3

Observed with coreutils 9.11 (built from the release tarball,
--disable-nls) on FreeBSD 15.0 and Linux/glibc. Denser inputs drift
further: an 8 KiB file mixing U+2003/U+2029/U+3000/NBSP with ASCII words
counts 803 words whole, 937 at bs=7, 1519 at bs=1.

Isolated split characters resume correctly (a single U+3000 split across
reads counts fine, as do split CJK letters and split invalid sequences) —
the trigger seems to need two adjacent multibyte separators fragmented
mid-sequence, suggesting the mbstate/prev-buffer carry in wc()'s multibyte
loop (src/wc.c, the mbrtoc32 resume path) mis-tracks the second character
of the pair.

Reproducible via pipes with any write pattern that splits the pair; regular
files only hit it if a boundary lands inside the pair (single sequences at
IO_BUFSIZE boundaries resume correctly, so file input is normally
unaffected).

---

Send with: the two-command repro above is self-contained; happy to provide
the 8 KiB generator if useful.
