# Ishizue build configuration — §4.
#
# Override via env or make args, e.g.:
#   make ISZ_MAX_SURFACES_PER_CLIENT=128
#   make ENABLE_HDR=0
#   make ENABLE_HEADLESS=1 test

# Defaults from SPEC §4.
ISZ_MAX_SURFACES_PER_CLIENT ?= 64
ISZ_MAX_CLIENTS             ?= 32
ISZ_MAX_DMABUF_IMPORTS_TOTAL ?= 256
ISZ_THREAD_POOL_SIZE        ?= 4
ISZ_MAX_EVENTS_PER_CLIENT   ?= 1024

ENABLE_HDR         ?= 1
ENABLE_VRR         ?= 1
ENABLE_THREAD_POOL ?= 1
ENABLE_HEADLESS    ?= 1

# Toolchain
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g \
           -fvisibility=hidden -fPIC \
           -DISZ_MAX_SURFACES_PER_CLIENT=$(ISZ_MAX_SURFACES_PER_CLIENT) \
           -DISZ_MAX_CLIENTS=$(ISZ_MAX_CLIENTS) \
           -DISZ_MAX_DMABUF_IMPORTS_TOTAL=$(ISZ_MAX_DMABUF_IMPORTS_TOTAL) \
           -DISZ_THREAD_POOL_SIZE=$(ISZ_THREAD_POOL_SIZE) \
           -DISZ_MAX_EVENTS_PER_CLIENT=$(ISZ_MAX_EVENTS_PER_CLIENT) \
           -DENABLE_HDR=$(ENABLE_HDR) \
           -DENABLE_VRR=$(ENABLE_VRR) \
           -DENABLE_THREAD_POOL=$(ENABLE_THREAD_POOL) \
           -DENABLE_HEADLESS=$(ENABLE_HEADLESS)
LDFLAGS ?= -shared -Wl,--version-script=libishizue.map \
           -Wl,-soname,libishizue.so.$(shell sed -n 's/.*VERSION_MAJOR *\([0-9]*\).*/\1/p' include/ishizue/version.h)

PKG_DEPS = libdrm libinput libseat xkbcommon

PKG_CFLAGS  := $(shell pkg-config --cflags $(PKG_DEPS) 2>/dev/null)
PKG_LIBS    := $(shell pkg-config --libs   $(PKG_DEPS) 2>/dev/null)

override CFLAGS  += -Iinclude $(PKG_CFLAGS)
override LDFLAGS += $(PKG_LIBS)

SRC_DIRS = src src/backend src/protocol src/render src/input src/buffer src/util
SRCS     = $(foreach d,$(SRC_DIRS),$(wildcard $(d)/*.c))
OBJS     = $(SRCS:.c=.o)
HDRS     = $(wildcard include/ishizue/*.h)

VERSION_MAJOR := $(shell sed -n 's/.*VERSION_MAJOR *\([0-9]*\).*/\1/p' include/ishizue/version.h)
VERSION_MINOR := $(shell sed -n 's/.*VERSION_MINOR *\([0-9]*\).*/\1/p' include/ishizue/version.h)
VERSION_PATCH := $(shell sed -n 's/.*VERSION_PATCH *\([0-9]*\).*/\1/p' include/ishizue/version.h)
LIB_NAME     := libishizue.so.$(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
LIB_SONAME   := libishizue.so.$(VERSION_MAJOR)
LIB_LINK     := libishizue.so

PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include

.PHONY: all clean install uninstall test check
all: $(LIB_NAME) $(LIB_SONAME) $(LIB_LINK)

$(LIB_NAME): $(OBJS) libishizue.map
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(LIB_SONAME): $(LIB_NAME)
	ln -sf $(LIB_NAME) $@

$(LIB_LINK): $(LIB_SONAME)
	ln -sf $(LIB_SONAME) $@

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

test check: CFLAGS += -DISHIZUE_ENABLE_TEST_HOOKS -g3 -O0
test check: all
	@$(MAKE) -C tests CFLAGS="$(CFLAGS)" LDFLAGS="-L. -lishizue" run

install: all
	install -Dm755 $(LIB_NAME) $(DESTDIR)$(LIBDIR)/$(LIB_NAME)
	ln -sf $(LIB_NAME) $(DESTDIR)$(LIBDIR)/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(DESTDIR)$(LIBDIR)/$(LIB_LINK)
	install -d $(DESTDIR)$(INCLUDEDIR)/ishizue
	install -m644 include/ishizue/*.h $(DESTDIR)$(INCLUDEDIR)/ishizue/

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_NAME)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_SONAME)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_LINK)
	rm -rf $(DESTDIR)$(INCLUDEDIR)/ishizue

clean:
	rm -f $(OBJS) $(LIB_NAME) $(LIB_SONAME) $(LIB_LINK)
	$(MAKE) -C tests clean 2>/dev/null || true
