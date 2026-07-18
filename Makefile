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

# Auto-detect each optional dependency and define ISHIZUE_HAVE_<name>
# when pkg-config finds it. The feature is gated behind the macro in
# the source; without it, the relevant code path is a stub.
HAVE_LIBDRM     := $(shell pkg-config --exists libdrm     && echo 1)
HAVE_LIBINPUT   := $(shell pkg-config --exists libinput   && echo 1)
HAVE_LIBSEAT    := $(shell pkg-config --exists libseat    && echo 1)
HAVE_XKBCOMMON  := $(shell pkg-config --exists xkbcommon  && echo 1)

PKG_HAVE_FLAGS :=
ifneq ($(HAVE_LIBDRM),)
PKG_HAVE_FLAGS += -DISHIZUE_HAVE_DRM=1
endif
ifneq ($(HAVE_LIBINPUT),)
PKG_HAVE_FLAGS += -DISHIZUE_HAVE_LIBINPUT=1
endif
ifneq ($(HAVE_LIBSEAT),)
PKG_HAVE_FLAGS += -DISHIZUE_HAVE_LIBSEAT=1
endif
ifneq ($(HAVE_XKBCOMMON),)
PKG_HAVE_FLAGS += -DISHIZUE_HAVE_XKBCOMMON=1
endif

override CFLAGS  += -Iinclude $(PKG_CFLAGS) $(PKG_HAVE_FLAGS)
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

# Tests need the library rebuilt with -DISHIZUE_ENABLE_TEST_HOOKS so the
# isz_test_* symbols are present. Use a separate build dir to avoid
# clobbering the release .o files.
TEST_BUILD_DIR := build-test

test check:
	@mkdir -p $(TEST_BUILD_DIR)
	@find src -name '*.c' | while read f; do \
	    obj=$$(echo "$$f" | sed 's|^src/|$(TEST_BUILD_DIR)/|;s|\.c$$|.o|'); \
	    mkdir -p "$$(dirname "$$obj")"; \
	    $(CC) -std=c11 -Wall -Wextra -Wpedantic -g3 -O0 -fPIC \
	        -DISZ_MAX_SURFACES_PER_CLIENT=$(ISZ_MAX_SURFACES_PER_CLIENT) \
	        -DISZ_MAX_CLIENTS=$(ISZ_MAX_CLIENTS) \
	        -DISZ_MAX_DMABUF_IMPORTS_TOTAL=$(ISZ_MAX_DMABUF_IMPORTS_TOTAL) \
	        -DISZ_THREAD_POOL_SIZE=$(ISZ_THREAD_POOL_SIZE) \
	        -DISZ_MAX_EVENTS_PER_CLIENT=$(ISZ_MAX_EVENTS_PER_CLIENT) \
	        -DENABLE_HDR=$(ENABLE_HDR) \
	        -DENABLE_VRR=$(ENABLE_VRR) \
	        -DENABLE_THREAD_POOL=$(ENABLE_THREAD_POOL) \
	        -DENABLE_HEADLESS=$(ENABLE_HEADLESS) \
	        -DISHIZUE_ENABLE_TEST_HOOKS \
	        $(PKG_HAVE_FLAGS) \
	        -Iinclude $$(pkg-config --cflags libdrm libinput libseat xkbcommon 2>/dev/null) \
	        -c "$$f" -o "$$obj" || exit 1; \
	done
	$(CC) $$(find $(TEST_BUILD_DIR) -name '*.o') -o $(TEST_BUILD_DIR)/$(LIB_NAME) \
	    -shared -Wl,--version-script=libishizue.map \
	    -Wl,-soname,$(LIB_SONAME) \
	    $$(pkg-config --libs libdrm libinput libseat xkbcommon 2>/dev/null)
	ln -sf $(LIB_NAME) $(TEST_BUILD_DIR)/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(TEST_BUILD_DIR)/$(LIB_LINK)
	@$(MAKE) -C tests \
	    CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -g3 -O0 -I../include -I../src/input -DISHIZUE_ENABLE_TEST_HOOKS" \
	    LDFLAGS="-L../$(TEST_BUILD_DIR) -Wl,-rpath,$$(pwd)/$(TEST_BUILD_DIR) -lishizue" \
	    run
	@# Build the x11bridge binary against the test build of libishizue.
	@# Override BRIDGE_LDLIBS / TEST_LDLIBS / TEST_RPATH so both the
	@# bridge and the integration test link against build-test/.
	@$(MAKE) -C x11bridge clean
	@$(MAKE) -C x11bridge \
	    CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -O2 -g" \
	    LDFLAGS="" \
	    BRIDGE_LDLIBS="-L../$(TEST_BUILD_DIR) -Wl,-rpath,$$(pwd)/$(TEST_BUILD_DIR) -lishizue" \
	    TEST_LDLIBS="-L../$(TEST_BUILD_DIR) -Wl,-rpath,$$(pwd)/$(TEST_BUILD_DIR) -lishizue" \
	    TEST_RPATH="$$(pwd)/$(TEST_BUILD_DIR)" \
	    all
	@echo "--- running test_x11_handshake ---"
	@ISZ_X11BRIDGE_BIN=x11bridge/x11bridge \
	    LD_LIBRARY_PATH=$$(pwd)/$(TEST_BUILD_DIR):$$LD_LIBRARY_PATH \
	    ./x11bridge/tests/test_x11_handshake
	@rm -rf $(TEST_BUILD_DIR)

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
