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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* test_x11_colormap.c: W10-B integration test.
 *
 * Spawns the x11bridge binary against a headless Ishizue server
 * running in the test process, then connects to the bridge's UDS as a
 * raw X11 client (no libX11) and exercises the five W10-B colormap
 * opcodes end-to-end:
 *
 *   1. CreateWindow on root (anchor for the colormap).
 *   2. CreateColormap(cmap, window, visual=0, alloc=AllocNone).
 *   3. AllocColor(cmap, R=255, G=0, B=0): reply has red=255 and
 *      pixel=0x00FF0000.
 *   4. QueryColors(cmap, [0x00FF0000]): reply has red=255, green=0,
 *      blue=0.
 *   5. LookupColor(cmap, "red"): reply has exact RGB = screen RGB =
 *      (0, 0, 0) (the bridge ships no Xrgb.txt).
 *   6. FreeColormap(cmap): no error.
 *   7. FreeColormap(cmap) again: BadColormap error.
 *   8. Close cleanly.
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
#define X11_REQ_DESTROY_WINDOW    4u
#define X11_REQ_CREATE_COLORMAP   78u
#define X11_REQ_FREE_COLORMAP     79u
#define X11_REQ_ALLOC_COLOR       84u
#define X11_REQ_QUERY_COLORS      91u
#define X11_REQ_LOOKUP_COLOR      92u

#define X11_ERR_COLORMAP          12u

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

/* X11 client side: end-to-end exercise of the five W10-B opcodes. */
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

    /* 2. CreateWindow on root. The colormap references this window. */
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
        CHECK(expect_no_error(fd) == 0, "x11 client: CreateWindow errored");
    }
    fprintf(stderr, "x11 client: CreateWindow wid=0x%x accepted\n",
            (unsigned)wid);

    /* 3. CreateColormap(cmap, window, visual=0, alloc=AllocNone).
     * Total request 16 bytes (length=4). */
    uint32_t cmap_xid = rid_base + 16u;
    {
        uint8_t cc[16];
        memset(cc, 0, sizeof(cc));
        cc[0] = X11_REQ_CREATE_COLORMAP;
        cc[1] = 0u;  /* AllocNone */
        put_u16_le(cc + 2, 4u);
        put_u32_le(cc + 4, cmap_xid);
        put_u32_le(cc + 8, wid);
        put_u32_le(cc + 12, 0u);  /* visual: CopyFromParent */
        CHECK(send_all(fd, cc, sizeof(cc)) == 0,
              "x11 client: send CreateColormap: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0,
              "x11 client: CreateColormap errored");
    }
    fprintf(stderr, "x11 client: CreateColormap cmap=0x%x ok\n",
            (unsigned)cmap_xid);

    /* 4. AllocColor(cmap, R=255, G=0, B=0). Reply must have red=255,
     * green=0, blue=0, pixel=0x00FF0000. */
    {
        uint8_t ac[16];
        memset(ac, 0, sizeof(ac));
        ac[0] = X11_REQ_ALLOC_COLOR;
        put_u16_le(ac + 2, 4u);
        put_u32_le(ac + 4, cmap_xid);
        put_u16_le(ac + 8,  255u);  /* red */
        put_u16_le(ac + 10, 0u);    /* green */
        put_u16_le(ac + 12, 0u);    /* blue */
        CHECK(send_all(fd, ac, sizeof(ac)) == 0,
              "x11 client: send AllocColor: %s", strerror(errno));

        uint8_t reply[32];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: AllocColor reply timed out");
        CHECK(reply[0] == 1u, "x11 client: AllocColor byte0=%u", reply[0]);
        uint16_t r = get_u16_le(reply + 8);
        uint16_t g = get_u16_le(reply + 10);
        uint16_t b = get_u16_le(reply + 12);
        uint32_t pixel = get_u32_le(reply + 16);
        CHECK(r == 255u,
              "x11 client: AllocColor red=%u (expected 255)", (unsigned)r);
        CHECK(g == 0u,
              "x11 client: AllocColor green=%u (expected 0)", (unsigned)g);
        CHECK(b == 0u,
              "x11 client: AllocColor blue=%u (expected 0)", (unsigned)b);
        CHECK(pixel == 0x00FF0000u,
              "x11 client: AllocColor pixel=0x%x (expected 0x00FF0000)",
              (unsigned)pixel);
        fprintf(stderr,
                "x11 client: AllocColor (255,0,0) -> pixel=0x%x ok\n",
                (unsigned)pixel);
    }

    /* 5. QueryColors(cmap, [0x00FF0000]). Reply must have n=1 and
     * color[0] = (red=255, green=0, blue=0). */
    {
        uint8_t qc[12];
        memset(qc, 0, sizeof(qc));
        qc[0] = X11_REQ_QUERY_COLORS;
        put_u16_le(qc + 2, 3u);  /* length = 3 units = 12 bytes */
        put_u32_le(qc + 4, cmap_xid);
        put_u32_le(qc + 8, 0x00FF0000u);  /* one pixel */
        CHECK(send_all(fd, qc, sizeof(qc)) == 0,
              "x11 client: send QueryColors: %s", strerror(errno));

        uint8_t hdr[32];
        CHECK(read_n(fd, hdr, sizeof(hdr), 5000) == 0,
              "x11 client: QueryColors header timed out");
        CHECK(hdr[0] == 1u, "x11 client: QueryColors byte0=%u", hdr[0]);
        uint16_t n = get_u16_le(hdr + 8);
        CHECK(n == 1u,
              "x11 client: QueryColors n=%u (expected 1)", (unsigned)n);
        uint8_t color[8];
        CHECK(read_n(fd, color, sizeof(color), 5000) == 0,
              "x11 client: QueryColors color[0] timed out");
        uint16_t r = get_u16_le(color + 0);
        uint16_t g = get_u16_le(color + 2);
        uint16_t b = get_u16_le(color + 4);
        CHECK(r == 255u,
              "x11 client: QueryColors red=%u (expected 255)", (unsigned)r);
        CHECK(g == 0u,
              "x11 client: QueryColors green=%u (expected 0)", (unsigned)g);
        CHECK(b == 0u,
              "x11 client: QueryColors blue=%u (expected 0)", (unsigned)b);
        fprintf(stderr,
                "x11 client: QueryColors [0x00FF0000] -> (255,0,0) ok\n");
    }

    /* 6. LookupColor(cmap, "red"). Reply must be 32 bytes with
     * exact RGB = screen RGB = (0, 0, 0). The bridge ships no
     * Xrgb.txt; the v1 lookup returns black for every name. */
    {
        const char name[] = "red";
        size_t nlen = sizeof(name) - 1u;
        size_t padded = (nlen + 3u) & ~(size_t)3u;
        size_t total = 12u + padded;
        uint8_t *lc = calloc(1, total);
        CHECK(lc != NULL, "x11 client: out of memory");
        lc[0] = X11_REQ_LOOKUP_COLOR;
        put_u16_le(lc + 2, (uint16_t)(total / 4u));
        put_u32_le(lc + 4, cmap_xid);
        put_u16_le(lc + 8, (uint16_t)nlen);
        memcpy(lc + 12, name, nlen);
        CHECK(send_all(fd, lc, total) == 0,
              "x11 client: send LookupColor: %s", strerror(errno));
        free(lc);

        uint8_t reply[32];
        CHECK(read_n(fd, reply, sizeof(reply), 5000) == 0,
              "x11 client: LookupColor reply timed out");
        CHECK(reply[0] == 1u, "x11 client: LookupColor byte0=%u", reply[0]);
        uint16_t exact_r = get_u16_le(reply + 8);
        uint16_t exact_g = get_u16_le(reply + 10);
        uint16_t exact_b = get_u16_le(reply + 12);
        uint16_t screen_r = get_u16_le(reply + 14);
        uint16_t screen_g = get_u16_le(reply + 16);
        uint16_t screen_b = get_u16_le(reply + 18);
        CHECK(exact_r == 0u && exact_g == 0u && exact_b == 0u,
              "x11 client: LookupColor exact=(%u,%u,%u) expected (0,0,0)",
              (unsigned)exact_r, (unsigned)exact_g, (unsigned)exact_b);
        CHECK(screen_r == 0u && screen_g == 0u && screen_b == 0u,
              "x11 client: LookupColor screen=(%u,%u,%u) expected (0,0,0)",
              (unsigned)screen_r, (unsigned)screen_g, (unsigned)screen_b);
        fprintf(stderr,
                "x11 client: LookupColor \"red\" -> (0,0,0) ok\n");
    }

    /* 7. FreeColormap(cmap). No error. */
    {
        uint8_t fc[8];
        memset(fc, 0, sizeof(fc));
        fc[0] = X11_REQ_FREE_COLORMAP;
        put_u16_le(fc + 2, 2u);
        put_u32_le(fc + 4, cmap_xid);
        CHECK(send_all(fd, fc, sizeof(fc)) == 0,
              "x11 client: send FreeColormap: %s", strerror(errno));
        CHECK(expect_no_error(fd) == 0,
              "x11 client: FreeColormap errored");
    }
    fprintf(stderr, "x11 client: FreeColormap cmap=0x%x ok\n",
            (unsigned)cmap_xid);

    /* 8. FreeColormap(cmap) again. Must get BadColormap. */
    {
        uint8_t fc[8];
        memset(fc, 0, sizeof(fc));
        fc[0] = X11_REQ_FREE_COLORMAP;
        put_u16_le(fc + 2, 2u);
        put_u32_le(fc + 4, cmap_xid);
        CHECK(send_all(fd, fc, sizeof(fc)) == 0,
              "x11 client: send FreeColormap(2): %s", strerror(errno));
        int err = expect_no_error(fd);
        CHECK(err == (int)X11_ERR_COLORMAP,
              "x11 client: FreeColormap(2) err=%d (expected %d BadColormap)",
              err, (int)X11_ERR_COLORMAP);
        fprintf(stderr,
                "x11 client: FreeColormap(2) -> BadColormap ok\n");
    }

    /* 9. Clean close. Drain any trailing events. */
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

    snprintf(isz_sock, sizeof(isz_sock), "/tmp/.ishizue-x11cmap-%d",
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
        printf("test_x11_colormap: PASS\n");
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
