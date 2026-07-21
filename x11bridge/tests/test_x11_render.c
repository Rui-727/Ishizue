/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Rui-727
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* test_x11_render.c: W10-A integration test.
 *
 * Spawns the x11bridge binary against a headless Ishizue server
 * running in the test process, then connects to the bridge's UDS as a
 * raw X11 client (no libX11) and exercises the five W10-A rendering
 * opcodes end-to-end:
 *
 *   1. CreateWindow on root.
 *   2. ChangeWindowAttributes to select StructureNotify | Exposure.
 *   3. MapWindow, expect MapNotify.
 *   4. CreateGC on the window.
 *   5. PutImage a 4x4 ZPixmap depth-24 image with non-zero data.
 *   6. GetImage the same 4x4 rect; verify the bytes match PutImage.
 *   7. ClearArea a 2x2 sub-rect at (0,0).
 *   8. GetImage the 2x2 rect; verify all zeros.
 *   9. PolyFillRectangle; verify no error.
 *  10. CopyArea from the window to itself; verify no error.
 *  11. FreeGC; verify no error.
 *  12. Close cleanly.
 *
 * Layout matches test_x11_opcodes2.c: parent spawns headless Ishizue,
 * forks the bridge and the X11 client, ticks isz_dispatch until the
 * client exits, then reaps the bridge. All multi-byte fields are
 * little-endian; the test always speaks LSB-first. */

#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <ishizue/isz.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define X11_BYTE_ORDER_LSB        0x6Cu
#define X11_SETUP_SUCCESS         1u

#define X11_REQ_CREATE_WINDOW     1u
#define X11_REQ_CHANGE_WIN_ATTRS  2u
#define X11_REQ_MAP_WINDOW        8u
#define X11_REQ_CREATE_GC         55u
#define X11_REQ_FREE_GC           60u
#define X11_REQ_CLEAR_AREA        61u
#define X11_REQ_COPY_AREA         62u
#define X11_REQ_POLY_FILL_RECT    69u
#define X11_REQ_PUT_IMAGE         72u
#define X11_REQ_GET_IMAGE         73u

#define X11_CW_EVENT_MASK         0x00000800u

#define X11_EVMASK_EXPOSURE           (1u << 15)
#define X11_EVMASK_STRUCTURE_NOTIFY   (1u << 17)

#define X11_EV_MAP_NOTIFY         19u

#define X11_IMAGE_FORMAT_ZPIXMAP  2u

#define TEST_ROOT_WINDOW_ID       0x00000100u
#define TEST_DISPLAY              99
#define TEST_ROOT_W               1024u
#define TEST_ROOT_H               768u

#define DEFAULT_BRIDGE_BIN        "x11bridge/x11bridge"

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
            goto fail; \
        } \
    } while (0)

static void put_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}
static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}
static uint16_t get_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32_le(const uint8_t *p) {
    return  (uint32_t)p[0]        |
           ((uint32_t)p[1] << 8)  |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int retry_connect(const char *path, int timeout_ms) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(path);
    if (plen >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, path, plen + 1u);

    int elapsed = 0;
    for (;;) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        if (errno != ECONNREFUSED && errno != ENOENT) {
            close(fd);
            return -1;
        }
        if (elapsed >= timeout_ms) {
            close(fd);
            return -1;
        }
        usleep(50000);
        elapsed += 50;
    }
}

static int read_n(int fd, void *buf, size_t n, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t off = 0;
    while (off < n) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) return -1;
        if (!(pfd.revents & POLLIN)) return -1;
        ssize_t r = recv(fd, (uint8_t *)buf + off, n - off, 0);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

static int peek_timeout(int fd, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    if (pfd.revents & POLLHUP) return -1;
    if (pfd.revents & POLLIN) return 1;
    return 0;
}

static int send_all(int fd, const void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t s = send(fd, (const uint8_t *)buf + off, n - off, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)s;
    }
    return 0;
}

static uint32_t do_setup(int fd) {
    uint8_t hdr8[8];
    if (read_n(fd, hdr8, sizeof(hdr8), 5000) < 0) return 0u;
    if (hdr8[0] != X11_SETUP_SUCCESS) return 0u;
    uint16_t add_len_units = get_u16_le(hdr8 + 6);
    size_t add_len_bytes = (size_t)add_len_units * 4u;
    size_t total_reply = 8u + add_len_bytes;
    uint8_t *reply = malloc(total_reply);
    if (reply == NULL) return 0u;
    memcpy(reply, hdr8, 8);
    if (add_len_bytes > 0) {
        if (read_n(fd, reply + 8, add_len_bytes, 5000) < 0) {
            free(reply);
            return 0u;
        }
    }
    if (total_reply < 40u) { free(reply); return 0u; }
    uint32_t rid_base = get_u32_le(reply + 12);
    free(reply);
    return rid_base;
}

/* Drain pending bytes (events and stray errors) so the next reply-
 * bearing request sees a clean socket. Returns bytes drained. */
static size_t drain_input(int fd, int timeout_ms) {
    size_t total = 0;
    for (;;) {
        int pk = peek_timeout(fd, timeout_ms);
        if (pk != 1) break;
        uint8_t buf[256];
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        total += (size_t)r;
    }
    return total;
}

/* After a void request, peek for any error byte. Errors are byte 0
 * == 0; events are byte 0 != 0. Returns 0 if no error seen, or the
 * error code if an error arrived. Events are silently drained. */
static int expect_no_error(int fd) {
    for (;;) {
        int pk = peek_timeout(fd, 200);
        if (pk < 0) return -1;
        if (pk == 0) return 0;
        uint8_t buf[32];
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) return -1;
        if (buf[0] == 0u) {
            return (int)buf[1];  /* error code */
        }
        /* else: an event. Keep draining. */
    }
}

/* X11 client side: end-to-end exercise of the five W10-A opcodes. */
static int x11_client_main(void) {
    int fd = -1;
    int ret = 1;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", TEST_DISPLAY);

    fd = retry_connect(path, 10000);
    CHECK(fd >= 0, "x11 client: connect(%s) timed out", path);

    /* 1. SetupRequest. */
    uint8_t setup_req[12];
    memset(setup_req, 0, sizeof(setup_req));
    setup_req[0] = X11_BYTE_ORDER_LSB;
    put_u16_le(setup_req + 2, 11u);
    put_u16_le(setup_req + 4, 0u);
    CHECK(send_all(fd, setup_req, sizeof(setup_req)) == 0,
          "x11 client: send setup_request: %s", strerror(errno));

    uint32_t rid_base = do_setup(fd);
    CHECK(rid_base != 0u, "x11 client: SetupSuccess parse failed");
    uint32_t root_xid = TEST_ROOT_WINDOW_ID;

    fprintf(stderr, "x11 client: setup ok, rid_base=0x%x\n",
            (unsigned)rid_base);

    /* 2. CreateWindow on root. */
    uint32_t wid = rid_base;
    {
        uint8_t cw[32];
        memset(cw, 0, sizeof(cw));
        cw[0] = X11_REQ_CREATE_WINDOW;
        cw[1] = 24u;  /* depth */
        put_u16_le(cw + 2, 8u);
        put_u32_le(cw + 4, wid);
        put_u32_le(cw + 8, root_xid);
        put_u16_le(cw + 12, 10u);
        put_u16_le(cw + 14, 10u);
        put_u16_le(cw + 16, 100u);
        put_u16_le(cw + 18, 100u);
        put_u16_le(cw + 20, 0u);
        put_u16_le(cw + 22, 1u);  /* class InputOutput */
        put_u32_le(cw + 24, 0u);  /* visual CopyFromParent */
        put_u32_le(cw + 28, 0u);  /* no value-list */
        CHECK(send_all(fd, cw, sizeof(cw)) == 0,
              "x11 client: send CreateWindow: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0, "x11 client: CreateWindow errored");
    }
    fprintf(stderr, "x11 client: CreateWindow wid=0x%x accepted\n",
            (unsigned)wid);

    /* 3. ChangeWindowAttributes: StructureNotify | Exposure. */
    {
        uint8_t cwa[16];
        memset(cwa, 0, sizeof(cwa));
        cwa[0] = X11_REQ_CHANGE_WIN_ATTRS;
        put_u16_le(cwa + 2, 4u);
        put_u32_le(cwa + 4, wid);
        put_u32_le(cwa + 8, X11_CW_EVENT_MASK);
        put_u32_le(cwa + 12, X11_EVMASK_STRUCTURE_NOTIFY |
                             X11_EVMASK_EXPOSURE);
        CHECK(send_all(fd, cwa, sizeof(cwa)) == 0,
              "x11 client: send CWA: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0, "x11 client: CWA errored");
    }

    /* 4. MapWindow. Expect a MapNotify event. */
    {
        uint8_t mw[8];
        memset(mw, 0, sizeof(mw));
        mw[0] = X11_REQ_MAP_WINDOW;
        put_u16_le(mw + 2, 2u);
        put_u32_le(mw + 4, wid);
        CHECK(send_all(fd, mw, sizeof(mw)) == 0,
              "x11 client: send MapWindow: %s", strerror(errno));
    }
    {
        uint8_t ev[32];
        CHECK(read_n(fd, ev, sizeof(ev), 5000) == 0,
              "x11 client: MapNotify timed out");
        CHECK(ev[0] == X11_EV_MAP_NOTIFY,
              "x11 client: expected MapNotify (19), got code=%u",
              (unsigned)ev[0]);
        uint32_t ev_event  = get_u32_le(ev + 4);
        uint32_t ev_window = get_u32_le(ev + 8);
        CHECK(ev_event == wid && ev_window == wid,
              "x11 client: MapNotify event/window mismatch");
        fprintf(stderr, "x11 client: MapNotify ok\n");
    }

    /* 5. CreateGC on the window. */
    uint32_t gc_xid = rid_base + 1u;
    {
        uint8_t cgc[16];
        memset(cgc, 0, sizeof(cgc));
        cgc[0] = X11_REQ_CREATE_GC;
        put_u16_le(cgc + 2, 4u);
        put_u32_le(cgc + 4, gc_xid);
        put_u32_le(cgc + 8, wid);
        put_u32_le(cgc + 12, 0u);  /* no value-list */
        CHECK(send_all(fd, cgc, sizeof(cgc)) == 0,
              "x11 client: send CreateGC: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0,
              "x11 client: CreateGC errored");
    }
    fprintf(stderr, "x11 client: CreateGC gc=0x%x ok\n", (unsigned)gc_xid);

    /* 6. PutImage a 4x4 ZPixmap depth-24 image with non-zero data.
     * 4x4 at 4 bpp = 64 bytes. Use a pattern 1..64 so a zero-fill
     * bug would be caught immediately. Total request = 24 + 64 = 88
     * bytes, length = 22 units. */
    uint8_t put_data[64];
    for (size_t i = 0; i < sizeof(put_data); i++) {
        put_data[i] = (uint8_t)(i + 1u);
    }
    {
        uint16_t width = 4u, height = 4u;
        uint8_t depth = 24u;
        size_t data_bytes = (size_t)width * height * 4u;
        size_t total = 24u + data_bytes;
        uint8_t *pi = calloc(1, total);
        CHECK(pi != NULL, "x11 client: out of memory");
        pi[0] = X11_REQ_PUT_IMAGE;
        pi[1] = X11_IMAGE_FORMAT_ZPIXMAP;
        put_u16_le(pi + 2, (uint16_t)(total / 4u));
        put_u32_le(pi + 4, wid);
        put_u32_le(pi + 8, gc_xid);
        put_u16_le(pi + 12, width);
        put_u16_le(pi + 14, height);
        put_u16_le(pi + 16, 0u);  /* dst-x */
        put_u16_le(pi + 18, 0u);  /* dst-y */
        pi[20] = 0u;  /* left-pad */
        pi[21] = depth;
        memcpy(pi + 24, put_data, data_bytes);
        CHECK(send_all(fd, pi, total) == 0,
              "x11 client: send PutImage: %s", strerror(errno));
        free(pi);
        CHECK(expect_no_error(fd) == 0,
              "x11 client: PutImage errored");
    }
    fprintf(stderr, "x11 client: PutImage 4x4 ZPixmap ok\n");

    /* 7. GetImage (0,0,4,4) ZPixmap. Verify the 64 data bytes match
     * the PutImage pattern. Reply = 32 header + 64 data = 96 bytes. */
    {
        uint8_t gi[20];
        memset(gi, 0, sizeof(gi));
        gi[0] = X11_REQ_GET_IMAGE;
        gi[1] = X11_IMAGE_FORMAT_ZPIXMAP;
        put_u16_le(gi + 2, 5u);  /* length = 5 units = 20 bytes */
        put_u32_le(gi + 4, wid);
        put_u16_le(gi + 8, 0u);   /* x */
        put_u16_le(gi + 10, 0u);  /* y */
        put_u16_le(gi + 12, 4u);  /* width */
        put_u16_le(gi + 14, 4u);  /* height */
        put_u32_le(gi + 16, 0x00FFFFFFu);  /* plane-mask: all planes */
        CHECK(send_all(fd, gi, sizeof(gi)) == 0,
              "x11 client: send GetImage: %s", strerror(errno));

        uint8_t hdr[32];
        CHECK(read_n(fd, hdr, sizeof(hdr), 5000) == 0,
              "x11 client: GetImage header timed out");
        CHECK(hdr[0] == 1u, "x11 client: GetImage byte0=%u", hdr[0]);
        uint8_t depth = hdr[1];
        CHECK(depth == 24u,
              "x11 client: GetImage depth=%u (expected 24)", depth);
        uint32_t length = get_u32_le(hdr + 4);
        CHECK(length == 16u,
              "x11 client: GetImage length=%u (expected 16)", length);

        uint8_t data[64];
        CHECK(read_n(fd, data, sizeof(data), 5000) == 0,
              "x11 client: GetImage data timed out");
        CHECK(memcmp(data, put_data, 64) == 0,
              "x11 client: GetImage data mismatch (expected PutImage bytes)");
        fprintf(stderr, "x11 client: GetImage 4x4 -> data matches PutImage ok\n");
    }

    /* 8. ClearArea (0,0,2,2) with exposures=false. The bridge should
     * zero the first 2x2 pixels in the backing image. */
    {
        uint8_t ca[16];
        memset(ca, 0, sizeof(ca));
        ca[0] = X11_REQ_CLEAR_AREA;
        ca[1] = 0u;  /* exposures = false */
        put_u16_le(ca + 2, 4u);
        put_u32_le(ca + 4, wid);
        put_u16_le(ca + 8, 0u);   /* x */
        put_u16_le(ca + 10, 0u);  /* y */
        put_u16_le(ca + 12, 2u);  /* width */
        put_u16_le(ca + 14, 2u);  /* height */
        CHECK(send_all(fd, ca, sizeof(ca)) == 0,
              "x11 client: send ClearArea: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0,
              "x11 client: ClearArea errored");
    }
    fprintf(stderr, "x11 client: ClearArea 2x2 ok\n");

    /* 9. GetImage (0,0,2,2) ZPixmap. Verify all 16 bytes are zero
     * (the cleared rect). Reply = 32 header + 16 data = 48 bytes. */
    {
        uint8_t gi[20];
        memset(gi, 0, sizeof(gi));
        gi[0] = X11_REQ_GET_IMAGE;
        gi[1] = X11_IMAGE_FORMAT_ZPIXMAP;
        put_u16_le(gi + 2, 5u);
        put_u32_le(gi + 4, wid);
        put_u16_le(gi + 8, 0u);
        put_u16_le(gi + 10, 0u);
        put_u16_le(gi + 12, 2u);
        put_u16_le(gi + 14, 2u);
        put_u32_le(gi + 16, 0x00FFFFFFu);
        CHECK(send_all(fd, gi, sizeof(gi)) == 0,
              "x11 client: send GetImage(2): %s", strerror(errno));

        uint8_t hdr[32];
        CHECK(read_n(fd, hdr, sizeof(hdr), 5000) == 0,
              "x11 client: GetImage(2) header timed out");
        CHECK(hdr[0] == 1u, "x11 client: GetImage(2) byte0=%u", hdr[0]);

        uint8_t data[16];
        CHECK(read_n(fd, data, sizeof(data), 5000) == 0,
              "x11 client: GetImage(2) data timed out");
        bool all_zero = true;
        for (size_t i = 0; i < sizeof(data); i++) {
            if (data[i] != 0u) { all_zero = false; break; }
        }
        CHECK(all_zero,
              "x11 client: GetImage(2) data not all zero after ClearArea");
        fprintf(stderr, "x11 client: GetImage 2x2 -> all zeros ok\n");
    }

    /* 10. PolyFillRectangle with 1 rect. Verify no error.
     * Total = 4 header + 8 payload + 8 rect = 20 bytes, length = 5. */
    {
        uint8_t pfr[20];
        memset(pfr, 0, sizeof(pfr));
        pfr[0] = X11_REQ_POLY_FILL_RECT;
        put_u16_le(pfr + 2, 5u);
        put_u32_le(pfr + 4, wid);       /* drawable */
        put_u32_le(pfr + 8, gc_xid);    /* gc */
        put_u16_le(pfr + 12, 0u);       /* rect x */
        put_u16_le(pfr + 14, 0u);       /* rect y */
        put_u16_le(pfr + 16, 10u);      /* rect w */
        put_u16_le(pfr + 18, 10u);      /* rect h */
        CHECK(send_all(fd, pfr, sizeof(pfr)) == 0,
              "x11 client: send PolyFillRectangle: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0,
              "x11 client: PolyFillRectangle errored");
    }
    fprintf(stderr, "x11 client: PolyFillRectangle ok\n");

    /* 11. CopyArea from the window to itself. Verify no error.
     * Total = 28 bytes, length = 7. */
    {
        uint8_t cp[28];
        memset(cp, 0, sizeof(cp));
        cp[0] = X11_REQ_COPY_AREA;
        put_u16_le(cp + 2, 7u);
        put_u32_le(cp + 4, wid);       /* src drawable */
        put_u32_le(cp + 8, wid);       /* dst drawable */
        put_u32_le(cp + 12, gc_xid);   /* gc */
        put_u16_le(cp + 16, 0u);       /* src-x */
        put_u16_le(cp + 18, 0u);       /* src-y */
        put_u16_le(cp + 20, 0u);       /* dst-x */
        put_u16_le(cp + 22, 0u);       /* dst-y */
        put_u16_le(cp + 24, 2u);       /* width */
        put_u16_le(cp + 26, 2u);       /* height */
        CHECK(send_all(fd, cp, sizeof(cp)) == 0,
              "x11 client: send CopyArea: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0,
              "x11 client: CopyArea errored");
    }
    fprintf(stderr, "x11 client: CopyArea ok\n");

    /* 12. FreeGC. Verify no error. */
    {
        uint8_t fg[8];
        memset(fg, 0, sizeof(fg));
        fg[0] = X11_REQ_FREE_GC;
        put_u16_le(fg + 2, 2u);
        put_u32_le(fg + 4, gc_xid);
        CHECK(send_all(fd, fg, sizeof(fg)) == 0,
              "x11 client: send FreeGC: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0,
              "x11 client: FreeGC errored");
    }
    fprintf(stderr, "x11 client: FreeGC ok\n");

    /* 13. Clean close. */
    (void)drain_input(fd, 100);
    close(fd);
    return 0;

fail:
    if (fd >= 0) close(fd);
    return ret;
}

int main(void) {
    int              ret        = 1;
    isz_server      *srv        = NULL;
    int              listen_fd  = -1;
    pid_t            bridge_pid = -1;
    pid_t            client_pid = -1;
    char             isz_sock[96];
    char             x11_sock[40];

    signal(SIGPIPE, SIG_IGN);

    const char *bridge_bin = getenv("ISZ_X11BRIDGE_BIN");
    if (bridge_bin == NULL || bridge_bin[0] == '\0') {
        bridge_bin = DEFAULT_BRIDGE_BIN;
    }

    isz_headless_config cfg = {
        .width = TEST_ROOT_W, .height = TEST_ROOT_H, .refresh_rate = 60000,
    };
    srv = isz_init(ISZ_BACKEND_HEADLESS, &cfg);
    CHECK(srv != NULL, "isz_init returned NULL");

    {
        struct stat stb;
        CHECK(stat(bridge_bin, &stb) == 0,
              "bridge binary not found at %s: %s", bridge_bin, strerror(errno));
        int rc = isz_allowlist_add_binary(srv, bridge_bin);
        CHECK(rc == ISZ_OK,
              "isz_allowlist_add_binary(%s) rc=%d", bridge_bin, rc);
    }

    snprintf(isz_sock, sizeof(isz_sock), "/tmp/.ishizue-x11render-%d",
             (int)getpid());
    (void)unlink(isz_sock);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listen_fd >= 0, "socket: %s", strerror(errno));
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, isz_sock, strlen(isz_sock) + 1u);
    CHECK(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0,
          "bind: %s", strerror(errno));
    CHECK(listen(listen_fd, 4) == 0, "listen: %s", strerror(errno));
    {
        int rc = isz_listen(srv, listen_fd);
        CHECK(rc == ISZ_OK, "isz_listen rc=%d", rc);
    }

    bridge_pid = fork();
    CHECK(bridge_pid >= 0, "fork bridge: %s", strerror(errno));
    if (bridge_pid == 0) {
        setenv("ISZ_SOCKET", isz_sock, 1);
        char disp[8];
        snprintf(disp, sizeof(disp), "%d", TEST_DISPLAY);
        setenv("ISZ_X11_DISPLAY", disp, 1);
        execl(bridge_bin, "x11bridge", (char *)NULL);
        _exit(127);
    }

    client_pid = fork();
    CHECK(client_pid >= 0, "fork client: %s", strerror(errno));
    if (client_pid == 0) {
        int r = x11_client_main();
        _exit(r);
    }

    fprintf(stderr, "[test] bridge_pid=%d client_pid=%d isz_sock=%s\n",
            (int)bridge_pid, (int)client_pid, isz_sock);

    int client_status = -1;
    for (;;) {
        if (client_pid > 0) {
            pid_t w = waitpid(client_pid, &client_status, WNOHANG);
            if (w == client_pid) {
                client_pid = -1;
            } else if (w < 0 && errno != EINTR && errno != ECHILD) {
                fprintf(stderr, "waitpid(client): %s\n", strerror(errno));
                break;
            }
        }
        if (client_pid < 0) break;

        isz_dispatch(srv);
        usleep(1000);
    }

    if (client_pid < 0) {
        if (WIFEXITED(client_status) && WEXITSTATUS(client_status) == 0) {
            ret = 0;
        } else {
            fprintf(stderr, "client child exited status=%d\n", client_status);
            ret = 1;
        }
    } else {
        fprintf(stderr, "client child did not exit\n");
        ret = 1;
    }

    if (bridge_pid > 0) {
        kill(bridge_pid, SIGTERM);
        for (;;) {
            pid_t w = waitpid(bridge_pid, NULL, 0);
            if (w == bridge_pid || (w < 0 && errno == ECHILD)) break;
            if (w < 0 && errno == EINTR) continue;
            break;
        }
    }

    if (ret == 0) {
        printf("test_x11_render: PASS\n");
    }

    isz_destroy(srv);
    (void)unlink(isz_sock);
    snprintf(x11_sock, sizeof(x11_sock), "/tmp/.X11-unix/X%d", TEST_DISPLAY);
    (void)unlink(x11_sock);
    return ret;

fail:
    if (bridge_pid > 0) {
        kill(bridge_pid, SIGTERM);
        waitpid(bridge_pid, NULL, 0);
    }
    if (client_pid > 0) {
        kill(client_pid, SIGTERM);
        waitpid(client_pid, NULL, 0);
    }
    if (srv) isz_destroy(srv);
    if (listen_fd >= 0) close(listen_fd);
    if (isz_sock[0]) (void)unlink(isz_sock);
    snprintf(x11_sock, sizeof(x11_sock), "/tmp/.X11-unix/X%d", TEST_DISPLAY);
    (void)unlink(x11_sock);
    return 1;
}
