# tools/Makefile - builds the isz_compositor host binary.

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g -I../include
LDFLAGS ?= -L.. -lishizue -Wl,-rpath,$(shell pwd)/..

all: isz_compositor

isz_compositor: isz_compositor.c ../libishizue.so
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

../libishizue.so:
	@$(MAKE) -C .. libishizue.so

clean:
	rm -f isz_compositor

.PHONY: all clean
