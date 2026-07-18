# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rui-727
# See LICENSE for full text.

# x11bridge Makefile: builds the X11 bridge scaffold (W4-C, SPEC §13).
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

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS ?=

CPPFLAGS := -I../include -I../src/protocol -I../src/util

BRIDGE_SRCS := main.c x11_proto.c x11_client.c isz_client.c translation.c
PROTO_SRCS  := ../src/protocol/isz_protocol.c ../src/protocol/isz_conn.c

BUILD_DIR   := build
BRIDGE_OBJS := $(addprefix $(BUILD_DIR)/, $(BRIDGE_SRCS:.c=.o))
PROTO_OBJS  := $(addprefix $(BUILD_DIR)/, $(notdir $(PROTO_SRCS:.c=.o)))
OBJS        := $(BRIDGE_OBJS) $(PROTO_OBJS)

BIN := x11bridge

.PHONY: all clean
all: $(BIN)

$(BIN): $(OBJS)
	@if [ ! -f ../libishizue.so ]; then \
	    echo "x11bridge: ../libishizue.so not found." >&2; \
	    echo "  Build the library first: make -C .." >&2; \
	    exit 1; \
	fi
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) -L.. -lishizue

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Bridge sources: compile in x11bridge/, output .o to build/.
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Protocol sources: compile from ../src/protocol/, output .o to
# build/ (so we do not pollute the library's source tree).
$(BUILD_DIR)/%.o: ../src/protocol/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN)
