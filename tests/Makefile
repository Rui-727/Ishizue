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
# --allow-shlib-undefined: the .so carries sibling-wave externs
# (isz_backend_blank_all_crtcs, isz_render_send_presented,
# isz_render_send_capture_done) that are not yet defined. They are
# never called on the headless path this test exercises, so allowing
# them to stay unresolved at link time is safe. The flag becomes a
# no-op once those waves land.
override LDFLAGS := -L.. -lishizue -Wl,--allow-shlib-undefined

TESTS = test_headless_basic

.PHONY: all run clean
all: $(TESTS)

# Build each test from its single .c. The library is built by the
# parent's `all` target before this Makefile runs, so ../libishizue.so
# (and the soname symlink) already exist.
test_headless_basic: test_headless_basic.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

# Run every test under LD_LIBRARY_PATH=.. so the dynamic linker finds
# ../libishizue.so.1. `set -e` propagates the first failure to make.
run: $(TESTS)
	@set -e; \
	for t in $(TESTS); do \
	        echo "--- running $$t ---"; \
	        LD_LIBRARY_PATH=..:$$LD_LIBRARY_PATH ./$$t; \
	done

clean:
	rm -f $(TESTS)
