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
 * FROM OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* test_x11_fonts.c: W11-A integration test.
 *
 * Spawns the x11bridge binary against a headless Ishizue server
 * running in the test process, then connects to the bridge's UDS as a
 * raw X11 client (no libX11) and exercises the twelve W11-A font and
 * text opcodes end-to-end:
 *
 *   1. CreateWindow + CreateGC (anchors for the text-drawing opcodes).
 *   2. OpenFont "fixed": no error.
 *   3. QueryFont: 60-byte reply, length field = 13, metrics all zero.
 *   4. QueryTextExtents: 32-byte reply, draw-direction = 0, all zero.
 *   5. ListFonts with pattern "*": empty reply (names_len = 0).
 *   6. ListFontsWithInfo with pattern "*": 60-byte terminator reply,
 *      name_len = 0, length = 13.
 *   7. GetFontPath: 32-byte reply, npaths = 0.
 *   8. SetFontPath with nfonts = 0: no error.
 *   9. ImageText8 "hello" on the window: no error.
 *  10. PolyText8 with one TEXTITEM8 on the window: no error.
 *  11. CloseFont: no error.
 *  12. CloseFont again: no error (silent no-op per X11).
 *  13. QueryFont on a non-existent font: BadFont error.
 *  14. Close cleanly.
 *
 * Layout matches test_x11_colormap.c: parent spawns headless Ishizue,
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
#define X11_REQ_CREATE_GC         55u
#define X11_REQ_OPEN_FONT         45u
#define X11_REQ_CLOSE_FONT        46u
#define X11_REQ_QUERY_FONT        47u
#define X11_REQ_QUERY_TEXT_EXTENTS 48u
#define X11_REQ_LIST_FONTS        49u
#define X11_REQ_LIST_FONTS_WITH_INFO 50u
#define X11_REQ_SET_FONT_PATH     51u
#define X11_REQ_GET_FONT_PATH     52u
#define X11_REQ_POLY_TEXT_8       74u
#define X11_REQ_IMAGE_TEXT_8      76u

#define X11_ERR_FONT              7u

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

/* X11 client side: end-to-end exercise of the twelve W11-A opcodes. */
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

    /* Pick XIDs in the client's resource-id range. wid at offset 0,
     * gc at offset 8, font at offset 16, bad_font at offset 64 (never
     * opened). */
    uint32_t wid         = rid_base + 0u;
    uint32_t gc_xid      = rid_base + 8u;
    uint32_t font_xid    = rid_base + 16u;
    uint32_t bad_font_xid = rid_base + 64u;

    /* 2. CreateWindow on root. The text-drawing opcodes need a
     * tracked drawable. */
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
        CHECK(expect_no_error(fd) == 0, "x11 client: CreateWindow errored");
    }
    fprintf(stderr, "x11 client: CreateWindow wid=0x%x accepted\n",
            (unsigned)wid);

    /* 3. CreateGC on the window. The text-drawing opcodes need a
     * tracked GC. */
    {
        uint8_t cg[16];
        memset(cg, 0, sizeof(cg));
        cg[0] = X11_REQ_CREATE_GC;
        put_u16_le(cg + 2, 4u);
        put_u32_le(cg + 4, gc_xid);
        put_u32_le(cg + 8, wid);
        put_u32_le(cg + 12, 0u);  /* empty value-mask */
        CHECK(send_all(fd, cg, sizeof(cg)) == 0,
              "x11 client: send CreateGC: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0, "x11 client: CreateGC errored");
    }
    fprintf(stderr, "x11 client: CreateGC gc=0x%x ok\n", (unsigned)gc_xid);

    /* 4. OpenFont "fixed". Total request = 12 + pad4(5) = 12 + 8 = 20
     * bytes. Length = 5. */
    {
        const char name[] = "fixed";
        size_t nlen = sizeof(name) - 1u;  /* 5 */
        size_t padded = (nlen + 3u) & ~(size_t)3u;  /* 8 */
        size_t total = 12u + padded;  /* 20 */
        uint8_t *of = calloc(1, total);
        CHECK(of != NULL, "x11 client: out of memory");
        of[0] = X11_REQ_OPEN_FONT;
        put_u16_le(of + 2, (uint16_t)(total / 4u));
        put_u32_le(of + 4, font_xid);
        put_u16_le(of + 8, (uint16_t)nlen);
        memcpy(of + 12, name, nlen);
        CHECK(send_all(fd, of, total) == 0,
              "x11 client: send OpenFont: %s", strerror(errno));
        free(of);
        CHECK(expect_no_error(fd) == 0, "x11 client: OpenFont errored");
    }
    fprintf(stderr, "x11 client: OpenFont \"fixed\" font=0x%x ok\n",
            (unsigned)font_xid);

    /* 5. QueryFont. Total request = 8 bytes. Reply must be 60 bytes
     * with byte 0 = 1 (reply), length field at bytes 4..7 = 13, and
     * all 52 bytes of metrics (bytes 8..59) zero. */
    {
        uint8_t qf[8];
        memset(qf, 0, sizeof(qf));
        qf[0] = X11_REQ_QUERY_FONT;
        put_u16_le(qf + 2, 2u);
        put_u32_le(qf + 4, font_xid);
        CHECK(send_all(fd, qf, sizeof(qf)) == 0,
              "x11 client: send QueryFont: %s", strerror(errno));

        uint8_t reply[60];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: QueryFont reply timed out");
        CHECK(reply[0] == 1u, "x11 client: QueryFont byte0=%u", reply[0]);
        uint32_t length = get_u32_le(reply + 4);
        CHECK(length == 13u,
              "x11 client: QueryFont length=%u (expected 13)",
              (unsigned)length);
        /* The bridge returns plausible 6x13 fixed-font metrics so
         * apps that need ascent/descent don't bail. Verify the
         * key fields are non-zero: font-ascent at offset 52,
         * font-descent at offset 54. */
        uint16_t font_ascent = get_u16_le(reply + 52);
        uint16_t font_descent = get_u16_le(reply + 54);
        CHECK(font_ascent > 0 && font_descent > 0,
              "x11 client: QueryFont ascent/descent zero (got %u/%u)",
              (unsigned)font_ascent, (unsigned)font_descent);
        fprintf(stderr,
                "x11 client: QueryFont font=0x%x -> 60-byte reply ok (ascent=%u descent=%u)\n",
                (unsigned)font_xid, (unsigned)font_ascent,
                (unsigned)font_descent);
    }

    /* 6. QueryTextExtents. Two 2-byte chars ("AB" in 16-bit). Length
     * = 3, total = 12 bytes. Reply must be 32 bytes with byte 0 = 1,
     * byte 1 = 0 (LeftToRight), length = 0. */
    {
        uint8_t qt[12];
        memset(qt, 0, sizeof(qt));
        qt[0] = X11_REQ_QUERY_TEXT_EXTENTS;
        qt[1] = 0u;  /* odd-length = false */
        put_u16_le(qt + 2, 3u);
        put_u32_le(qt + 4, font_xid);
        put_u16_le(qt + 8, 0x0041u);   /* 'A' */
        put_u16_le(qt + 10, 0x0042u);  /* 'B' */
        CHECK(send_all(fd, qt, sizeof(qt)) == 0,
              "x11 client: send QueryTextExtents: %s", strerror(errno));

        uint8_t reply[32];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: QueryTextExtents reply timed out");
        CHECK(reply[0] == 1u,
              "x11 client: QueryTextExtents byte0=%u", reply[0]);
        CHECK(reply[1] == 0u,
              "x11 client: QueryTextExtents draw-direction=%u (expected 0)",
              reply[1]);
        uint32_t length = get_u32_le(reply + 4);
        CHECK(length == 0u,
              "x11 client: QueryTextExtents length=%u (expected 0)",
              (unsigned)length);
        bool all_zero = true;
        for (size_t i = 8u; i < sizeof(reply); i++) {
            if (reply[i] != 0u) { all_zero = false; break; }
        }
        CHECK(all_zero,
              "x11 client: QueryTextExtents metrics not all zero");
        fprintf(stderr,
                "x11 client: QueryTextExtents -> 32-byte zeroed reply ok\n");
    }

    /* 7. ListFonts with pattern "*". pattern_len = 1, padded to 4.
     * Total = 12 bytes, length = 3. Reply must be 32 bytes with
     * names_len at bytes 8..9 = 0 and length = 0. */
    {
        uint8_t lf[12];
        memset(lf, 0, sizeof(lf));
        lf[0] = X11_REQ_LIST_FONTS;
        put_u16_le(lf + 2, 3u);
        put_u16_le(lf + 4, 100u);  /* max-names */
        put_u16_le(lf + 6, 1u);    /* pattern_len */
        lf[8] = (uint8_t)'*';
        CHECK(send_all(fd, lf, sizeof(lf)) == 0,
              "x11 client: send ListFonts: %s", strerror(errno));

        uint8_t reply[32];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: ListFonts reply timed out");
        CHECK(reply[0] == 1u, "x11 client: ListFonts byte0=%u", reply[0]);
        uint32_t length = get_u32_le(reply + 4);
        CHECK(length == 0u,
              "x11 client: ListFonts length=%u (expected 0)",
              (unsigned)length);
        uint16_t names_len = get_u16_le(reply + 8);
        CHECK(names_len == 0u,
              "x11 client: ListFonts names_len=%u (expected 0)",
              (unsigned)names_len);
        fprintf(stderr,
                "x11 client: ListFonts \"*\" -> 0 names ok\n");
    }

    /* 8. ListFontsWithInfo with pattern "*". Same payload as ListFonts.
     * Reply must be a 60-byte terminator with byte 1 = 0 (name_len),
     * length = 13, all metrics zero. */
    {
        uint8_t lf[12];
        memset(lf, 0, sizeof(lf));
        lf[0] = X11_REQ_LIST_FONTS_WITH_INFO;
        put_u16_le(lf + 2, 3u);
        put_u16_le(lf + 4, 100u);
        put_u16_le(lf + 6, 1u);
        lf[8] = (uint8_t)'*';
        CHECK(send_all(fd, lf, sizeof(lf)) == 0,
              "x11 client: send ListFontsWithInfo: %s", strerror(errno));

        uint8_t reply[60];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: ListFontsWithInfo reply timed out");
        CHECK(reply[0] == 1u,
              "x11 client: ListFontsWithInfo byte0=%u", reply[0]);
        CHECK(reply[1] == 0u,
              "x11 client: ListFontsWithInfo name_len=%u (expected 0)",
              reply[1]);
        uint32_t length = get_u32_le(reply + 4);
        CHECK(length == 13u,
              "x11 client: ListFontsWithInfo length=%u (expected 13)",
              (unsigned)length);
        bool all_zero = true;
        for (size_t i = 8u; i < sizeof(reply); i++) {
            if (reply[i] != 0u) { all_zero = false; break; }
        }
        CHECK(all_zero,
              "x11 client: ListFontsWithInfo metrics not all zero");
        fprintf(stderr,
                "x11 client: ListFontsWithInfo \"*\" -> terminator ok\n");
    }

    /* 9. GetFontPath. Total = 4 bytes (header only). Reply must be 32
     * bytes with npaths at bytes 8..9 = 0. */
    {
        uint8_t gf[4];
        memset(gf, 0, sizeof(gf));
        gf[0] = X11_REQ_GET_FONT_PATH;
        put_u16_le(gf + 2, 1u);
        CHECK(send_all(fd, gf, sizeof(gf)) == 0,
              "x11 client: send GetFontPath: %s", strerror(errno));

        uint8_t reply[32];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: GetFontPath reply timed out");
        CHECK(reply[0] == 1u, "x11 client: GetFontPath byte0=%u", reply[0]);
        uint16_t npaths = get_u16_le(reply + 8);
        CHECK(npaths == 0u,
              "x11 client: GetFontPath npaths=%u (expected 0)",
              (unsigned)npaths);
        fprintf(stderr, "x11 client: GetFontPath -> 0 paths ok\n");
    }

    /* 10. SetFontPath with nfonts = 0. Total = 8 bytes, length = 2.
     * No reply. */
    {
        uint8_t sf[8];
        memset(sf, 0, sizeof(sf));
        sf[0] = X11_REQ_SET_FONT_PATH;
        put_u16_le(sf + 2, 2u);
        put_u16_le(sf + 4, 0u);  /* nfonts */
        CHECK(send_all(fd, sf, sizeof(sf)) == 0,
              "x11 client: send SetFontPath: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0, "x11 client: SetFontPath errored");
        fprintf(stderr, "x11 client: SetFontPath nfonts=0 ok\n");
    }

    /* 11. ImageText8 "hello". string_len = 5, text_padded = 8. Total
     * = 16 + 8 = 24 bytes, length = 6. No reply. */
    {
        const char text[] = "hello";
        size_t slen = sizeof(text) - 1u;  /* 5 */
        size_t padded = (slen + 3u) & ~(size_t)3u;  /* 8 */
        size_t total = 16u + padded;  /* 24 */
        uint8_t *it = calloc(1, total);
        CHECK(it != NULL, "x11 client: out of memory");
        it[0] = X11_REQ_IMAGE_TEXT_8;
        it[1] = (uint8_t)slen;
        put_u16_le(it + 2, (uint16_t)(total / 4u));
        put_u32_le(it + 4, wid);
        put_u32_le(it + 8, gc_xid);
        put_u16_le(it + 12, 1u);  /* x */
        put_u16_le(it + 14, 1u);  /* y */
        memcpy(it + 16, text, slen);
        CHECK(send_all(fd, it, total) == 0,
              "x11 client: send ImageText8: %s", strerror(errno));
        free(it);
        CHECK(expect_no_error(fd) == 0, "x11 client: ImageText8 errored");
        fprintf(stderr, "x11 client: ImageText8 \"hello\" ok\n");
    }

    /* 12. PolyText8 with one TEXTITEM8: len=2, delta=0, chars="hi".
     * Item bytes = 1+1+2 = 4. Total = 16 + 4 = 20 bytes, length = 5.
     * No reply. */
    {
        uint8_t pt[20];
        memset(pt, 0, sizeof(pt));
        pt[0] = X11_REQ_POLY_TEXT_8;
        put_u16_le(pt + 2, 5u);
        put_u32_le(pt + 4, wid);
        put_u32_le(pt + 8, gc_xid);
        put_u16_le(pt + 12, 1u);  /* x */
        put_u16_le(pt + 14, 1u);  /* y */
        /* TEXTITEM8 at offset 16: len=2, delta=0, chars="hi" */
        pt[16] = 2u;
        pt[17] = 0u;
        pt[18] = (uint8_t)'h';
        pt[19] = (uint8_t)'i';
        CHECK(send_all(fd, pt, sizeof(pt)) == 0,
              "x11 client: send PolyText8: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0, "x11 client: PolyText8 errored");
        fprintf(stderr, "x11 client: PolyText8 \"hi\" ok\n");
    }

    /* 13. CloseFont. Total = 8 bytes, length = 2. No reply. */
    {
        uint8_t cf[8];
        memset(cf, 0, sizeof(cf));
        cf[0] = X11_REQ_CLOSE_FONT;
        put_u16_le(cf + 2, 2u);
        put_u32_le(cf + 4, font_xid);
        CHECK(send_all(fd, cf, sizeof(cf)) == 0,
              "x11 client: send CloseFont: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0, "x11 client: CloseFont errored");
    }
    fprintf(stderr, "x11 client: CloseFont font=0x%x ok\n",
            (unsigned)font_xid);

    /* 14. CloseFont again. Per X11, closing an already-closed font
     * is not an error; the bridge must send no error. */
    {
        uint8_t cf[8];
        memset(cf, 0, sizeof(cf));
        cf[0] = X11_REQ_CLOSE_FONT;
        put_u16_le(cf + 2, 2u);
        put_u32_le(cf + 4, font_xid);
        CHECK(send_all(fd, cf, sizeof(cf)) == 0,
              "x11 client: send CloseFont(2): %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0,
              "x11 client: CloseFont(2) errored (expected silent no-op)");
    }
    fprintf(stderr, "x11 client: CloseFont(2) silent no-op ok\n");

    /* 15. QueryFont on a non-existent font. Must return BadFont. */
    {
        uint8_t qf[8];
        memset(qf, 0, sizeof(qf));
        qf[0] = X11_REQ_QUERY_FONT;
        put_u16_le(qf + 2, 2u);
        put_u32_le(qf + 4, bad_font_xid);
        CHECK(send_all(fd, qf, sizeof(qf)) == 0,
              "x11 client: send QueryFont(bad): %s", strerror(errno));
        int err = expect_no_error(fd);
        CHECK(err == (int)X11_ERR_FONT,
              "x11 client: QueryFont(bad) err=%d (expected %d BadFont)",
              err, (int)X11_ERR_FONT);
        fprintf(stderr,
                "x11 client: QueryFont font=0x%x -> BadFont ok\n",
                (unsigned)bad_font_xid);
    }

    /* 16. Clean close. Drain any trailing events. */
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

    snprintf(isz_sock, sizeof(isz_sock), "/tmp/.ishizue-x11fonts-%d",
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
        printf("test_x11_fonts: PASS\n");
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
