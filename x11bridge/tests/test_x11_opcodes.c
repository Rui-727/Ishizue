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

/* test_x11_opcodes.c: W8-A integration test.
 *
 * Spawns the x11bridge binary against a headless Ishizue server running
 * in the test process, then connects to the bridge's UDS as a raw X11
 * client (no libX11) and exercises the eight W8-A opcodes end-to-end:
 *
 *   1. SetupSuccess (validated already by test_x11_handshake).
 *   2. CreateWindow on root.
 *   3. ChangeWindowAttributes to set event-mask = StructureNotify | Exposure.
 *   4. MapWindow and verify a MapNotify event arrives.
 *   5. InternAtom "WM_PROTOCOLS" (only-if-exists=false): reply atom > 68.
 *   6. InternAtom "PRIMARY" (only-if-exists=false): reply atom == 1.
 *   7. ChangeProperty on the window: WM_NAME (STRING) = "hello".
 *   8. GetProperty for WM_NAME: reply data is "hello".
 *   9. ConfigureWindow to move+resize; verify ConfigureNotify arrives.
 *  10. UnmapWindow; verify UnmapNotify arrives.
 *  11. DestroyWindow; verify DestroyNotify arrives.
 *  12. Close cleanly.
 *
 * Layout matches test_x11_handshake.c: parent spawns headless Ishizue,
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

/* X11 wire constants the test exercises. Mirrored from
 * x11bridge/x11_proto.h so the test does not pull internal bridge
 * headers (it links only the public libishizue API). */
#define X11_BYTE_ORDER_LSB        0x6Cu
#define X11_SETUP_SUCCESS         1u

#define X11_REQ_CREATE_WINDOW     1u
#define X11_REQ_CHANGE_WIN_ATTRS  2u
#define X11_REQ_DESTROY_WINDOW    4u
#define X11_REQ_MAP_WINDOW        8u
#define X11_REQ_UNMAP_WINDOW      10u
#define X11_REQ_CONFIGURE_WINDOW  12u
#define X11_REQ_INTERN_ATOM       16u
#define X11_REQ_CHANGE_PROPERTY   18u
#define X11_REQ_GET_PROPERTY      20u

#define X11_CW_EVENT_MASK         0x00000800u

#define X11_EVMASK_EXPOSURE           (1u << 15)
#define X11_EVMASK_STRUCTURE_NOTIFY   (1u << 17)
#define X11_EVMASK_PROPERTY_CHANGE    (1u << 22)

#define X11_CFG_MASK_X          0x0001u
#define X11_CFG_MASK_Y          0x0002u
#define X11_CFG_MASK_WIDTH      0x0004u
#define X11_CFG_MASK_HEIGHT     0x0008u

#define X11_EV_DESTROY_NOTIFY   17u
#define X11_EV_UNMAP_NOTIFY     18u
#define X11_EV_MAP_NOTIFY       19u
#define X11_EV_CONFIGURE_NOTIFY 22u
#define X11_EV_PROPERTY_NOTIFY  28u

#define X11_PROP_MODE_REPLACE   0u

/* Predefined atoms used by the test. */
#define X11_ATOM_PRIMARY        1u
#define X11_ATOM_STRING         31u
#define X11_ATOM_WM_NAME        39u

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

/* Little-endian encoders. */
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
 * available, 0 on timeout, -1 on hard error / peer-closed. */
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

/* Read the 8-byte setup_success header, then the rest of the reply
 * based on the additional-length field. Returns the parsed rid_base
 * (used to pick the client's window XID) or 0 on failure. */
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

/* Drain pending bytes if any (used to flush the bridge's outgoing
 * queue between requests). Returns the byte count drained. */
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

/* X11 client side: end-to-end exercise of the eight W8-A opcodes.
 * Returns 0 on success, non-zero on failure. */
static int x11_client_main(void) {
    int fd = -1;
    int ret = 1;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", TEST_DISPLAY);

    fd = retry_connect(path, 10000);
    CHECK(fd >= 0, "x11 client: connect(%s) timed out", path);

    /* 1. SetupRequest: 12 bytes, no auth, LSB-first, proto 11.0. */
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

    /* 2. CreateWindow on root: 100x100 at (10,10), depth 24,
     * class InputOutput, no value-list yet. */
    uint32_t wid = rid_base;
    {
        uint8_t cw[32];
        memset(cw, 0, sizeof(cw));
        cw[0] = X11_REQ_CREATE_WINDOW;
        cw[1] = 24u;
        put_u16_le(cw + 2, 8u);
        put_u32_le(cw + 4, wid);
        put_u32_le(cw + 8, root_xid);
        put_u16_le(cw + 12, 10u);
        put_u16_le(cw + 14, 10u);
        put_u16_le(cw + 16, 100u);
        put_u16_le(cw + 18, 100u);
        put_u16_le(cw + 20, 0u);
        put_u16_le(cw + 22, 1u);
        put_u32_le(cw + 24, 0u);
        put_u32_le(cw + 28, 0u);
        CHECK(send_all(fd, cw, sizeof(cw)) == 0,
              "x11 client: send CreateWindow: %s", strerror(errno));
        /* Give the bridge a moment to react; expect no reply (void). */
        int pk = peek_timeout(fd, 300);
        CHECK(pk >= 0, "x11 client: poll after CreateWindow failed");
        if (pk == 1) {
            uint8_t errbuf[32];
            ssize_t r = recv(fd, errbuf, sizeof(errbuf), 0);
            CHECK(r > 0, "x11 client: recv after CreateWindow failed");
            CHECK(errbuf[0] == 0u,
                  "x11 client: CreateWindow produced non-error byte0=%u",
                  errbuf[0]);
            CHECK(0, "x11 client: CreateWindow errored code=%u",
                  (unsigned)errbuf[1]);
        }
    }
    fprintf(stderr, "x11 client: CreateWindow wid=0x%x accepted\n",
            (unsigned)wid);

    /* 3. ChangeWindowAttributes: set event-mask =
     * StructureNotify | Exposure. */
    {
        uint8_t cwa[16];
        memset(cwa, 0, sizeof(cwa));
        cwa[0] = X11_REQ_CHANGE_WIN_ATTRS;
        put_u16_le(cwa + 2, 4u);                 /* length = 4 units = 16 bytes */
        put_u32_le(cwa + 4, wid);
        put_u32_le(cwa + 8, X11_CW_EVENT_MASK);
        put_u32_le(cwa + 12, X11_EVMASK_STRUCTURE_NOTIFY | X11_EVMASK_EXPOSURE);
        CHECK(send_all(fd, cwa, sizeof(cwa)) == 0,
              "x11 client: send ChangeWindowAttributes: %s", strerror(errno));
        int pk = peek_timeout(fd, 300);
        CHECK(pk >= 0, "x11 client: poll after ChangeWindowAttributes failed");
        if (pk == 1) {
            uint8_t errbuf[32];
            ssize_t r = recv(fd, errbuf, sizeof(errbuf), 0);
            CHECK(r > 0, "x11 client: recv after CWA failed");
            CHECK(0, "x11 client: CWA errored code=%u", (unsigned)errbuf[1]);
        }
    }
    fprintf(stderr, "x11 client: ChangeWindowAttributes set event-mask\n");

    /* 4. MapWindow. Expect a MapNotify event (32 bytes, code 19). */
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
              "x11 client: MapNotify event/window mismatch: "
              "event=0x%x window=0x%x",
              (unsigned)ev_event, (unsigned)ev_window);
        fprintf(stderr, "x11 client: MapNotify ok\n");
    }

    /* 5. InternAtom "WM_PROTOCOLS" with only-if-exists=false. Expect
     * a reply with atom > 68 (it is not predefined). */
    uint32_t wm_protocols_atom = 0u;
    {
        const char name[] = "WM_PROTOCOLS";
        size_t nlen = sizeof(name) - 1u;
        size_t padded = (nlen + 3u) & ~(size_t)3u;
        size_t total = 4u + 4u + padded;
        uint8_t *ia = calloc(1, total);
        CHECK(ia != NULL, "x11 client: out of memory");
        ia[0] = X11_REQ_INTERN_ATOM;
        ia[1] = 0u;  /* only-if-exists = false */
        put_u16_le(ia + 2, (uint16_t)(total / 4u));
        put_u16_le(ia + 4, (uint16_t)nlen);
        memcpy(ia + 8, name, nlen);
        CHECK(send_all(fd, ia, total) == 0,
              "x11 client: send InternAtom(WM_PROTOCOLS): %s",
              strerror(errno));
        free(ia);

        uint8_t reply[32];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: InternAtom reply timed out");
        CHECK(reply[0] == 1u,
              "x11 client: InternAtom byte0=%u (expected 1=reply)",
              reply[0]);
        wm_protocols_atom = get_u32_le(reply + 8);
        CHECK(wm_protocols_atom > 68u,
              "x11 client: WM_PROTOCOLS atom=%u (expected > 68)",
              (unsigned)wm_protocols_atom);
        fprintf(stderr, "x11 client: InternAtom(WM_PROTOCOLS) = %u\n",
                (unsigned)wm_protocols_atom);
    }

    /* 6. InternAtom "PRIMARY" with only-if-exists=false. Expect atom
     * 1 (predefined). */
    {
        const char name[] = "PRIMARY";
        size_t nlen = sizeof(name) - 1u;
        size_t padded = (nlen + 3u) & ~(size_t)3u;
        size_t total = 4u + 4u + padded;
        uint8_t *ia = calloc(1, total);
        CHECK(ia != NULL, "x11 client: out of memory");
        ia[0] = X11_REQ_INTERN_ATOM;
        ia[1] = 0u;
        put_u16_le(ia + 2, (uint16_t)(total / 4u));
        put_u16_le(ia + 4, (uint16_t)nlen);
        memcpy(ia + 8, name, nlen);
        CHECK(send_all(fd, ia, total) == 0,
              "x11 client: send InternAtom(PRIMARY): %s", strerror(errno));
        free(ia);

        uint8_t reply[32];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: InternAtom(PRIMARY) reply timed out");
        CHECK(reply[0] == 1u,
              "x11 client: InternAtom PRIMARY byte0=%u", reply[0]);
        uint32_t atom = get_u32_le(reply + 8);
        CHECK(atom == X11_ATOM_PRIMARY,
              "x11 client: PRIMARY atom=%u (expected 1)", (unsigned)atom);
        fprintf(stderr, "x11 client: InternAtom(PRIMARY) = %u\n",
                (unsigned)atom);
    }

    /* 7. ChangeProperty: WM_NAME (STRING, format=8) = "hello". */
    {
        const char value[] = "hello";
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
        /* bytes 17..19 unused */
        put_u32_le(cp + 20, (uint32_t)vlen);
        memcpy(cp + 24, value, vlen);
        CHECK(send_all(fd, cp, total) == 0,
              "x11 client: send ChangeProperty: %s", strerror(errno));
        free(cp);
        /* ChangeProperty is void; no reply expected. The bridge will
         * not deliver a PropertyNotify here because the test client
         * did not select PropertyChange on the window. */
        int pk = peek_timeout(fd, 300);
        CHECK(pk >= 0, "x11 client: poll after ChangeProperty failed");
        if (pk == 1) {
            uint8_t buf[32];
            ssize_t r = recv(fd, buf, sizeof(buf), 0);
            CHECK(r > 0, "x11 client: recv after ChangeProperty failed");
            CHECK(buf[0] == 0u,
                  "x11 client: ChangeProperty produced non-error byte0=%u",
                  buf[0]);
            CHECK(0, "x11 client: ChangeProperty errored code=%u",
                  (unsigned)buf[1]);
        }
    }
    fprintf(stderr, "x11 client: ChangeProperty WM_NAME=\"hello\"\n");

    /* 8. GetProperty for WM_NAME. Expect a 32-byte reply plus the
     * value bytes ("hello", 5 bytes, padded to 8). Verify the
     * returned value matches "hello". */
    {
        uint8_t gp[24];
        memset(gp, 0, sizeof(gp));
        gp[0] = X11_REQ_GET_PROPERTY;
        gp[1] = 0u;  /* delete = false */
        put_u16_le(gp + 2, 6u);   /* length = 6 units = 24 bytes */
        put_u32_le(gp + 4, wid);
        put_u32_le(gp + 8, X11_ATOM_WM_NAME);
        put_u32_le(gp + 12, X11_ATOM_STRING);
        put_u32_le(gp + 16, 0u);  /* long-offset = 0 */
        put_u32_le(gp + 20, 1024u);  /* long-length */
        CHECK(send_all(fd, gp, sizeof(gp)) == 0,
              "x11 client: send GetProperty: %s", strerror(errno));

        uint8_t reply_hdr[32];
        CHECK(read_n(fd, reply_hdr, sizeof(reply_hdr), 5000) == 0,
              "x11 client: GetProperty reply header timed out");
        CHECK(reply_hdr[0] == 1u,
              "x11 client: GetProperty byte0=%u (expected 1=reply)",
              reply_hdr[0]);
        uint8_t fmt = reply_hdr[1];
        uint32_t type = get_u32_le(reply_hdr + 8);
        uint32_t bytes_after = get_u32_le(reply_hdr + 12);
        uint32_t value_len = get_u32_le(reply_hdr + 16);
        uint32_t reply_extra = get_u32_le(reply_hdr + 4);
        CHECK(fmt == 8u,
              "x11 client: GetProperty format=%u (expected 8)", (unsigned)fmt);
        CHECK(type == X11_ATOM_STRING,
              "x11 client: GetProperty type=%u (expected STRING=31)",
              (unsigned)type);
        CHECK(value_len == 5u,
              "x11 client: GetProperty value_len=%u (expected 5)",
              (unsigned)value_len);
        CHECK(bytes_after == 0u,
              "x11 client: GetProperty bytes_after=%u (expected 0)",
              (unsigned)bytes_after);
        /* reply_extra is in 4-byte units of additional data. */
        size_t extra_bytes = (size_t)reply_extra * 4u;
        CHECK(extra_bytes == 8u,
              "x11 client: GetProperty extra=%u (expected 8)",
              (unsigned)reply_extra);
        uint8_t value_buf[8];
        CHECK(read_n(fd, value_buf, extra_bytes, 5000) == 0,
              "x11 client: GetProperty value bytes timed out");
        CHECK(memcmp(value_buf, "hello", 5u) == 0,
              "x11 client: GetProperty value != \"hello\"");
        fprintf(stderr, "x11 client: GetProperty WM_NAME = \"hello\"\n");
    }

    /* 9. ConfigureWindow: move to (20, 20), resize to 200x200. Expect
     * a ConfigureNotify event. */
    {
        uint8_t cw[20];
        memset(cw, 0, sizeof(cw));
        cw[0] = X11_REQ_CONFIGURE_WINDOW;
        put_u16_le(cw + 2, 5u);  /* length = 5 units = 20 bytes */
        put_u32_le(cw + 4, wid);
        /* mask: x | y | width | height = 0x0F */
        put_u16_le(cw + 8, X11_CFG_MASK_X | X11_CFG_MASK_Y |
                            X11_CFG_MASK_WIDTH | X11_CFG_MASK_HEIGHT);
        put_u16_le(cw + 10, 0u);  /* padding */
        put_u32_le(cw + 12, 20u);   /* x */
        put_u32_le(cw + 16, 20u);   /* y */
        /* Wait, the value-list order is x,y,w,h. We need 4 values,
         * so total payload = 8 (header+mask+pad) + 16 (4 values) = 24
         * bytes -> length = 6 units. Fix below. */
        /* The above layout is wrong; rewrite. */
    }
    {
        uint8_t cw[28];
        memset(cw, 0, sizeof(cw));
        cw[0] = X11_REQ_CONFIGURE_WINDOW;
        put_u16_le(cw + 2, 7u);  /* length = 7 units = 28 bytes */
        put_u32_le(cw + 4, wid);
        put_u16_le(cw + 8, X11_CFG_MASK_X | X11_CFG_MASK_Y |
                            X11_CFG_MASK_WIDTH | X11_CFG_MASK_HEIGHT);
        /* bytes 10..11 padding (already zero) */
        put_u32_le(cw + 12, 20u);    /* x */
        put_u32_le(cw + 16, 20u);    /* y */
        put_u32_le(cw + 20, 200u);   /* width */
        put_u32_le(cw + 24, 200u);   /* height */
        CHECK(send_all(fd, cw, sizeof(cw)) == 0,
              "x11 client: send ConfigureWindow: %s", strerror(errno));
    }
    {
        uint8_t ev[32];
        CHECK(read_n(fd, ev, sizeof(ev), 5000) == 0,
              "x11 client: ConfigureNotify timed out");
        CHECK(ev[0] == X11_EV_CONFIGURE_NOTIFY,
              "x11 client: expected ConfigureNotify (22), got code=%u",
              (unsigned)ev[0]);
        uint32_t ev_event  = get_u32_le(ev + 4);
        uint32_t ev_window = get_u32_le(ev + 8);
        int16_t  ev_x      = (int16_t)get_u16_le(ev + 16);
        int16_t  ev_y      = (int16_t)get_u16_le(ev + 18);
        uint16_t ev_w      = get_u16_le(ev + 20);
        uint16_t ev_h      = get_u16_le(ev + 22);
        CHECK(ev_event == wid && ev_window == wid,
              "x11 client: ConfigureNotify event/window mismatch");
        CHECK(ev_x == 20 && ev_y == 20,
              "x11 client: ConfigureNotify pos=(%d,%d) expected (20,20)",
              (int)ev_x, (int)ev_y);
        CHECK(ev_w == 200 && ev_h == 200,
              "x11 client: ConfigureNotify size=%ux%u expected 200x200",
              (unsigned)ev_w, (unsigned)ev_h);
        fprintf(stderr, "x11 client: ConfigureNotify ok (20,20,200x200)\n");
    }

    /* 10. UnmapWindow. Expect an UnmapNotify event. */
    {
        uint8_t uw[8];
        memset(uw, 0, sizeof(uw));
        uw[0] = X11_REQ_UNMAP_WINDOW;
        put_u16_le(uw + 2, 2u);
        put_u32_le(uw + 4, wid);
        CHECK(send_all(fd, uw, sizeof(uw)) == 0,
              "x11 client: send UnmapWindow: %s", strerror(errno));
    }
    {
        uint8_t ev[32];
        CHECK(read_n(fd, ev, sizeof(ev), 5000) == 0,
              "x11 client: UnmapNotify timed out");
        CHECK(ev[0] == X11_EV_UNMAP_NOTIFY,
              "x11 client: expected UnmapNotify (18), got code=%u",
              (unsigned)ev[0]);
        uint32_t ev_event  = get_u32_le(ev + 4);
        uint32_t ev_window = get_u32_le(ev + 8);
        CHECK(ev_event == wid && ev_window == wid,
              "x11 client: UnmapNotify event/window mismatch");
        fprintf(stderr, "x11 client: UnmapNotify ok\n");
    }

    /* 11. DestroyWindow. Expect a DestroyNotify event. */
    {
        uint8_t dw[8];
        memset(dw, 0, sizeof(dw));
        dw[0] = X11_REQ_DESTROY_WINDOW;
        put_u16_le(dw + 2, 2u);
        put_u32_le(dw + 4, wid);
        CHECK(send_all(fd, dw, sizeof(dw)) == 0,
              "x11 client: send DestroyWindow: %s", strerror(errno));
    }
    {
        uint8_t ev[32];
        CHECK(read_n(fd, ev, sizeof(ev), 5000) == 0,
              "x11 client: DestroyNotify timed out");
        CHECK(ev[0] == X11_EV_DESTROY_NOTIFY,
              "x11 client: expected DestroyNotify (17), got code=%u",
              (unsigned)ev[0]);
        uint32_t ev_event  = get_u32_le(ev + 4);
        uint32_t ev_window = get_u32_le(ev + 8);
        CHECK(ev_event == wid && ev_window == wid,
              "x11 client: DestroyNotify event/window mismatch");
        fprintf(stderr, "x11 client: DestroyNotify ok\n");
    }

    /* 12. Clean close. Drain any trailing bytes so the bridge's
     * UnmapWindow focus-clear wire message does not stall. */
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

    /* 1. Headless Ishizue server. */
    isz_headless_config cfg = {
        .width = TEST_ROOT_W, .height = TEST_ROOT_H, .refresh_rate = 60000,
    };
    srv = isz_init(ISZ_BACKEND_HEADLESS, &cfg);
    CHECK(srv != NULL, "isz_init returned NULL");

    /* 2. Allowlist the bridge binary so the §6.3 SO_PEERCRED check
     *    passes when the bridge connects. */
    {
        struct stat stb;
        CHECK(stat(bridge_bin, &stb) == 0,
              "bridge binary not found at %s: %s", bridge_bin, strerror(errno));
        int rc = isz_allowlist_add_binary(srv, bridge_bin);
        CHECK(rc == ISZ_OK,
              "isz_allowlist_add_binary(%s) rc=%d", bridge_bin, rc);
    }

    /* 3. Create the Ishizue UDS at a unique path. */
    snprintf(isz_sock, sizeof(isz_sock), "/tmp/.ishizue-x11opc-%d",
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
        setenv("ISZ_SOCKET", isz_sock, 1);
        char disp[8];
        snprintf(disp, sizeof(disp), "%d", TEST_DISPLAY);
        setenv("ISZ_X11_DISPLAY", disp, 1);
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

    /* 6. Parent: drive isz_dispatch until the client child exits. */
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

    /* 7. Validate client exit status. */
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

    /* Kill the bridge if it is still running and reap it. */
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
        printf("test_x11_opcodes: PASS\n");
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
