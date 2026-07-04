# tally — a fast, byte-for-byte GNU wc(1) clone.
# Single Makefile, libc only. C11. See .docs/ for the design.

# Require GNU make. This Makefile uses GNU-only features (pattern rules,
# target-specific vars, -include). BSD make would otherwise silently link an
# objectless, empty binary. .FEATURES is set by every GNU make (>=3.81) and
# unknown to BSD make — which fatally rejects the `ifeq` line below rather than
# building garbage. Use `gmake` on FreeBSD.
ifeq ($(.FEATURES),)
$(error tally's Makefile requires GNU make. Run 'gmake' instead (FreeBSD: pkg install gmake; on macOS /usr/bin/make is already GNU make).)
endif

-include config.mk

CC      ?= cc
PREFIX  ?= /usr/local
BINDIR   = $(DESTDIR)$(PREFIX)/bin
MANDIR   = $(DESTDIR)$(PREFIX)/share/man/man1

# The version string lives in src/version.h (single source of truth, used by
# --version and packaging); do not duplicate it here.
VERSION := $(shell sed -n 's/.*TAL_VERSION "\([^"]*\)".*/\1/p' src/version.h)
DISTNAME = tally-$(VERSION)

WARN     = -Wall -Wextra -Wpedantic -Wstrict-prototypes -Wshadow -Wconversion -Wwrite-strings
STD      = -std=c11
# OPT is the optimization level; release/debug override it cleanly. CFLAGS stays
# free for user-appended flags.
OPT     ?= -O2
ALL_CFLAGS = $(STD) $(WARN) $(OPT) $(CFLAGS) $(CONF_CFLAGS) -Isrc -I. -D_FILE_OFFSET_BITS=64

# Explicit source list — a stray .c in src/ is a deliberate add, not a silent
# one. Keep sorted; tests/run.sh asserts this list matches the filesystem.
SRC = \
	src/count.c \
	src/format.c \
	src/io.c \
	src/main.c \
	src/options.c \
	src/scalar.c \
	src/simd_avx2.c \
	src/simd_neon.c \
	src/simd_sse2.c \
	src/util.c \
	src/sys/detect.c
OBJ = $(SRC:.c=.o)
DEP = $(OBJ:.o=.d)

.PHONY: all clean distclean install uninstall test bench fmt analyze release debug pgo dist

all: config.h tally

config.h config.mk:
	@./configure

tally: $(OBJ)
	$(CC) $(ALL_CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

# ISA-specific flags go ONLY on the matching kernel TU: a global -mavx2 would
# let the compiler autovectorize scalar paths into illegal instructions on
# SSE2-only hosts (runtime dispatch is the whole point).
src/simd_avx2.o: ALL_CFLAGS += $(AVX2_CFLAGS)

%.o: %.c
	$(CC) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

release: OPT = -O3 -flto -DNDEBUG
release: clean all
	@strip tally 2>/dev/null || true

debug: OPT = -O0 -g -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean all

# Opt-in profile-guided build (clang/llvm). Not the default release — packaged
# builds stay plain for reproducibility.
pgo:
	@sh bench/pgo.sh

test: all
	@sh tests/run.sh

bench: release
	@sh bench/run.sh

fmt:
	@command -v clang-format >/dev/null && clang-format -i $(SRC) src/*.h src/sys/*.h || echo "clang-format not found"

analyze:
	@$(CC) $(ALL_CFLAGS) --analyze $(SRC) 2>&1 || true

install: all
	@mkdir -p $(BINDIR) $(MANDIR)
	install -m 0755 tally $(BINDIR)/tally
	install -m 0644 doc/tally.1 $(MANDIR)/tally.1

uninstall:
	rm -f $(BINDIR)/tally $(MANDIR)/tally.1

# Self-contained source tarball for packaging (AUR, Homebrew). Tracked files
# only; portable across GNU and BSD tar.
dist:
	@rm -rf "$(DISTNAME)" "$(DISTNAME).tar.gz"
	@git archive --prefix="$(DISTNAME)/" HEAD | tar -x
	@tar -czf "$(DISTNAME).tar.gz" "$(DISTNAME)"
	@rm -rf "$(DISTNAME)"
	@echo "dist: $(DISTNAME).tar.gz"

clean:
	rm -f $(OBJ) $(DEP) tally

distclean: clean
	rm -f config.h config.mk

-include $(DEP)
