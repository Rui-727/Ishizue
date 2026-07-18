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

/* x11_client.c: per-X11-client state and request parser.
 *
 * W7-C: completes the X11 connection setup handshake for real,
 * parses CreateWindow's full value-list, answers GetGeometry, and
 * silently drops other opcodes (with a debug log) so a client that
 * probes extensions does not stall. */

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

/* Monotonic counter that picks a fresh per-client XID slot. Wraps
 * after X11_XID_MASK+1 slots so the base stays inside the 32-bit
 * range. In practice the bridge handles tens of clients per session,
 * nowhere near the wrap. */
static uint32_t g_xid_slot = 0;

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

    /* Assign this client a slice of the 32-bit XID space. */
    uint32_t slot = g_xid_slot;
    g_xid_slot = (g_xid_slot + 1u) & 0x000003FFu;  /* cap at 1024 slots */
    c->xid_base = X11_XID_BASE_FIRST + slot * X11_XID_STRIDE;
    c->xid_mask = X11_XID_MASK;
    c->next_xid = 0;
    c->have      = 0;
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

/* Send a 32-byte buffer to the client. Returns 0 on success, -1 on
 * hard error (caller should disconnect). */
static int x11_client_send_raw(struct x11_client *c, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(c->fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            xc_log("warn", "send: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Send an X11 error event to the client. Returns 0 on success. */
static int x11_client_send_error(struct x11_client *c, uint8_t error_code,
                                 uint32_t bad_value, uint8_t major_opcode,
                                 uint8_t minor_opcode) {
    uint8_t buf[32];
    uint16_t seq = (uint16_t)(c->sequence - 1u);  /* sequence of failing request */
    x11_build_error(buf, error_code, seq, bad_value, major_opcode, minor_opcode,
                    c->byte_order);
    return x11_client_send_raw(c, buf, sizeof(buf));
}

/* ------------------------------------------------------------------ */
/* Connection setup                                                    */
/* ------------------------------------------------------------------ */

/* Read the setup_request (12 bytes + auth name/data, padded to 4)
 * and reply with a real setup_success. Returns 0 on success, -1
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
    xc_log("info", "setup: byte_order=0x%02x proto=%u.%u auth=%u/%u xid_base=0x%x xid_mask=0x%x",
           c->byte_order, (unsigned)proto_major, (unsigned)proto_minor,
           (unsigned)auth_name_len, (unsigned)auth_data_len,
           (unsigned)c->xid_base, (unsigned)c->xid_mask);

    /* Build the SetupSuccess. The size is bounded by the layout
     * (40 + 256 + 24 + 40 + 32 = 392 bytes max), so a 512-byte
     * scratch buffer is plenty. */
    uint8_t reply[512];
    size_t rlen = x11_build_setup_success(reply, sizeof(reply),
                                          c->byte_order,
                                          proto_major, proto_minor,
                                          c->xid_base, c->xid_mask,
                                          "Ishizue");
    if (rlen == 0) {
        xc_log("error", "setup: build_setup_success returned 0");
        return -1;
    }

    if (x11_client_send_raw(c, reply, rlen) < 0) {
        xc_log("error", "setup: send reply failed");
        return -1;
    }

    c->setup_done = true;
    xc_log("info", "setup: success, sent %zu-byte SetupSuccess", rlen);
    return 0;
}

/* ------------------------------------------------------------------ */
/* CreateWindow value-list parsing                                     */
/* ------------------------------------------------------------------ */

/* Walk the value-mask + value-list of a CreateWindow (or
 * ChangeWindowAttributes) request and pull out the fields the bridge
 * actually cares about: event_mask, override_redirect, cursor,
 * colormap. The rest are accepted and discarded.
 *
 * The value-list layout: one 4-byte slot per set bit in mask, in
 * order from bit 0 (background-pixmap) up to bit 14 (cursor).
 *
 * Returns true if the value-list parsed cleanly (or was absent),
 * false if the request was too short for the bits set in the mask. */
static bool parse_window_value_list(const uint8_t *vp, size_t remaining,
                                    uint32_t mask, uint8_t byte_order,
                                    struct x11_window *win) {
    /* Walk every bit position 0..14. For each set bit, consume one
     * 4-byte value. */
    for (uint32_t bit = 1u; bit != 0u && bit <= X11_CW_CURSOR; bit <<= 1u) {
        if ((mask & bit) == 0u) continue;
        if (remaining < 4u) {
            return false;
        }
        uint32_t v = x11_get_u32(vp, byte_order);
        switch (bit) {
        case X11_CW_OVERRIDE_REDIRECT:
            win->override_redirect = (v != 0u);
            break;
        case X11_CW_EVENT_MASK:
            win->event_mask = v;
            break;
        default:
            /* Accept and discard: background, border, gravity,
             * backing-store, save-under, do-not-propagate, colormap,
             * cursor. The bridge does no rendering and no cursor
             * translation yet, so these are stored nowhere. */
            break;
        }
        vp += 4u;
        remaining -= 4u;
    }
    return true;
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
    uint8_t data   = buf[1];  /* minor opcode or request-specific */
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

    /* Most requests bump the sequence counter even on no-reply paths.
     * We bump it once here so reply-bearing handlers can read the
     * pre-bump value when constructing their reply. */
    uint16_t seq = x11_client_next_sequence(c);
    (void)seq;

    switch (opcode) {
    case X11_REQ_CREATE_WINDOW: {
        /* CreateWindow payload (first 24 bytes after the 4-byte
         * header):
         *   1 depth (header byte 1 = data)
         *   4 wid
         *   4 parent
         *   2 x, 2 y, 2 w, 2 h, 2 border-width, 2 class, 4 visual,
         *   4 value-mask, then 4n value-list. */
        if (payload_len < 24) {
            xc_log("warn", "CreateWindow: short payload %zu", payload_len);
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint8_t  depth  = data;
        uint32_t wid    = x11_get_u32(payload,     c->byte_order);
        uint32_t parent = x11_get_u32(payload + 4, c->byte_order);
        int32_t  x      = (int32_t)(int16_t)x11_get_u16(payload + 8,
                                                        c->byte_order);
        int32_t  y      = (int32_t)(int16_t)x11_get_u16(payload + 10,
                                                        c->byte_order);
        int32_t  w      = (int32_t)x11_get_u16(payload + 12, c->byte_order);
        int32_t  h      = (int32_t)x11_get_u16(payload + 14, c->byte_order);
        uint16_t border = x11_get_u16(payload + 16, c->byte_order);
        uint16_t cls    = x11_get_u16(payload + 18, c->byte_order);
        uint32_t visual = x11_get_u32(payload + 20, c->byte_order);
        uint32_t vmask  = x11_get_u32(payload + 24, c->byte_order);

        /* Validate: class is 0..2, depth is 1/8/16/24/32 (we accept
         * any and let the rendering path complain later). */
        if (cls > 2u) {
            xc_log("warn", "CreateWindow: bad class %u", (unsigned)cls);
            x11_client_send_error(c, X11_ERR_VALUE, (uint32_t)cls, opcode, 0u);
            break;
        }

        /* The value-list lives at payload + 28 onward; one slot per
         * set bit in vmask. */
        const uint8_t *vp = payload + 28;
        size_t vremaining = (payload_len > 28u) ? payload_len - 28u : 0u;

        /* Pre-fill a window struct so the value-list parser can
         * populate event_mask and override_redirect. */
        struct x11_window tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.depth         = depth;
        tmp.window_class  = (uint8_t)cls;
        tmp.visual_id     = visual;
        tmp.parent_xid    = parent;
        tmp.border_width  = border;

        if (!parse_window_value_list(vp, vremaining, vmask, c->byte_order, &tmp)) {
            xc_log("warn", "CreateWindow: value-list short for mask=0x%x",
                   (unsigned)vmask);
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }

        /* Reject duplicate XIDs. */
        if (x11_client_find_window(c, wid) != NULL) {
            xc_log("warn", "CreateWindow: duplicate XID 0x%x", (unsigned)wid);
            x11_client_send_error(c, X11_ERR_IDCHOICE, wid, opcode, 0u);
            break;
        }

        struct x11_window *win = x11_client_new_window(c, wid);
        if (win == NULL) {
            xc_log("warn", "CreateWindow: window table full");
            x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
            break;
        }
        /* Copy parsed fields into the tracked window. */
        win->depth             = tmp.depth;
        win->window_class      = tmp.window_class;
        win->visual_id         = tmp.visual_id;
        win->parent_xid        = tmp.parent_xid;
        win->border_width      = tmp.border_width;
        win->event_mask        = tmp.event_mask;
        win->override_redirect = tmp.override_redirect;
        win->x = x;
        win->y = y;
        win->w = w;
        win->h = h;

        if (parent == X11_ROOT_WINDOW_ID) {
            /* Top-level window. Allocate an Ishizue surface for it.
             * The surface starts unmapped (no output, no commit) until
             * the client sends MapWindow. */
            translation_on_x11_create_window(isz, c, win, x, y, w, h);
            xc_log("info",
                   "CreateWindow: top-level wid=0x%x depth=%u class=%u "
                   "geom=(%d,%d,%dx%d) border=%u event-mask=0x%x or=%d",
                   (unsigned)wid, (unsigned)depth, (unsigned)cls,
                   (int)x, (int)y, (int)w, (int)h, (unsigned)border,
                   (unsigned)win->event_mask, (int)win->override_redirect);
        } else {
            /* Non-root parent: store as a child window. The bridge
             * does not create an Ishizue surface for child windows
             * until they are mapped (per X11 semantics, child windows
             * are clipped to parents and do not get their own
             * scanout). */
            xc_log("info",
                   "CreateWindow: child wid=0x%x parent=0x%x depth=%u "
                   "geom=(%d,%d,%dx%d) (no surface yet)",
                   (unsigned)wid, (unsigned)parent, (unsigned)depth,
                   (int)x, (int)y, (int)w, (int)h);
        }
        /* CreateWindow is a void request: no reply, errors only. */
        break;
    }
    case X11_REQ_CHANGE_WINDOW_ATTRS: {
        if (payload_len < 8) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t wid   = x11_get_u32(payload, c->byte_order);
        uint32_t vmask = x11_get_u32(payload + 4, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, wid);
        if (win == NULL) {
            x11_client_send_error(c, X11_ERR_WINDOW, wid, opcode, 0u);
            break;
        }
        const uint8_t *vp = payload + 8;
        size_t vremaining = (payload_len > 8u) ? payload_len - 8u : 0u;
        if (!parse_window_value_list(vp, vremaining, vmask, c->byte_order, win)) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        xc_log("debug",
               "ChangeWindowAttributes: wid=0x%x mask=0x%x event-mask=0x%x or=%d",
               (unsigned)wid, (unsigned)vmask,
               (unsigned)win->event_mask, (int)win->override_redirect);
        break;
    }
    case X11_REQ_GET_GEOMETRY: {
        /* GetGeometry payload: 4-byte drawable XID. */
        if (payload_len < 4) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t did = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, did);
        if (win == NULL) {
            xc_log("debug", "GetGeometry: drawable=0x%x not tracked",
                   (unsigned)did);
            x11_client_send_error(c, X11_ERR_DRAWABLE, did, opcode, 0u);
            break;
        }
        uint8_t reply[32];
        uint32_t root_xid = (win->parent_xid == X11_ROOT_WINDOW_ID)
                              ? X11_ROOT_WINDOW_ID
                              : win->parent_xid;
        x11_build_get_geometry_reply(reply, win->depth, root_xid,
                                     (int16_t)win->x, (int16_t)win->y,
                                     (uint16_t)win->w, (uint16_t)win->h,
                                     (uint16_t)win->border_width,
                                     seq, c->byte_order);
        xc_log("info",
               "GetGeometry: wid=0x%x -> depth=%u geom=(%d,%d,%dx%d) border=%u",
               (unsigned)did, (unsigned)win->depth,
               (int)win->x, (int)win->y, (int)win->w, (int)win->h,
               (unsigned)win->border_width);
        if (x11_client_send_raw(c, reply, sizeof(reply)) < 0) {
            return -1;
        }
        break;
    }
    case X11_REQ_MAP_WINDOW: {
        if (payload_len < 4) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t window = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, window);
        if (win == NULL) {
            x11_client_send_error(c, X11_ERR_WINDOW, window, opcode, 0u);
            break;
        }
        win->mapped = true;
        translation_on_x11_map_window(isz, c, win);
        xc_log("info", "MapWindow: wid=0x%x", (unsigned)window);
        break;
    }
    case X11_REQ_UNMAP_WINDOW: {
        if (payload_len < 4) break;
        uint32_t window = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, window);
        if (win != NULL) {
            win->mapped = false;
            translation_on_x11_unmap_window(isz, c, win);
            xc_log("info", "UnmapWindow: wid=0x%x", (unsigned)window);
        }
        break;
    }
    case X11_REQ_CONFIGURE_WINDOW: {
        if (payload_len < 4) break;
        uint32_t window = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, window);
        if (win == NULL) {
            x11_client_send_error(c, X11_ERR_WINDOW, window, opcode, 0u);
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
        uint16_t nborder = win->border_width;
        const uint16_t bits[] = {
            X11_CFG_MASK_X, X11_CFG_MASK_Y, X11_CFG_MASK_WIDTH,
            X11_CFG_MASK_HEIGHT, X11_CFG_MASK_BORDER_W,
            X11_CFG_MASK_SIBLING, X11_CFG_MASK_STACK_MODE,
        };
        int32_t *fields[] = { &nx, &ny, &nw, &nh, (int32_t *)&nborder, NULL, NULL };
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
        win->border_width = nborder;
        translation_on_x11_configure_window(isz, c, win, nx, ny, nw, nh);
        xc_log("info", "ConfigureWindow: wid=0x%x geom=(%d,%d,%dx%d)",
               (unsigned)window, (int)nx, (int)ny, (int)nw, (int)nh);
        break;
    }
    case X11_REQ_DESTROY_WINDOW: {
        if (payload_len < 4) break;
        uint32_t window = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, window);
        if (win != NULL) {
            translation_on_x11_destroy_window(isz, c, win);
            win->in_use = false;
            xc_log("info", "DestroyWindow: wid=0x%x", (unsigned)window);
        }
        break;
    }
    default:
        /* Unknown / unhandled opcode: silently consume the bytes and
         * log. Sending BadImplementation for every unsupported opcode
         * breaks clients that probe extensions (QueryExtension,
         * InternAtom, GetInputFocus, ...); the bridge deliberately
         * no-ops them so a client that just wants CreateWindow +
         * GetGeometry works. */
        xc_log("debug", "request: opcode=%u data=%u len=%zu (no-op)",
               (unsigned)opcode, (unsigned)data, total);
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
    return x11_client_send_raw(c, (const uint8_t *)ev, sizeof(*ev));
}
