# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rui-727
# See LICENSE for full text.

# x11bridge Makefile: builds the X11 bridge binary (W4-C, SPEC §13)
# and the W7-C integration test.
#
# The bridge is a separate process. It speaks the Ishizue wire
# protocol as a client. The framing primitives (isz_proto_*,
# isz_conn_*) are ISZ_INTERNAL (hidden) in libishizue.so, so the
# bridge cannot link them from the .so. Instead, the Makefile compiles
# ../src/protocol/isz_protocol.c and isz_conn.c directly into the
# binary, keeping the framing code in sync with the library without
# exporting internal symbols. The bridge also links ../libishizue.so
# for the public API; the bridge calls no public isz_* function yet
# (it is a wire-protocol client), but the DT_NEEDED entry is set up
# for future use and per the W4-C spec.
#
# The test binaries (tests/test_x11_handshake, tests/test_x11_opcodes)
# are integration tests that spawn the bridge against a headless
# Ishizue server running in the test process. They link only the
# public API; no bridge internals are pulled in.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS ?=

CPPFLAGS := -I../include -I../src/protocol -I../src/util

# Default link flags: link against ../libishizue.so (the regular
# release build). The parent `test` target overrides these to point
# at the build-test/ variant compiled with ISHIZUE_ENABLE_TEST_HOOKS.
BRIDGE_LDLIBS ?= -L.. -lishizue
TEST_LDLIBS   ?= -L.. -lishizue
TEST_RPATH    ?= $(shell cd .. && pwd)

BRIDGE_SRCS := main.c x11_proto.c x11_atoms.c x11_client.c isz_client.c translation.c
PROTO_SRCS  := ../src/protocol/isz_protocol.c ../src/protocol/isz_conn.c

BUILD_DIR   := build
BRIDGE_OBJS := $(addprefix $(BUILD_DIR)/, $(BRIDGE_SRCS:.c=.o))
PROTO_OBJS  := $(addprefix $(BUILD_DIR)/, $(notdir $(PROTO_SRCS:.c=.o)))
OBJS        := $(BRIDGE_OBJS) $(PROTO_OBJS)

BIN       := x11bridge
TEST_BIN  := tests/test_x11_handshake
TEST_SRC  := tests/test_x11_handshake.c
TEST2_BIN := tests/test_x11_opcodes
TEST2_SRC := tests/test_x11_opcodes.c

.PHONY: all clean test
all: $(BIN) $(TEST_BIN) $(TEST2_BIN)

$(BIN): $(OBJS)
	@if [ ! -f ../libishizue.so ] && [ "$(BRIDGE_LDLIBS)" = "-L.. -lishizue" ]; then \
		echo "x11bridge: ../libishizue.so not found." >&2; \
		echo "  Build the library first: make -C .." >&2; \
		exit 1; \
	fi
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(BRIDGE_LDLIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Bridge sources: compile in x11bridge/, output .o to build/.
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Protocol sources: compile from ../src/protocol/, output .o to
# build/ (so we do not pollute the library source tree).
$(BUILD_DIR)/%.o: ../src/protocol/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Test binaries: built from tests/*.c, linked against libishizue.so
# (public API only). They do NOT link bridge objects; the tests
# spawn the bridge as a subprocess.
$(TEST_BIN): $(TEST_SRC)
	@mkdir -p tests
	$(CC) $(CFLAGS) -I../include $(LDFLAGS) -o $@ $< \
		$(TEST_LDLIBS) -Wl,-rpath,$(TEST_RPATH)

# W8-A integration test: exercises MapWindow, UnmapWindow,
# ChangeWindowAttributes, ConfigureWindow, DestroyWindow, InternAtom,
# ChangeProperty, GetProperty.
$(TEST2_BIN): $(TEST2_SRC)
	@mkdir -p tests
	$(CC) $(CFLAGS) -I../include $(LDFLAGS) -o $@ $< \
		$(TEST_LDLIBS) -Wl,-rpath,$(TEST_RPATH)

# Build + run the integration tests. Runs from the repo root so the
# default bridge binary path (x11bridge/x11bridge) resolves.
test: $(BIN) $(TEST_BIN) $(TEST2_BIN)
	@cd .. && \
		ISZ_X11BRIDGE_BIN=x11bridge/x11bridge \
		LD_LIBRARY_PATH=$(TEST_RPATH):$$LD_LIBRARY_PATH \
		./x11bridge/tests/test_x11_handshake
	@cd .. && \
		ISZ_X11BRIDGE_BIN=x11bridge/x11bridge \
		LD_LIBRARY_PATH=$(TEST_RPATH):$$LD_LIBRARY_PATH \
		./x11bridge/tests/test_x11_opcodes

clean:
	rm -rf $(BUILD_DIR) $(BIN) $(TEST_BIN) $(TEST2_BIN)

