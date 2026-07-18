# tests/Makefile -- W3-B. Honors the parent's `make -C tests run` interface.
#
# The parent Makefile's `test` target invokes this directory with
# CFLAGS (already carrying -DISHIZUE_ENABLE_TEST_HOOKS, -Wall -Wextra
# -Wpedantic, -fPIC, -fvisibility=hidden, and the -D ISZ_* limit macros)
# and LDFLAGS="-L. -lishizue". The parent's -L. and -Iinclude are
# relative to the parent dir, so they miss in this sub-make; we add
# the correct -I and -L paths and relink against the soname.

CC ?= cc

# Defaults for standalone `make -C tests`. The parent's `test` target
# overrides CFLAGS on the command line (with -DISHIZUE_ENABLE_TEST_HOOKS,
# -Wall -Wextra -Wpedantic, -fPIC, -fvisibility=hidden, the -D ISZ_*
# limit macros, etc.), and `?=` yields to that command-line value.
# -DISHIZUE_ENABLE_TEST_HOOKS is mandatory: isz.h hides the test hook
# declarations behind it.
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -g \
	  -DISHIZUE_ENABLE_TEST_HOOKS

# Always-on include paths. The parent's CFLAGS carries -Iinclude,
# which is relative to the parent dir and so misses in this sub-make.
# -I../include    for <ishizue/isz.h>.
# -I../src/input  for "isz_seat_internal.h" (isz_event layout).
override CFLAGS += -I../include -I../src/input

# Replace the parent's broken -L. with -L.. so -lishizue resolves.
# If the parent passes LDFLAGS pointing at a test-build dir (e.g.
# ../build-test), honor it; otherwise default to -L..
ifeq ($(LDFLAGS),)
override LDFLAGS := -L.. -lishizue -Wl,--allow-shlib-undefined
endif

# rpath for the test binary so we don't need LD_LIBRARY_PATH at run time
# (already set by parent when present)
RUN_LDFLAGS = $(LDFLAGS)

TESTS = test_headless_basic

.PHONY: all run clean
all: $(TESTS)

# Build each test from its single .c. The library is built by the
# parent's `all` target before this Makefile runs, so ../libishizue.so
# (and the soname symlink) already exist.
test_headless_basic: test_headless_basic.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

# Run every test. Honor the LDFLAGS-derived library path if the parent
# passed one (e.g. -L../build-test); fall back to the parent dir.
# `set -e` propagates the first failure to make.
run: $(TESTS)
	@set -e; \
	libdir=$$(echo "$(LDFLAGS)" | sed -n 's/.*-L\([^ ]*\).*/\1/p'); \
	[ -z "$$libdir" ] && libdir=..; \
	for t in $(TESTS); do \
	        echo "--- running $$t ---"; \
	        LD_LIBRARY_PATH=$$libdir:$$LD_LIBRARY_PATH ./$$t; \
	done

clean:
	rm -f $(TESTS)
