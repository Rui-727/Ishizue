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

/* test_x11_opcodes2.c: W9-B integration test.
 *
 * Spawns the x11bridge binary against a headless Ishizue server running
 * in the test process, then connects to the bridge's UDS as a raw X11
 * client (no libX11) and exercises the ten W9-B opcodes end-to-end:
 *
 *   1. CreateWindow on root.
 *   2. ChangeWindowAttributes to select StructureNotify | Exposure |
 *      PropertyChange.
 *   3. GetWindowAttributes on the window: verify map-state=Unmapped (0).
 *   4. MapWindow, expect MapNotify.
 *   5. GetWindowAttributes: verify map-state=IsViewable (2).
 *   6. QueryTree on root: verify 1 child == our window.
 *   7. QueryTree on the window: verify 0 children.
 *   8. GetAtomName(PRIMARY=1): name == "PRIMARY".
 *   9. GetAtomName(STRING=31): name == "STRING".
 *  10. ChangeProperty WM_NAME = "test".
 *  11. DeleteProperty WM_NAME.
 *  12. GetProperty WM_NAME: type == 0 (property does not exist).
 *  13. SetSelectionOwner(PRIMARY, window, time=1).
 *  14. GetSelectionOwner(PRIMARY): owner == window.
 *  15. SetSelectionOwner(PRIMARY, 0, time=2) to clear.
 *  16. GetSelectionOwner(PRIMARY): owner == 0.
 *  17. QueryPointer on root: verify reply structure.
 *  18. SetInputFocus(revert-to=Ancestor, focus=window).
 *  19. CreateGC on the window.
 *  20. PutImage a 4x4 ZPixmap depth-24 image on the window.
 *  21. Close cleanly.
 *
 * Layout matches test_x11_opcodes.c: parent spawns headless Ishizue,
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
#define X11_REQ_GET_WIN_ATTRS     3u
#define X11_REQ_MAP_WINDOW        8u
#define X11_REQ_QUERY_TREE        15u
#define X11_REQ_GET_ATOM_NAME     17u
#define X11_REQ_CHANGE_PROPERTY   18u
#define X11_REQ_DELETE_PROPERTY   19u
#define X11_REQ_GET_PROPERTY      20u
#define X11_REQ_SET_SEL_OWNER     22u
#define X11_REQ_GET_SEL_OWNER     23u
#define X11_REQ_QUERY_POINTER     38u
#define X11_REQ_SET_INPUT_FOCUS   42u
#define X11_REQ_CREATE_GC         55u
#define X11_REQ_PUT_IMAGE         72u

#define X11_CW_EVENT_MASK         0x00000800u

#define X11_EVMASK_EXPOSURE           (1u << 15)
#define X11_EVMASK_STRUCTURE_NOTIFY   (1u << 17)
#define X11_EVMASK_PROPERTY_CHANGE    (1u << 22)

#define X11_EV_MAP_NOTIFY         19u

#define X11_PROP_MODE_REPLACE     0u

#define X11_ATOM_PRIMARY          1u
#define X11_ATOM_STRING           31u
#define X11_ATOM_WM_NAME          39u

#define X11_MAP_STATE_UNMAPPED    0u
#define X11_MAP_STATE_VIEWABLE    2u

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

/* X11 client side: end-to-end exercise of the ten W9-B opcodes. */
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

    /* 3. ChangeWindowAttributes: StructureNotify | Exposure |
     * PropertyChange. PropertyChange is needed so DeleteProperty's
     * PropertyNotify can be observed by the bridge's drain. */
    {
        uint8_t cwa[16];
        memset(cwa, 0, sizeof(cwa));
        cwa[0] = X11_REQ_CHANGE_WIN_ATTRS;
        put_u16_le(cwa + 2, 4u);
        put_u32_le(cwa + 4, wid);
        put_u32_le(cwa + 8, X11_CW_EVENT_MASK);
        put_u32_le(cwa + 12, X11_EVMASK_STRUCTURE_NOTIFY |
                             X11_EVMASK_EXPOSURE |
                             X11_EVMASK_PROPERTY_CHANGE);
        CHECK(send_all(fd, cwa, sizeof(cwa)) == 0,
              "x11 client: send CWA: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0, "x11 client: CWA errored");
    }

    /* 4. GetWindowAttributes on the unmapped window. Expect
     * map-state = Unmapped (0). */
    {
        uint8_t gwa[8];
        memset(gwa, 0, sizeof(gwa));
        gwa[0] = X11_REQ_GET_WIN_ATTRS;
        put_u16_le(gwa + 2, 2u);
        put_u32_le(gwa + 4, wid);
        CHECK(send_all(fd, gwa, sizeof(gwa)) == 0,
              "x11 client: send GetWindowAttributes: %s", strerror(errno));

        uint8_t reply[44];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: GetWindowAttributes reply timed out");
        CHECK(reply[0] == 1u, "x11 client: GWA byte0=%u", reply[0]);
        uint8_t map_state = reply[26];
        CHECK(map_state == X11_MAP_STATE_UNMAPPED,
              "x11 client: GWA map-state=%u (expected 0 Unmapped)",
              (unsigned)map_state);
        uint16_t cls = get_u16_le(reply + 12);
        CHECK(cls == 1u, "x11 client: GWA class=%u (expected 1 InputOutput)",
              (unsigned)cls);
        fprintf(stderr, "x11 client: GWA map-state=0 (Unmapped) ok\n");
    }

    /* 5. MapWindow. Expect a MapNotify event. */
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

    /* 6. GetWindowAttributes on the mapped window. Expect
     * map-state = IsViewable (2). */
    {
        uint8_t gwa[8];
        memset(gwa, 0, sizeof(gwa));
        gwa[0] = X11_REQ_GET_WIN_ATTRS;
        put_u16_le(gwa + 2, 2u);
        put_u32_le(gwa + 4, wid);
        CHECK(send_all(fd, gwa, sizeof(gwa)) == 0,
              "x11 client: send GetWindowAttributes(2): %s", strerror(errno));

        uint8_t reply[44];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: GWA(2) reply timed out");
        CHECK(reply[0] == 1u, "x11 client: GWA(2) byte0=%u", reply[0]);
        uint8_t map_state = reply[26];
        CHECK(map_state == X11_MAP_STATE_VIEWABLE,
              "x11 client: GWA(2) map-state=%u (expected 2 IsViewable)",
              (unsigned)map_state);
        fprintf(stderr, "x11 client: GWA map-state=2 (IsViewable) ok\n");
    }

    /* 7. QueryTree on root. Expect 1 child == wid. */
    {
        uint8_t qt[8];
        memset(qt, 0, sizeof(qt));
        qt[0] = X11_REQ_QUERY_TREE;
        put_u16_le(qt + 2, 2u);
        put_u32_le(qt + 4, root_xid);
        CHECK(send_all(fd, qt, sizeof(qt)) == 0,
              "x11 client: send QueryTree(root): %s", strerror(errno));

        uint8_t hdr[32];
        CHECK(read_n(fd, hdr, sizeof(hdr), 5000) == 0,
              "x11 client: QueryTree(root) header timed out");
        CHECK(hdr[0] == 1u, "x11 client: QueryTree byte0=%u", hdr[0]);
        uint32_t root = get_u32_le(hdr + 8);
        uint32_t parent = get_u32_le(hdr + 12);
        uint16_t n = get_u16_le(hdr + 16);
        CHECK(root == root_xid, "x11 client: QueryTree root=0x%x expected 0x%x",
              (unsigned)root, (unsigned)root_xid);
        CHECK(parent == 0u, "x11 client: QueryTree parent=0x%x expected 0",
              (unsigned)parent);
        CHECK(n == 1u, "x11 client: QueryTree(root) children=%u (expected 1)",
              (unsigned)n);
        uint8_t child_buf[4];
        CHECK(read_n(fd, child_buf, sizeof(child_buf), 5000) == 0,
              "x11 client: QueryTree(root) child timed out");
        uint32_t child = get_u32_le(child_buf);
        CHECK(child == wid,
              "x11 client: QueryTree(root) child=0x%x expected 0x%x",
              (unsigned)child, (unsigned)wid);
        fprintf(stderr, "x11 client: QueryTree(root) children=[0x%x] ok\n",
                (unsigned)child);
    }

    /* 8. QueryTree on our window. Expect 0 children. */
    {
        uint8_t qt[8];
        memset(qt, 0, sizeof(qt));
        qt[0] = X11_REQ_QUERY_TREE;
        put_u16_le(qt + 2, 2u);
        put_u32_le(qt + 4, wid);
        CHECK(send_all(fd, qt, sizeof(qt)) == 0,
              "x11 client: send QueryTree(wid): %s", strerror(errno));

        uint8_t hdr[32];
        CHECK(read_n(fd, hdr, sizeof(hdr), 5000) == 0,
              "x11 client: QueryTree(wid) header timed out");
        CHECK(hdr[0] == 1u, "x11 client: QueryTree(wid) byte0=%u", hdr[0]);
        uint32_t parent = get_u32_le(hdr + 12);
        uint16_t n = get_u16_le(hdr + 16);
        CHECK(parent == root_xid,
              "x11 client: QueryTree(wid) parent=0x%x expected 0x%x",
              (unsigned)parent, (unsigned)root_xid);
        CHECK(n == 0u, "x11 client: QueryTree(wid) children=%u (expected 0)",
              (unsigned)n);
        fprintf(stderr, "x11 client: QueryTree(wid) children=[] ok\n");
    }

    /* 9. GetAtomName(PRIMARY=1). Expect "PRIMARY". */
    {
        uint8_t gan[8];
        memset(gan, 0, sizeof(gan));
        gan[0] = X11_REQ_GET_ATOM_NAME;
        put_u16_le(gan + 2, 2u);
        put_u32_le(gan + 4, X11_ATOM_PRIMARY);
        CHECK(send_all(fd, gan, sizeof(gan)) == 0,
              "x11 client: send GetAtomName(PRIMARY): %s", strerror(errno));

        uint8_t hdr[32];
        CHECK(read_n(fd, hdr, sizeof(hdr), 5000) == 0,
              "x11 client: GetAtomName(PRIMARY) header timed out");
        CHECK(hdr[0] == 1u, "x11 client: GetAtomName byte0=%u", hdr[0]);
        uint16_t name_len = get_u16_le(hdr + 8);
        CHECK(name_len == 7u,
              "x11 client: GetAtomName(PRIMARY) name_len=%u (expected 7)",
              (unsigned)name_len);
        size_t padded = (size_t)((name_len + 3u) & ~3u);
        uint8_t namebuf[8];
        CHECK(read_n(fd, namebuf, padded, 5000) == 0,
              "x11 client: GetAtomName(PRIMARY) name timed out");
        CHECK(memcmp(namebuf, "PRIMARY", 7u) == 0,
              "x11 client: GetAtomName(PRIMARY) name mismatch");
        fprintf(stderr, "x11 client: GetAtomName(1) = \"PRIMARY\" ok\n");
    }

    /* 10. GetAtomName(STRING=31). Expect "STRING". */
    {
        uint8_t gan[8];
        memset(gan, 0, sizeof(gan));
        gan[0] = X11_REQ_GET_ATOM_NAME;
        put_u16_le(gan + 2, 2u);
        put_u32_le(gan + 4, X11_ATOM_STRING);
        CHECK(send_all(fd, gan, sizeof(gan)) == 0,
              "x11 client: send GetAtomName(STRING): %s", strerror(errno));

        uint8_t hdr[32];
        CHECK(read_n(fd, hdr, sizeof(hdr), 5000) == 0,
              "x11 client: GetAtomName(STRING) header timed out");
        CHECK(hdr[0] == 1u, "x11 client: GetAtomName(STRING) byte0=%u", hdr[0]);
        uint16_t name_len = get_u16_le(hdr + 8);
        CHECK(name_len == 6u,
              "x11 client: GetAtomName(STRING) name_len=%u (expected 6)",
              (unsigned)name_len);
        size_t padded = (size_t)((name_len + 3u) & ~3u);
        uint8_t namebuf[8];
        CHECK(read_n(fd, namebuf, padded, 5000) == 0,
              "x11 client: GetAtomName(STRING) name timed out");
        CHECK(memcmp(namebuf, "STRING", 6u) == 0,
              "x11 client: GetAtomName(STRING) name mismatch");
        fprintf(stderr, "x11 client: GetAtomName(31) = \"STRING\" ok\n");
    }

    /* 11. ChangeProperty WM_NAME = "test". */
    {
        const char value[] = "test";
        size_t vlen = sizeof(value) - 1u;
        size_t padded = (vlen + 3u) & ~(size_t)3u;
        size_t total = 24u + padded;
        uint8_t *cp = calloc(1, total);
        CHECK(cp != NULL, "x11 client: out of memory");
        cp[0] = X11_REQ_CHANGE_PROPERTY;
        cp[1] = X11_PROP_MODE_REPLACE;
        put_u16_le(cp + 2, (uint16_t)(total / 4u));
        put_u32_le(cp + 4, wid);
        put_u32_le(cp + 8, X11_ATOM_WM_NAME);
        put_u32_le(cp + 12, X11_ATOM_STRING);
        cp[16] = 8u;  /* format */
        put_u32_le(cp + 20, (uint32_t)vlen);
        memcpy(cp + 24, value, vlen);
        CHECK(send_all(fd, cp, total) == 0,
              "x11 client: send ChangeProperty: %s", strerror(errno));
        free(cp);
        /* ChangeProperty is void; a PropertyNotify event may arrive
         * because the client selected PropertyChange. Drain it. */
        (void)drain_input(fd, 100);
    }
    fprintf(stderr, "x11 client: ChangeProperty WM_NAME=\"test\" ok\n");

    /* 12. DeleteProperty WM_NAME. */
    {
        uint8_t dp[12];
        memset(dp, 0, sizeof(dp));
        dp[0] = X11_REQ_DELETE_PROPERTY;
        put_u16_le(dp + 2, 3u);
        put_u32_le(dp + 4, wid);
        put_u32_le(dp + 8, X11_ATOM_WM_NAME);
        CHECK(send_all(fd, dp, sizeof(dp)) == 0,
              "x11 client: send DeleteProperty: %s", strerror(errno));
        /* DeleteProperty is void; a PropertyNotify(Deleted) event may
         * arrive. Drain it. */
        (void)drain_input(fd, 100);
    }
    fprintf(stderr, "x11 client: DeleteProperty WM_NAME ok\n");

    /* 13. GetProperty WM_NAME. Expect type=0 (property does not
     * exist) since we just deleted it. */
    {
        uint8_t gp[24];
        memset(gp, 0, sizeof(gp));
        gp[0] = X11_REQ_GET_PROPERTY;
        gp[1] = 0u;  /* delete = false */
        put_u16_le(gp + 2, 6u);
        put_u32_le(gp + 4, wid);
        put_u32_le(gp + 8, X11_ATOM_WM_NAME);
        put_u32_le(gp + 12, X11_ATOM_STRING);
        put_u32_le(gp + 16, 0u);
        put_u32_le(gp + 20, 1024u);
        CHECK(send_all(fd, gp, sizeof(gp)) == 0,
              "x11 client: send GetProperty: %s", strerror(errno));

        uint8_t reply_hdr[32];
        CHECK(read_n(fd, reply_hdr, sizeof(reply_hdr), 5000) == 0,
              "x11 client: GetProperty reply header timed out");
        CHECK(reply_hdr[0] == 1u, "x11 client: GetProperty byte0=%u",
              reply_hdr[0]);
        uint8_t fmt = reply_hdr[1];
        uint32_t type = get_u32_le(reply_hdr + 8);
        uint32_t value_len = get_u32_le(reply_hdr + 16);
        uint32_t reply_extra = get_u32_le(reply_hdr + 4);
        CHECK(fmt == 0u,
              "x11 client: GetProperty format=%u (expected 0 after delete)",
              (unsigned)fmt);
        CHECK(type == 0u,
              "x11 client: GetProperty type=%u (expected 0 after delete)",
              (unsigned)type);
        CHECK(value_len == 0u,
              "x11 client: GetProperty value_len=%u (expected 0)",
              (unsigned)value_len);
        /* reply_extra can be 0; drain any padded bytes just in case. */
        size_t extra_bytes = (size_t)reply_extra * 4u;
        if (extra_bytes > 0u) {
            uint8_t sink[8];
            CHECK(read_n(fd, sink, extra_bytes, 5000) == 0,
                  "x11 client: GetProperty extra bytes timed out");
        }
        fprintf(stderr,
                "x11 client: GetProperty after delete -> type=0 fmt=0 ok\n");
    }

    /* 14. SetSelectionOwner(PRIMARY, window, time=1). */
    {
        uint8_t sso[16];
        memset(sso, 0, sizeof(sso));
        sso[0] = X11_REQ_SET_SEL_OWNER;
        put_u16_le(sso + 2, 4u);
        put_u32_le(sso + 4, wid);
        put_u32_le(sso + 8, X11_ATOM_PRIMARY);
        put_u32_le(sso + 12, 1u);  /* time */
        CHECK(send_all(fd, sso, sizeof(sso)) == 0,
              "x11 client: send SetSelectionOwner: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0,
              "x11 client: SetSelectionOwner errored");
    }
    fprintf(stderr, "x11 client: SetSelectionOwner(PRIMARY, wid) ok\n");

    /* 15. GetSelectionOwner(PRIMARY). Expect owner == wid. */
    {
        uint8_t gso[8];
        memset(gso, 0, sizeof(gso));
        gso[0] = X11_REQ_GET_SEL_OWNER;
        put_u16_le(gso + 2, 2u);
        put_u32_le(gso + 4, X11_ATOM_PRIMARY);
        CHECK(send_all(fd, gso, sizeof(gso)) == 0,
              "x11 client: send GetSelectionOwner: %s", strerror(errno));

        uint8_t reply[32];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: GetSelectionOwner reply timed out");
        CHECK(reply[0] == 1u, "x11 client: GSO byte0=%u", reply[0]);
        uint32_t owner = get_u32_le(reply + 8);
        CHECK(owner == wid,
              "x11 client: GSO owner=0x%x expected 0x%x",
              (unsigned)owner, (unsigned)wid);
        fprintf(stderr, "x11 client: GetSelectionOwner -> 0x%x ok\n",
                (unsigned)owner);
    }

    /* 16. SetSelectionOwner(PRIMARY, 0, time=2) to clear. The
     * bridge emits a SelectionClear event to the prior owner (wid);
     * drain it so it does not confuse the next reply. */
    {
        uint8_t sso[16];
        memset(sso, 0, sizeof(sso));
        sso[0] = X11_REQ_SET_SEL_OWNER;
        put_u16_le(sso + 2, 4u);
        put_u32_le(sso + 4, 0u);  /* clear */
        put_u32_le(sso + 8, X11_ATOM_PRIMARY);
        put_u32_le(sso + 12, 2u);  /* time */
        CHECK(send_all(fd, sso, sizeof(sso)) == 0,
              "x11 client: send SetSelectionOwner(clear): %s", strerror(errno));
        /* Drain the SelectionClear event the bridge delivered to wid. */
        (void)drain_input(fd, 200);
    }
    fprintf(stderr, "x11 client: SetSelectionOwner(PRIMARY, 0) clear ok\n");

    /* 17. GetSelectionOwner(PRIMARY). Expect owner == 0. */
    {
        uint8_t gso[8];
        memset(gso, 0, sizeof(gso));
        gso[0] = X11_REQ_GET_SEL_OWNER;
        put_u16_le(gso + 2, 2u);
        put_u32_le(gso + 4, X11_ATOM_PRIMARY);
        CHECK(send_all(fd, gso, sizeof(gso)) == 0,
              "x11 client: send GetSelectionOwner(2): %s", strerror(errno));

        uint8_t reply[32];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: GSO(2) reply timed out");
        CHECK(reply[0] == 1u, "x11 client: GSO(2) byte0=%u", reply[0]);
        uint32_t owner = get_u32_le(reply + 8);
        CHECK(owner == 0u,
              "x11 client: GSO(2) owner=0x%x expected 0",
              (unsigned)owner);
        fprintf(stderr, "x11 client: GetSelectionOwner -> 0 ok\n");
    }

    /* 18. QueryPointer on root. Verify reply structure. */
    {
        uint8_t qp[8];
        memset(qp, 0, sizeof(qp));
        qp[0] = X11_REQ_QUERY_POINTER;
        put_u16_le(qp + 2, 2u);
        put_u32_le(qp + 4, root_xid);
        CHECK(send_all(fd, qp, sizeof(qp)) == 0,
              "x11 client: send QueryPointer: %s", strerror(errno));

        uint8_t reply[32];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: QueryPointer reply timed out");
        CHECK(reply[0] == 1u, "x11 client: QP byte0=%u", reply[0]);
        uint8_t same_screen = reply[1];
        uint32_t root = get_u32_le(reply + 8);
        uint32_t child = get_u32_le(reply + 12);
        int16_t root_x = (int16_t)get_u16_le(reply + 16);
        int16_t root_y = (int16_t)get_u16_le(reply + 18);
        int16_t win_x  = (int16_t)get_u16_le(reply + 20);
        int16_t win_y  = (int16_t)get_u16_le(reply + 22);
        uint16_t mask  = get_u16_le(reply + 24);
        CHECK(same_screen == 1u,
              "x11 client: QP same-screen=%u (expected 1)",
              (unsigned)same_screen);
        CHECK(root == root_xid,
              "x11 client: QP root=0x%x expected 0x%x",
              (unsigned)root, (unsigned)root_xid);
        /* On the headless backend the bridge reports no window under
         * the pointer and zero coordinates. */
        (void)child; (void)root_x; (void)root_y; (void)win_x; (void)win_y;
        (void)mask;
        fprintf(stderr, "x11 client: QueryPointer same-screen=1 ok\n");
    }

    /* 19. SetInputFocus(revert-to=Ancestor, focus=window, time=0). */
    {
        uint8_t sif[12];
        memset(sif, 0, sizeof(sif));
        sif[0] = X11_REQ_SET_INPUT_FOCUS;
        sif[1] = 2u;  /* revert-to = Ancestor */
        put_u16_le(sif + 2, 3u);
        put_u32_le(sif + 4, wid);
        put_u32_le(sif + 8, 0u);  /* time = CurrentTime */
        CHECK(send_all(fd, sif, sizeof(sif)) == 0,
              "x11 client: send SetInputFocus: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0,
              "x11 client: SetInputFocus errored");
    }
    fprintf(stderr, "x11 client: SetInputFocus ok\n");

    /* 20. CreateGC on the window. */
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

    /* 21. PutImage a 4x4 ZPixmap depth-24 image on the window.
     * ZPixmap depth 24 with scanline-pad 32 = 4 bytes per pixel, so
     * 4x4 = 64 bytes of data, padded to 4 (already). Total request
     * = 24 (fixed) + 64 (data) = 88 bytes, length = 22 units. */
    {
        uint16_t width = 4u, height = 4u;
        uint8_t depth = 24u;
        size_t data_bytes = (size_t)width * height * 4u;  /* 4 bpp */
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
        /* bytes 22..23 unused (already zero) */
        /* data: leave zeroed (all black pixels). */
        CHECK(send_all(fd, pi, total) == 0,
              "x11 client: send PutImage: %s", strerror(errno));
        free(pi);
        CHECK(expect_no_error(fd) == 0,
              "x11 client: PutImage errored");
    }
    fprintf(stderr, "x11 client: PutImage 4x4 ZPixmap ok\n");

    /* 22. Clean close. */
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

    snprintf(isz_sock, sizeof(isz_sock), "/tmp/.ishizue-x11opc2-%d",
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
        printf("test_x11_opcodes2: PASS\n");
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
