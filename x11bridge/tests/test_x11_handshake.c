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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* test_x11_handshake.c: W7-C integration test.
 *
 * Spawns the x11bridge binary against a headless Ishizue server
 * running in the test process, then connects to the bridge's UDS as
 * a raw X11 client (no libX11) and exercises the first concrete X11
 * milestone: SetupSuccess, CreateWindow, GetGeometry.
 *
 * Layout:
 *   1. Parent: isz_init(headless) + allowlist + isz_listen + fork.
 *   2. Child A: exec x11bridge binary (env ISZ_SOCKET, ISZ_X11_DISPLAY=99).
 *      The bridge connects to the parent's Ishizue UDS as a real
 *      client, completes the §6.2 handshake, opens /tmp/.X11-unix/X99,
 *      and enters its epoll loop.
 *   3. Child B: act as the X11 client. Wait for /tmp/.X11-unix/X99 to
 *      appear, connect, send SetupRequest, read SetupSuccess, validate,
 *      send CreateWindow (100x100 at 0,0 on root), read back any error
 *      (expect none), send GetGeometry for the new window, read the
 *      reply, validate depth/geometry, close cleanly, exit 0.
 *   4. Parent: drive isz_dispatch in a loop until both children exit.
 *      Verify child B returned 0. Tear down server, unlink sockets.
 *
 * The X11 client side speaks raw bytes per the W6-B reference doc.
 * All multi-byte fields are written in the byte order the client
 * picked in its SetupRequest (little-endian here). */

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

/* X11 wire constants the test exercises. Mirrored from
 * x11bridge/x11_proto.h so the test does not pull internal bridge
 * headers (it links only against the public libishizue API). */
#define X11_BYTE_ORDER_LSB        0x6Cu
#define X11_SETUP_SUCCESS         1u
#define X11_SETUP_FAILED          0u

#define X11_REQ_CREATE_WINDOW     1u
#define X11_REQ_GET_GEOMETRY      14u

#define TEST_ROOT_WINDOW_ID       0x00000100u
#define TEST_ROOT_VISUAL_ID       0x00000021u
#define TEST_DEFAULT_COLORMAP     0x00000022u
#define TEST_BLACK_PIXEL          0x00000000u
#define TEST_WHITE_PIXEL          0x00FFFFFFu

#define TEST_DISPLAY              99
#define TEST_ROOT_W               1024u
#define TEST_ROOT_H               768u

/* Default path to the bridge binary relative to the repo root. The
 * parent Makefile's `test` target runs this test from the repo root
 * so the relative path resolves. Override with $ISZ_X11BRIDGE_BIN. */
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

/* Little-endian encoders. The test always speaks LSB-first. */
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

/* Retry connect for up to ~10 seconds so the bridge has time to
 * open its X11 listening socket. Returns fd on success, -1 on
 * timeout / hard error. */
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
        usleep(50000);  /* 50 ms */
        elapsed += 50;
    }
}

/* Read exactly n bytes with a timeout. Returns 0 on success, -1 on
 * timeout or hard error. */
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

/* Peek with a short timeout. Returns 1 if at least one byte is
 * available, 0 on timeout, -1 on hard error / peer-closed. Useful
 * for asserting "no error/reply was sent" after a void request. */
static int peek_timeout(int fd, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    if (pfd.revents & POLLHUP) return -1;
    if (pfd.revents & POLLIN) return 1;
    return 0;
}

/* Send all bytes. Returns 0 on success, -1 on hard error. */
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

/* X11 client side: SetupRequest -> SetupSuccess -> CreateWindow ->
 * GetGeometry. Returns 0 on success, non-zero on failure. */
static int x11_client_main(void) {
    int fd = -1;
    int ret = 1;
    uint8_t *reply = NULL;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", TEST_DISPLAY);

    fd = retry_connect(path, 10000);
    CHECK(fd >= 0, "x11 client: connect(%s) timed out", path);

    /* 1. SetupRequest: 12 bytes, no auth, LSB-first, proto 11.0. */
    uint8_t setup_req[12];
    memset(setup_req, 0, sizeof(setup_req));
    setup_req[0] = X11_BYTE_ORDER_LSB;
    put_u16_le(setup_req + 2, 11u);  /* proto major */
    put_u16_le(setup_req + 4, 0u);   /* proto minor */
    /* auth-name-len = 0, auth-data-len = 0 (bytes 6..9, already zero) */
    CHECK(send_all(fd, setup_req, sizeof(setup_req)) == 0,
          "x11 client: send setup_request: %s", strerror(errno));

    /* 2. Read SetupSuccess. First 8 bytes give us the length of the
     * additional data; we then read (8 + length*4) total bytes. */
    uint8_t hdr8[8];
    CHECK(read_n(fd, hdr8, sizeof(hdr8), 5000) == 0,
          "x11 client: read setup_success header timed out");
    CHECK(hdr8[0] == X11_SETUP_SUCCESS,
          "x11 client: setup status=%u (expected %u)", hdr8[0], X11_SETUP_SUCCESS);
    uint16_t add_len_units = get_u16_le(hdr8 + 6);
    size_t add_len_bytes = (size_t)add_len_units * 4u;
    size_t total_reply = 8u + add_len_bytes;

    reply = malloc(total_reply);
    if (reply == NULL) {
        fprintf(stderr, "FAIL: out of memory for reply\n");
        close(fd);
        return 1;
    }
    memcpy(reply, hdr8, 8);
    if (add_len_bytes > 0) {
        CHECK(read_n(fd, reply + 8, add_len_bytes, 5000) == 0,
              "x11 client: read setup_success body timed out");
    }

    /* Validate the fixed header fields (bytes 0..39 of the reply). */
    CHECK(total_reply >= 40, "x11 client: setup_success too short: %zu",
          total_reply);
    uint16_t proto_major = get_u16_le(reply + 2);
    uint16_t proto_minor = get_u16_le(reply + 4);
    CHECK(proto_major == 11u && proto_minor == 0u,
          "x11 client: proto=%u.%u (expected 11.0)",
          (unsigned)proto_major, (unsigned)proto_minor);
    uint32_t rid_base = get_u32_le(reply + 12);
    uint32_t rid_mask = get_u32_le(reply + 16);
    CHECK(rid_base != 0u && rid_mask != 0u,
          "x11 client: rid_base=0x%x rid_mask=0x%x (both must be non-zero)",
          (unsigned)rid_base, (unsigned)rid_mask);
    uint16_t vendor_len = get_u16_le(reply + 24);
    uint16_t max_req_len = get_u16_le(reply + 26);
    CHECK(max_req_len >= 4096u,
          "x11 client: max_request_length=%u (expected >= 4096)",
          (unsigned)max_req_len);
    uint8_t roots_len = reply[28];
    uint8_t formats_len = reply[29];
    uint8_t min_keycode = reply[34];
    uint8_t max_keycode = reply[35];
    CHECK(roots_len >= 1u, "x11 client: roots_len=%u (expected >= 1)", roots_len);
    CHECK(formats_len >= 1u, "x11 client: formats_len=%u (expected >= 1)", formats_len);
    CHECK(min_keycode >= 8u && min_keycode < max_keycode,
          "x11 client: keycodes %u..%u out of range",
          (unsigned)min_keycode, (unsigned)max_keycode);

    /* Walk the variable-length body: vendor (padded), then formats,
     * then roots. */
    size_t off = 40;
    size_t vendor_padded = (size_t)((vendor_len + 3u) & ~3u);

    /* Vendor string. */
    CHECK(off + vendor_padded <= total_reply,
          "x11 client: vendor overruns reply");
    if (vendor_len > 0) {
        /* The bridge advertises "Ishizue". Tolerate any non-empty
         * vendor; the bridge could legitimately pick a different
         * string later. */
        char vendor_buf[256];
        size_t copy_n = vendor_len < sizeof(vendor_buf) - 1u
                          ? vendor_len : sizeof(vendor_buf) - 1u;
        memcpy(vendor_buf, reply + off, copy_n);
        vendor_buf[copy_n] = '\0';
        fprintf(stderr, "x11 client: vendor=\"%s\"\n", vendor_buf);
    }
    off += vendor_padded;

    /* Pixmap formats: 8 bytes each. */
    CHECK(off + (size_t)formats_len * 8u <= total_reply,
          "x11 client: formats overruns reply");
    int found_depth24 = 0;
    for (uint8_t i = 0; i < formats_len; i++) {
        uint8_t depth = reply[off + (size_t)i * 8u];
        uint8_t bpp   = reply[off + (size_t)i * 8u + 1u];
        if (depth == 24u && bpp == 32u) found_depth24 = 1;
    }
    CHECK(found_depth24, "x11 client: no depth-24/bpp-32 pixmap format");
    off += (size_t)formats_len * 8u;

    /* Root screen: 40-byte fixed header, then variable depths. */
    CHECK(off + 40u <= total_reply, "x11 client: root screen overruns reply");
    uint32_t root_xid     = get_u32_le(reply + off + 0);
    uint32_t def_colormap = get_u32_le(reply + off + 4);
    uint32_t white_pixel  = get_u32_le(reply + off + 8);
    uint32_t black_pixel  = get_u32_le(reply + off + 12);
    uint16_t root_w       = get_u16_le(reply + off + 20);
    uint16_t root_h       = get_u16_le(reply + off + 22);
    uint32_t root_visual  = get_u32_le(reply + off + 32);
    uint8_t  root_depth   = reply[off + 38];
    uint8_t  depths_len   = reply[off + 39];
    CHECK(root_xid == TEST_ROOT_WINDOW_ID,
          "x11 client: root_xid=0x%x (expected 0x%x)",
          (unsigned)root_xid, (unsigned)TEST_ROOT_WINDOW_ID);
    CHECK(def_colormap == TEST_DEFAULT_COLORMAP,
          "x11 client: default-colormap=0x%x (expected 0x%x)",
          (unsigned)def_colormap, (unsigned)TEST_DEFAULT_COLORMAP);
    CHECK(white_pixel == TEST_WHITE_PIXEL,
          "x11 client: white_pixel=0x%x (expected 0x%x)",
          (unsigned)white_pixel, (unsigned)TEST_WHITE_PIXEL);
    CHECK(black_pixel == TEST_BLACK_PIXEL,
          "x11 client: black_pixel=0x%x (expected 0x%x)",
          (unsigned)black_pixel, (unsigned)TEST_BLACK_PIXEL);
    CHECK(root_w == TEST_ROOT_W,
          "x11 client: root_w=%u (expected %u)",
          (unsigned)root_w, (unsigned)TEST_ROOT_W);
    CHECK(root_h == TEST_ROOT_H,
          "x11 client: root_h=%u (expected %u)",
          (unsigned)root_h, (unsigned)TEST_ROOT_H);
    CHECK(root_visual == TEST_ROOT_VISUAL_ID,
          "x11 client: root_visual=0x%x (expected 0x%x)",
          (unsigned)root_visual, (unsigned)TEST_ROOT_VISUAL_ID);
    CHECK(root_depth == 24u,
          "x11 client: root_depth=%u (expected 24)", (unsigned)root_depth);
    /* The bridge advertises 1 depth (24) with 1 visual. */
    CHECK(depths_len >= 1u,
          "x11 client: depths_len=%u (expected >= 1)", (unsigned)depths_len);
    free(reply);
    reply = NULL;

    fprintf(stderr,
            "x11 client: SetupSuccess validated (rid_base=0x%x rid_mask=0x%x "
            "root=0x%x geom=%ux%u visual=0x%x depth=%u)\n",
            (unsigned)rid_base, (unsigned)rid_mask,
            (unsigned)root_xid, (unsigned)root_w, (unsigned)root_h,
            (unsigned)root_visual, (unsigned)root_depth);

    /* 3. CreateWindow: 100x100 at (0,0), parent=root, depth 24,
     * class InputOutput, visual CopyFromParent (0), no value-list.
     *
     * The client picks its own XID via (rid_base | 0 & rid_mask) =
     * rid_base for the first window. */
    uint32_t wid = rid_base;
    uint8_t cw[32];
    memset(cw, 0, sizeof(cw));
    cw[0] = X11_REQ_CREATE_WINDOW;
    cw[1] = 24u;  /* depth */
    put_u16_le(cw + 2, 8u);             /* length = 8 4-byte units = 32 bytes */
    put_u32_le(cw + 4, wid);            /* window id */
    put_u32_le(cw + 8, root_xid);       /* parent = root */
    put_u16_le(cw + 12, 0u);            /* x */
    put_u16_le(cw + 14, 0u);            /* y */
    put_u16_le(cw + 16, 100u);          /* width */
    put_u16_le(cw + 18, 100u);          /* height */
    put_u16_le(cw + 20, 0u);            /* border-width */
    put_u16_le(cw + 22, 1u);            /* class = InputOutput */
    put_u32_le(cw + 24, 0u);            /* visual = CopyFromParent */
    put_u32_le(cw + 28, 0u);            /* value-mask = 0 (no attributes) */
    CHECK(send_all(fd, cw, sizeof(cw)) == 0,
          "x11 client: send CreateWindow: %s", strerror(errno));

    /* CreateWindow is void: no reply, errors only. Give the bridge
     * 500 ms to react; if any bytes arrive, they must be an error. */
    {
        int pk = peek_timeout(fd, 500);
        CHECK(pk >= 0, "x11 client: poll after CreateWindow failed");
        if (pk == 1) {
            uint8_t errbuf[32];
            ssize_t r = recv(fd, errbuf, sizeof(errbuf), 0);
            CHECK(r > 0, "x11 client: recv after CreateWindow failed");
            CHECK(errbuf[0] == 0u,
                  "x11 client: CreateWindow produced a non-error reply (byte0=%u)",
                  errbuf[0]);
            fprintf(stderr,
                    "x11 client: CreateWindow produced error code=%u major=%u\n",
                    (unsigned)errbuf[1], (unsigned)errbuf[9]);
            CHECK(0, "x11 client: CreateWindow errored");
        }
    }
    fprintf(stderr, "x11 client: CreateWindow wid=0x%x accepted\n",
            (unsigned)wid);

    /* 4. GetGeometry for the just-created window. */
    uint8_t gg[8];
    memset(gg, 0, sizeof(gg));
    gg[0] = X11_REQ_GET_GEOMETRY;
    put_u16_le(gg + 2, 2u);             /* length = 2 */
    put_u32_le(gg + 4, wid);
    CHECK(send_all(fd, gg, sizeof(gg)) == 0,
          "x11 client: send GetGeometry: %s", strerror(errno));

    /* Read the 32-byte reply. */
    uint8_t gg_reply[32];
    CHECK(read_n(fd, gg_reply, sizeof(gg_reply), 5000) == 0,
          "x11 client: read GetGeometry reply timed out");
    CHECK(gg_reply[0] == 1u,
          "x11 client: GetGeometry byte0=%u (expected 1=reply)",
          gg_reply[0]);
    uint8_t  gg_depth  = gg_reply[1];
    uint16_t gg_seq    = get_u16_le(gg_reply + 2);
    uint32_t gg_root   = get_u32_le(gg_reply + 8);
    int16_t  gg_x      = (int16_t)get_u16_le(gg_reply + 12);
    int16_t  gg_y      = (int16_t)get_u16_le(gg_reply + 14);
    uint16_t gg_w      = get_u16_le(gg_reply + 16);
    uint16_t gg_h      = get_u16_le(gg_reply + 18);
    uint16_t gg_border = get_u16_le(gg_reply + 20);
    (void)gg_seq;
    CHECK(gg_depth == 24u,
          "x11 client: GetGeometry depth=%u (expected 24)",
          (unsigned)gg_depth);
    CHECK(gg_root == root_xid,
          "x11 client: GetGeometry root=0x%x (expected 0x%x)",
          (unsigned)gg_root, (unsigned)root_xid);
    CHECK(gg_x == 0 && gg_y == 0,
          "x11 client: GetGeometry pos=(%d,%d) (expected 0,0)",
          (int)gg_x, (int)gg_y);
    CHECK(gg_w == 100u && gg_h == 100u,
          "x11 client: GetGeometry size=%ux%u (expected 100x100)",
          (unsigned)gg_w, (unsigned)gg_h);
    CHECK(gg_border == 0u,
          "x11 client: GetGeometry border=%u (expected 0)",
          (unsigned)gg_border);
    fprintf(stderr,
            "x11 client: GetGeometry reply validated (depth=%u root=0x%x "
            "geom=(%d,%d,%ux%u) border=%u)\n",
            (unsigned)gg_depth, (unsigned)gg_root,
            (int)gg_x, (int)gg_y, (unsigned)gg_w, (unsigned)gg_h,
            (unsigned)gg_border);

    /* 5. Clean close. */
    close(fd);
    return 0;

fail:
    if (fd >= 0) close(fd);
    free(reply);
    return ret;
}

int main(void) {
    int              ret       = 1;
    isz_server      *srv       = NULL;
    int              listen_fd = -1;
    pid_t            bridge_pid = -1;
    pid_t            client_pid = -1;
    char             isz_sock[96];
    char             x11_sock[40];

    signal(SIGPIPE, SIG_IGN);

    const char *bridge_bin = getenv("ISZ_X11BRIDGE_BIN");
    if (bridge_bin == NULL || bridge_bin[0] == '\0') {
        bridge_bin = DEFAULT_BRIDGE_BIN;
    }

    /* 1. Headless Ishizue server. */
    isz_headless_config cfg = {
        .width = TEST_ROOT_W, .height = TEST_ROOT_H, .refresh_rate = 60000,
    };
    srv = isz_init(ISZ_BACKEND_HEADLESS, &cfg);
    CHECK(srv != NULL, "isz_init returned NULL");

    /* 2. Allowlist the bridge binary so the §6.3 SO_PEERCRED check
     *    passes when the bridge connects. The allowlist resolves the
     *    path to (st_dev, st_ino) at call time, so the freshly-built
     *    binary is picked up. */
    {
        struct stat stb;
        CHECK(stat(bridge_bin, &stb) == 0,
              "bridge binary not found at %s: %s", bridge_bin, strerror(errno));
        int rc = isz_allowlist_add_binary(srv, bridge_bin);
        CHECK(rc == ISZ_OK,
              "isz_allowlist_add_binary(%s) rc=%d", bridge_bin, rc);
    }

    /* 3. Create the Ishizue UDS at a unique path. */
    snprintf(isz_sock, sizeof(isz_sock), "/tmp/.ishizue-x11test-%d",
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

    /* 4. Fork the bridge subprocess. */
    bridge_pid = fork();
    CHECK(bridge_pid >= 0, "fork bridge: %s", strerror(errno));
    if (bridge_pid == 0) {
        /* Child: exec the bridge binary with env pointing at our
         * Ishizue UDS and display 99. */
        setenv("ISZ_SOCKET", isz_sock, 1);
        char disp[8];
        snprintf(disp, sizeof(disp), "%d", TEST_DISPLAY);
        setenv("ISZ_X11_DISPLAY", disp, 1);
        /* Keep stderr attached so the bridge's log lines are visible
         * alongside the test's output. */
        execl(bridge_bin, "x11bridge", (char *)NULL);
        _exit(127);
    }

    /* 5. Fork the X11 client child. */
    client_pid = fork();
    CHECK(client_pid >= 0, "fork client: %s", strerror(errno));
    if (client_pid == 0) {
        int r = x11_client_main();
        _exit(r);
    }

    fprintf(stderr, "[test] bridge_pid=%d client_pid=%d isz_sock=%s\n",
            (int)bridge_pid, (int)client_pid, isz_sock);

    /* 6. Parent: drive isz_dispatch until the client child exits. The
     *    bridge child doesn't self-terminate; we kill it in step 7. */
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

        /* Tick the server's epoll loop. Non-blocking; returns
         * immediately when there is nothing to do. */
        isz_dispatch(srv);
        usleep(1000);
    }

    /* 7. Validate child exit statuses. The client child must have
     *    exited 0. The bridge child may still be running (it does
     *    not self-terminate); if so, kill it. */
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

    /* If the bridge is still running, signal it. */
    if (bridge_pid > 0) {
        kill(bridge_pid, SIGTERM);
        /* Reap. */
        for (;;) {
            pid_t w = waitpid(bridge_pid, NULL, 0);
            if (w == bridge_pid || (w < 0 && errno == ECHILD)) break;
            if (w < 0 && errno == EINTR) continue;
            break;
        }
    }

    if (ret == 0) {
        printf("test_x11_handshake: PASS\n");
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
