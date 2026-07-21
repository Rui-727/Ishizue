# SPDX-License-Identifier: MIT
# tinyisz/Makefile - build the tinyisz WM against the in-tree libishizue.so.
#
# The library is built from the repo root with `make` (see ../Makefile).
# tinyisz links against ../libishizue.so with an rpath of $ORIGIN/.. so
# the binary finds the .so when run from tinyisz/ without installing.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
SRCS     = tinyisz.c window.c layout.c input.c
OBJS     = $(SRCS:.c=.o)
BIN      = tinyisz

# Build against the in-tree library.
ROOT     = ..
LIBPATH  = $(ROOT)/libishizue.so
INCDIR   = $(ROOT)/include
LIBDIR   = $(ROOT)

override CFLAGS  += -I$(INCDIR)
# $$ORIGIN escapes make's $$ -> $ expansion; the backslash protects
# the $ from the shell so the linker receives the literal $ORIGIN/...
override LDFLAGS += -L$(LIBDIR) -Wl,-rpath,\$$ORIGIN/.. -lishizue

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJS) $(LIBPATH)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Headless run for smoke testing. Spawns tinyisz, lets it run 2 s,
# sends SIGTERM, expects exit 0.
run: $(BIN)
	./$(BIN) --backend headless & \
	pid=$$!; sleep 2; kill -TERM $$pid; wait $$pid; \
	echo "exit code: $$?"

clean:
	rm -f $(OBJS) $(BIN)
