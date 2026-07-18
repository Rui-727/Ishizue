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

/* x11_client.c: per-X11-client state and request parser. */

#define _GNU_SOURCE 1  /* accept4, MSG_NOSIGNAL */

#include "x11_client.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "translation.h"

static void xc_log(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void xc_log(const char *level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "x11bridge/x11: %s: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

struct x11_client *x11_client_create(int fd) {
    struct x11_client *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        close(fd);
        return NULL;
    }
    c->fd          = fd;
    c->setup_done  = false;
    c->byte_order  = X11_BYTE_ORDER_LSB;
    c->sequence    = 1;
    c->have        = 0;
    return c;
}

void x11_client_destroy(struct x11_client *c) {
    if (c == NULL) return;
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    free(c);
}

int x11_client_fd(const struct x11_client *c) {
    return c ? c->fd : -1;
}

uint16_t x11_client_next_sequence(struct x11_client *c) {
    uint16_t s = c->sequence;
    c->sequence = (uint16_t)(c->sequence + 1u);
    return s;
}

struct x11_window *x11_client_first_mapped(struct x11_client *c) {
    for (size_t i = 0; i < X11_CLIENT_MAX_WIN; i++) {
        if (c->windows[i].in_use && c->windows[i].mapped) {
            return &c->windows[i];
        }
    }
    return NULL;
}

static struct x11_window *x11_client_find_window(struct x11_client *c,
                                                 uint32_t x11_id) {
    for (size_t i = 0; i < X11_CLIENT_MAX_WIN; i++) {
        if (c->windows[i].in_use && c->windows[i].x11_id == x11_id) {
            return &c->windows[i];
        }
    }
    return NULL;
}

static struct x11_window *x11_client_new_window(struct x11_client *c,
                                                uint32_t x11_id) {
    for (size_t i = 0; i < X11_CLIENT_MAX_WIN; i++) {
        if (!c->windows[i].in_use) {
            memset(&c->windows[i], 0, sizeof(c->windows[i]));
            c->windows[i].in_use   = true;
            c->windows[i].x11_id   = x11_id;
            return &c->windows[i];
        }
    }
    return NULL;  /* table full */
}

/* ------------------------------------------------------------------ */
/* Connection setup                                                    */
/* ------------------------------------------------------------------ */

/* Read the setup_request (12 bytes + auth name/data, padded to 4)
 * and reply with a minimal setup_success. Returns 0 on success, -1
 * on EOF / hard error / protocol violation. */
static int x11_client_do_setup(struct x11_client *c) {
    xc_log("debug", "do_setup: entry, fd=%d", c->fd);
    /* The client sends at least 12 bytes. We read those first to
     * learn the byte order and the auth name/data lengths, then read
     * the padded auth fields. */
    uint8_t hdr[12];
    size_t off = 0;
    while (off < sizeof(hdr)) {
        ssize_t n = recv(c->fd, hdr + off, sizeof(hdr) - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            xc_log("error", "setup recv: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            xc_log("info", "setup: client closed before sending request");
            return -1;
        }
        off += (size_t)n;
    }

    c->byte_order = hdr[0];
    if (c->byte_order != X11_BYTE_ORDER_LSB &&
        c->byte_order != X11_BYTE_ORDER_MSB) {
        xc_log("error", "setup: bad byte order 0x%02x", hdr[0]);
        return -1;
    }

    uint16_t auth_name_len = x11_get_u16(hdr + 6, c->byte_order);
    uint16_t auth_data_len = x11_get_u16(hdr + 8, c->byte_order);
    size_t auth_name_padded = x11_pad4(auth_name_len);
    size_t auth_data_padded = x11_pad4(auth_data_len);
    size_t auth_total = auth_name_padded + auth_data_padded;

    /* Drain the auth bytes (we accept any auth, so we discard them).
     * The bridge is a privileged client of Ishizue; X11 clients
     * connecting to the bridge do not need to authenticate. */
    uint8_t sink[256];
    while (auth_total > 0) {
        size_t want = auth_total > sizeof(sink) ? sizeof(sink) : auth_total;
        ssize_t n = recv(c->fd, sink, want, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        auth_total -= (size_t)n;
    }

    uint16_t proto_major = x11_get_u16(hdr + 2, c->byte_order);
    uint16_t proto_minor = x11_get_u16(hdr + 4, c->byte_order);
    xc_log("info", "setup: byte_order=0x%02x proto=%u.%u auth=%u/%u",
           c->byte_order, (unsigned)proto_major, (unsigned)proto_minor,
           (unsigned)auth_name_len, (unsigned)auth_data_len);

    uint8_t reply[88];
    size_t rlen = x11_build_setup_success(reply, sizeof(reply),
                                          c->byte_order,
                                          proto_major, proto_minor);
    if (rlen == 0) {
        xc_log("error", "setup: build_setup_success returned 0");
        return -1;
    }

    size_t sent = 0;
    while (sent < rlen) {
        ssize_t n = send(c->fd, reply + sent, rlen - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            xc_log("error", "setup send: %s", strerror(errno));
            return -1;
        }
        sent += (size_t)n;
    }

    c->setup_done = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Request dispatch                                                    */
/* ------------------------------------------------------------------ */

/* ConfigureWindow value-mask bits (X11 protocol spec). */
#define X11_CFG_MASK_X          0x0001u
#define X11_CFG_MASK_Y          0x0002u
#define X11_CFG_MASK_WIDTH      0x0004u
#define X11_CFG_MASK_HEIGHT     0x0008u
#define X11_CFG_MASK_BORDER_W   0x0010u
#define X11_CFG_MASK_SIBLING    0x0020u
#define X11_CFG_MASK_STACK_MODE 0x0040u

/* Parse and dispatch one request. The request lives in `buf` and is
 * `total` bytes long (>= 4). Returns 0 on success, -1 on a malformed
 * request that should disconnect the client. */
static int x11_client_dispatch_request(struct x11_client *c,
                                       struct isz_client *isz,
                                       const uint8_t *buf, size_t total) {
    if (total < 4) return -1;
    uint8_t opcode = buf[0];
    uint16_t length = x11_get_u16(buf + 2, c->byte_order);
    size_t stated = (size_t)length * 4u;
    if (stated != total) {
        /* Caller ensures the two match; if not, something is wrong. */
        xc_log("warn", "request: length mismatch stated=%zu total=%zu",
               stated, total);
        return -1;
    }

    const uint8_t *payload = buf + 4;
    size_t payload_len = total - 4;

    switch (opcode) {
    case X11_REQ_CREATE_WINDOW: {
        /* CreateWindow payload (first 24 bytes):
         *   u32 parent, u32 window, i16 x, i16 y, u16 w, u16 h,
         *   u16 border_width, u16 class, u32 visual, u32 value_mask */
        if (payload_len < 24) {
            xc_log("warn", "CreateWindow: short payload %zu", payload_len);
            break;
        }
        uint32_t parent = x11_get_u32(payload, c->byte_order);
        uint32_t window = x11_get_u32(payload + 4, c->byte_order);
        int32_t  x      = (int32_t)(int16_t)x11_get_u16(payload + 8,
                                                        c->byte_order);
        int32_t  y      = (int32_t)(int16_t)x11_get_u16(payload + 10,
                                                        c->byte_order);
        int32_t  w      = (int32_t)x11_get_u16(payload + 12, c->byte_order);
        int32_t  h      = (int32_t)x11_get_u16(payload + 14, c->byte_order);

        /* The bridge only tracks top-level windows (parent == root).
         * The root window id is 1 per our setup_success. */
        if (parent != 1u) {
            xc_log("debug",
                   "CreateWindow: window=%u parent=%u (not top-level, skip)",
                   (unsigned)window, (unsigned)parent);
            break;
        }
        struct x11_window *win = x11_client_new_window(c, window);
        if (win == NULL) {
            xc_log("warn", "CreateWindow: window table full");
            break;
        }
        win->x = x;
        win->y = y;
        win->w = w;
        win->h = h;
        translation_on_x11_create_window(isz, c, win, x, y, w, h);
        break;
    }
    case X11_REQ_MAP_WINDOW: {
        if (payload_len < 4) {
            xc_log("warn", "MapWindow: short payload %zu", payload_len);
            break;
        }
        uint32_t window = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, window);
        if (win == NULL) {
            xc_log("debug", "MapWindow: window=%u not tracked",
                   (unsigned)window);
            break;
        }
        win->mapped = true;
        translation_on_x11_map_window(isz, c, win);
        break;
    }
    case X11_REQ_UNMAP_WINDOW: {
        if (payload_len < 4) break;
        uint32_t window = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, window);
        if (win != NULL) {
            win->mapped = false;
            translation_on_x11_unmap_window(isz, c, win);
        }
        break;
    }
    case X11_REQ_CONFIGURE_WINDOW: {
        if (payload_len < 4) break;
        uint32_t window = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, window);
        if (win == NULL) {
            xc_log("debug", "ConfigureWindow: window=%u not tracked",
                   (unsigned)window);
            break;
        }
        if (payload_len < 8) break;
        uint16_t mask = x11_get_u16(payload + 4, c->byte_order);
        /* value-list follows the 2-byte mask + 2 bytes padding. Each
         * present value is 4 bytes, in the order: x, y, width, height,
         * border-width, sibling, stack-mode. */
        const uint8_t *vp = payload + 8;
        size_t remaining = payload_len - 8;
        int32_t nx = win->x, ny = win->y, nw = win->w, nh = win->h;
        const uint16_t bits[] = {
            X11_CFG_MASK_X, X11_CFG_MASK_Y, X11_CFG_MASK_WIDTH,
            X11_CFG_MASK_HEIGHT, X11_CFG_MASK_BORDER_W,
            X11_CFG_MASK_SIBLING, X11_CFG_MASK_STACK_MODE,
        };
        int32_t *fields[] = { &nx, &ny, &nw, &nh, NULL, NULL, NULL };
        for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
            if ((mask & bits[i]) == 0) continue;
            if (remaining < 4) break;
            if (fields[i] != NULL) {
                *fields[i] = (int32_t)x11_get_u32(vp, c->byte_order);
            }
            vp += 4;
            remaining -= 4;
        }
        win->x = nx; win->y = ny; win->w = nw; win->h = nh;
        translation_on_x11_configure_window(isz, c, win, nx, ny, nw, nh);
        break;
    }
    case X11_REQ_DESTROY_WINDOW: {
        if (payload_len < 4) break;
        uint32_t window = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, window);
        if (win != NULL) {
            translation_on_x11_destroy_window(isz, c, win);
            win->in_use = false;
        }
        break;
    }
    default:
        /* The bridge does not reply to most requests. Real X11 clients
         * that block on a reply (InternAtom, GetInputFocus, etc.) will
         * stall here. See README.md. */
        xc_log("debug", "request: opcode=%u len=%zu (no handler)",
               (unsigned)opcode, total);
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Read + parse loop                                                   */
/* ------------------------------------------------------------------ */

int x11_client_drain(struct x11_client *c, struct isz_client *isz) {
    if (c == NULL) return -1;

    if (!c->setup_done) {
        if (x11_client_do_setup(c) < 0) return -1;
        return 0;  /* one setup per client; further requests next call */
    }

    /* Top up the buffer. */
    for (;;) {
        size_t cap = sizeof(c->buf) - c->have;
        if (cap == 0) {
            /* Buffer full but no complete request was parsable in a
             * previous call. Bail before the next recv overwrites. */
            xc_log("error", "recv buffer full with no parseable request");
            return -1;
        }
        ssize_t n = recv(c->fd, c->buf + c->have, cap, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            xc_log("info", "recv error: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            xc_log("info", "client closed");
            return -1;
        }
        c->have += (size_t)n;
        /* Loop once more in case the kernel coalesced multiple
         * sendmsg calls. Stop when recv returns EAGAIN or the buffer
         * is full. */
        if (c->have == sizeof(c->buf)) break;
        /* If recv would block, exit the loop. We do another iteration
         * only if we got data and there's still room. */
        break;
    }

    /* Parse as many complete requests as the buffer holds. */
    size_t consumed = 0;
    while (c->have - consumed >= 4) {
        const uint8_t *p = c->buf + consumed;
        uint16_t length = x11_get_u16(p + 2, c->byte_order);
        if (length < 1u) {
            xc_log("error", "request: zero length field");
            return -1;
        }
        size_t total = (size_t)length * 4u;
        if (total > sizeof(c->buf)) {
            xc_log("error", "request: length %zu exceeds buffer", total);
            return -1;
        }
        if (c->have - consumed < total) {
            /* Incomplete; wait for more bytes. */
            break;
        }
        if (x11_client_dispatch_request(c, isz, p, total) < 0) {
            return -1;
        }
        consumed += total;
    }

    /* Shift the remainder down. */
    if (consumed > 0) {
        size_t rest = c->have - consumed;
        if (rest > 0) {
            memmove(c->buf, c->buf + consumed, rest);
        }
        c->have = rest;
    }
    return 0;
}

int x11_client_send_event(struct x11_client *c,
                          const struct x11_event_32 *ev) {
    if (c == NULL || ev == NULL) return -1;
    size_t off = 0;
    while (off < sizeof(*ev)) {
        ssize_t n = send(c->fd, (const uint8_t *)ev + off,
                         sizeof(*ev) - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            xc_log("warn", "send_event: %s", strerror(errno));
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}
