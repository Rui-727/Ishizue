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
 * W9-B: the dispatcher handles twenty core opcodes end-to-end. The
 * ten W8-A opcodes plus GetWindowAttributes, QueryTree, GetAtomName,
 * DeleteProperty, SetSelectionOwner, GetSelectionOwner, QueryPointer,
 * SetInputFocus, CreateGC, PutImage. New per-client state: GC table,
 * selection table, keyboard focus. Each handler updates the relevant
 * state, emits the corresponding Ishizue wire message(s) via
 * translation.c, and generates the relevant X11 event(s) when the
 * client selected for them. Unknown opcodes are still silently
 * consumed (logged at debug) so a client that probes extensions does
 * not stall.
 *
 * W10-B: five colormap opcodes added (CreateColormap, FreeColormap,
 * AllocColor, QueryColors, LookupColor). Per-client state gained a
 * colormap table. The bridge does no real color allocation; AllocColor
 * echoes the requested RGB and returns a packed 0x00RRGGBB pixel,
 * QueryColors unpacks each pixel as 0x00RRGGBB, and LookupColor
 * returns exact RGB = screen RGB = (0,0,0) because the bridge ships
 * no Xrgb.txt color database. */

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
#include "x11_atoms.h"
#include "isz_client.h"

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

/* Forward decl: x11_client_destroy releases any per-window property
 * values via this helper, defined below in the properties section. */
static void x11_window_props_destroy(struct x11_window *win);

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
    /* Release any malloc'd property values and PutImage backing
     * stores on tracked windows so we do not leak on disconnect. */
    for (size_t i = 0; i < X11_CLIENT_MAX_WIN; i++) {
        if (c->windows[i].in_use) {
            x11_window_props_destroy(&c->windows[i]);
            free(c->windows[i].backing_image);
            c->windows[i].backing_image = NULL;
            c->windows[i].in_use = false;
        }
    }
    /* GC and selection tables carry no malloc'd state; the slot
     * reuse path on next CreateGC / SetSelectionOwner overwrites
     * in place. */
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

struct x11_window *x11_client_find_window_by_xid(struct x11_client *c,
                                                 uint32_t x11_id) {
    return x11_client_find_window(c, x11_id);
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
    xc_log("warn", "send_error: code=%u bad_value=0x%x opcode=%u seq=%u",
           (unsigned)error_code, (unsigned)bad_value,
           (unsigned)major_opcode, (unsigned)seq);
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

    /* Register the root window (0x100) in the per-client window
     * table. Every X11 client queries root properties (WM_PID,
     * _NET_SUPPORTED, etc.) at startup; without this, GetProperty
     * on root sends BadWindow and the client aborts. The root
     * window has no Ishizue surface; it is a logical anchor. */
    {
        struct x11_window *root = x11_client_new_window(c,
                                                        X11_ROOT_WINDOW_ID);
        if (root != NULL) {
            root->parent_xid = 0;
            root->mapped     = true;
            root->override_redirect = true;
            root->w = X11_DEFAULT_ROOT_W;
            root->h = X11_DEFAULT_ROOT_H;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* CreateWindow value-list parsing                                     */
/* ------------------------------------------------------------------ */

/* Walk the value-mask + value-list of a CreateWindow (or
 * ChangeWindowAttributes) request and pull out the fields the bridge
 * actually cares about: event_mask, override_redirect, cursor,
 * bit_gravity, win_gravity, backing_store, backing_planes,
 * backing_pixel, save_under, colormap, do_not_propagate_mask.
 * The rest are accepted and discarded.
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
        case X11_CW_BIT_GRAVITY:
            win->bit_gravity = (uint8_t)v;
            break;
        case X11_CW_WIN_GRAVITY:
            win->win_gravity = (uint8_t)v;
            break;
        case X11_CW_BACKING_STORE:
            win->backing_store = (uint8_t)v;
            break;
        case X11_CW_BACKING_PLANES:
            win->backing_planes = v;
            break;
        case X11_CW_BACKING_PIXEL:
            win->backing_pixel = v;
            break;
        case X11_CW_BG_PIXEL:
            win->background_pixel = v;
            break;
        case X11_CW_OVERRIDE_REDIRECT:
            win->override_redirect = (v != 0u);
            break;
        case X11_CW_SAVE_UNDER:
            win->save_under = (v != 0u);
            break;
        case X11_CW_EVENT_MASK:
            win->event_mask = v;
            break;
        case X11_CW_DONT_PROPAGATE:
            win->do_not_propagate_mask = (uint16_t)v;
            break;
        case X11_CW_COLORMAP:
            win->colormap = v;
            break;
        case X11_CW_CURSOR:
            /* Store the cursor XID. v == 0 means None (inherit from
             * parent); the bridge stores 0 and treats it as no cursor
             * override. Cursor translation is not wired in v1. */
            win->cursor_xid = v;
            break;
        default:
            /* Accept and discard: background-pixmap, background-pixel,
             * border-pixmap, border-pixel. The bridge does no
             * rendering yet, so these have no Ishizue action. */
            break;
        }
        vp += 4u;
        remaining -= 4u;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Per-window properties                                               */
/* ------------------------------------------------------------------ */

/* Free any malloc'd property value and mark the slot empty. Called
 * on property overwrite and on window destruction. */
static void x11_window_prop_clear(struct x11_property *p) {
    if (p == NULL) return;
    free(p->value);
    p->value = NULL;
    p->value_len = 0u;
    p->format = 0u;
    p->type = 0u;
    p->property = 0u;
    p->in_use = false;
}

/* Free all property slots on a window. Called during DestroyWindow. */
static void x11_window_props_destroy(struct x11_window *win) {
    if (win == NULL) return;
    for (size_t i = 0; i < X11_CLIENT_MAX_PROPS; i++) {
        x11_window_prop_clear(&win->props[i]);
    }
}

/* Find a property slot by atom, or NULL if not present. */
static struct x11_property *x11_window_prop_find(struct x11_window *win,
                                                 uint32_t atom) {
    for (size_t i = 0; i < X11_CLIENT_MAX_PROPS; i++) {
        if (win->props[i].in_use && win->props[i].property == atom) {
            return &win->props[i];
        }
    }
    return NULL;
}

/* Find an empty property slot, or NULL if the table is full. */
static struct x11_property *x11_window_prop_alloc(struct x11_window *win) {
    for (size_t i = 0; i < X11_CLIENT_MAX_PROPS; i++) {
        if (!win->props[i].in_use) {
            return &win->props[i];
        }
    }
    return NULL;
}

/* Free any PutImage backing store on the window. Called on
 * DestroyWindow and on a new PutImage that overwrites the prior. */
static void x11_window_backing_image_clear(struct x11_window *win) {
    if (win == NULL) return;
    free(win->backing_image);
    win->backing_image = NULL;
    win->backing_image_len = 0u;
    win->backing_image_w = 0u;
    win->backing_image_h = 0u;
    win->backing_image_depth = 0u;
    win->backing_image_format = 0u;
}

/* ------------------------------------------------------------------ */
/* Per-client graphics contexts (W9-B)                                  */
/* ------------------------------------------------------------------ */

static struct x11_gc *x11_client_find_gc(struct x11_client *c, uint32_t gc_xid) {
    for (size_t i = 0; i < X11_CLIENT_MAX_GCS; i++) {
        if (c->gcs[i].in_use && c->gcs[i].gc_xid == gc_xid) {
            return &c->gcs[i];
        }
    }
    return NULL;
}

static struct x11_gc *x11_client_alloc_gc(struct x11_client *c, uint32_t gc_xid) {
    for (size_t i = 0; i < X11_CLIENT_MAX_GCS; i++) {
        if (!c->gcs[i].in_use) {
            memset(&c->gcs[i], 0, sizeof(c->gcs[i]));
            c->gcs[i].in_use   = true;
            c->gcs[i].gc_xid   = gc_xid;
            /* X11 defaults: graphics_exposure=true, function=copy (0),
             * foreground=0, background=1, line_width=0, line_style=0
             * (Solid), fill_style=0 (Solid), subwindow_mode=0
             * (ClipByChildren), arc_mode=0 (Chord). */
            c->gcs[i].graphics_exposure = true;
            c->gcs[i].foreground        = 0u;
            c->gcs[i].background        = 1u;
            return &c->gcs[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Per-client pixmap tracking                                          */
/* ------------------------------------------------------------------ */

static struct x11_pixmap *x11_client_find_pixmap(struct x11_client *c,
                                                   uint32_t pix_xid) {
    for (size_t i = 0; i < X11_CLIENT_MAX_PIX; i++) {
        if (c->pixmaps[i].in_use && c->pixmaps[i].pixmap_xid == pix_xid) {
            return &c->pixmaps[i];
        }
    }
    return NULL;
}

static struct x11_pixmap *x11_client_alloc_pixmap(struct x11_client *c,
                                                    uint32_t pix_xid) {
    for (size_t i = 0; i < X11_CLIENT_MAX_PIX; i++) {
        if (!c->pixmaps[i].in_use) {
            memset(&c->pixmaps[i], 0, sizeof(c->pixmaps[i]));
            c->pixmaps[i].in_use     = true;
            c->pixmaps[i].pixmap_xid = pix_xid;
            return &c->pixmaps[i];
        }
    }
    return NULL;
}

/* Check if a drawable XID is valid: root window, a tracked window, or
 * a tracked pixmap. Returns the drawable type or 0 if not found. */
static int x11_client_check_drawable(struct x11_client *c, uint32_t draw) {
    if (draw == X11_ROOT_WINDOW_ID) return 1;  /* root */
    if (x11_client_find_window(c, draw) != NULL) return 2;  /* window */
    if (x11_client_find_pixmap(c, draw) != NULL) return 3;  /* pixmap */
    return 0;  /* not found */
}

/* ------------------------------------------------------------------ */
/* Per-client colormaps (W10-B)                                          */
/* ------------------------------------------------------------------ */

static struct x11_colormap *x11_client_find_colormap(struct x11_client *c,
                                                     uint32_t cmap_xid) {
    for (size_t i = 0; i < X11_CLIENT_MAX_CMAPS; i++) {
        if (c->colormaps[i].in_use &&
            c->colormaps[i].colormap_xid == cmap_xid) {
            return &c->colormaps[i];
        }
    }
    return NULL;
}

static struct x11_colormap *x11_client_alloc_colormap(struct x11_client *c,
                                                      uint32_t cmap_xid) {
    for (size_t i = 0; i < X11_CLIENT_MAX_CMAPS; i++) {
        if (!c->colormaps[i].in_use) {
            memset(&c->colormaps[i], 0, sizeof(c->colormaps[i]));
            c->colormaps[i].in_use       = true;
            c->colormaps[i].colormap_xid = cmap_xid;
            return &c->colormaps[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Per-client selection ownership (W9-B)                                */
/* ------------------------------------------------------------------ */

static struct x11_selection *x11_client_find_selection(struct x11_client *c,
                                                       uint32_t atom) {
    for (size_t i = 0; i < X11_CLIENT_MAX_SELS; i++) {
        if (c->selections[i].in_use && c->selections[i].selection_atom == atom) {
            return &c->selections[i];
        }
    }
    return NULL;
}

static struct x11_selection *x11_client_alloc_selection(struct x11_client *c,
                                                        uint32_t atom) {
    for (size_t i = 0; i < X11_CLIENT_MAX_SELS; i++) {
        if (!c->selections[i].in_use) {
            memset(&c->selections[i], 0, sizeof(c->selections[i]));
            c->selections[i].in_use         = true;
            c->selections[i].selection_atom = atom;
            return &c->selections[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Request dispatch                                                    */
/* ------------------------------------------------------------------ */

/* ConfigureWindow value-mask bits and stack-mode values live in
 * x11_proto.h (shared with the test code). */

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
         * populate event_mask and override_redirect. Set X11 defaults
         * for fields the spec mandates a non-zero default for:
         * win_gravity=NorthWest(1), backing_planes=AllPlanes(~0). */
        struct x11_window tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.depth          = depth;
        tmp.window_class   = (uint8_t)cls;
        tmp.visual_id      = visual;
        tmp.parent_xid     = parent;
        tmp.border_width   = border;
        tmp.win_gravity    = 1u;           /* NorthWestGravity */
        tmp.backing_planes = 0xFFFFFFFFu;  /* AllPlanes */

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
        win->depth                = tmp.depth;
        win->window_class         = tmp.window_class;
        win->visual_id            = tmp.visual_id;
        win->parent_xid           = tmp.parent_xid;
        win->border_width         = tmp.border_width;
        win->event_mask           = tmp.event_mask;
        win->override_redirect    = tmp.override_redirect;
        win->cursor_xid           = tmp.cursor_xid;
        win->bit_gravity          = tmp.bit_gravity;
        win->win_gravity          = tmp.win_gravity;
        win->backing_store        = tmp.backing_store;
        win->backing_planes       = tmp.backing_planes;
        win->backing_pixel        = tmp.backing_pixel;
        win->background_pixel     = tmp.background_pixel;
        win->colormap             = tmp.colormap;
        win->save_under           = tmp.save_under;
        win->do_not_propagate_mask = tmp.do_not_propagate_mask;
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
        /* CreateWindow is a void request: no reply, errors only.
         * X11 also generates a CreateNotify event to clients that
         * selected SubstructureNotify on the parent. The bridge does
         * not yet implement CreateNotify (omitted from W8-A scope). */
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
        /* ChangeWindowAttributes is a void request. The bridge does
         * not generate an event for it; clients learn about another
         * client's attribute changes via GetWindowAttributes, which
         * the bridge does not yet reply to. */
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
        if (win->mapped) {
            /* X11: MapWindow on an already-mapped window is a no-op
             * (no event generated, no error). */
            xc_log("debug", "MapWindow: wid=0x%x already mapped", (unsigned)window);
            break;
        }
        win->mapped = true;
        translation_on_x11_map_window(isz, c, win);
        xc_log("info", "MapWindow: wid=0x%x", (unsigned)window);

        /* Generate a MapNotify event. Per X11: the event is delivered
         * to clients selecting StructureNotify on the window itself,
         * or SubstructureNotify on the parent. The bridge only
         * delivers to the originating client here (single-client
         * model). The event's `event` field is the window XID for
         * StructureNotify subscribers; that is the common case. */
        {
            uint8_t evt[32];
            x11_build_map_notify(evt, win->x11_id, win->x11_id,
                                 win->override_redirect, seq, c->byte_order);
            (void)translation_deliver_event(c, evt,
                                            X11_EVMASK_STRUCTURE_NOTIFY,
                                            win->x11_id);
        }
        break;
    }
    case X11_REQ_UNMAP_WINDOW: {
        if (payload_len < 4) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t window = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, window);
        if (win == NULL) {
            /* X11: UnmapWindow on an unknown window is a Window error. */
            x11_client_send_error(c, X11_ERR_WINDOW, window, opcode, 0u);
            break;
        }
        if (!win->mapped) {
            /* X11: UnmapWindow on an already-unmapped window is a
             * no-op, but still generates an UnmapNotify event. The
             * bridge matches the no-op behavior to keep the test
             * deterministic. */
            xc_log("debug", "UnmapWindow: wid=0x%x already unmapped",
                   (unsigned)window);
            break;
        }
        win->mapped = false;
        translation_on_x11_unmap_window(isz, c, win);
        xc_log("info", "UnmapWindow: wid=0x%x", (unsigned)window);

        /* Generate UnmapNotify. `event` = the window XID (matches the
         * common case of StructureNotify on the window itself). */
        {
            uint8_t evt[32];
            x11_build_unmap_notify(evt, win->x11_id, win->x11_id,
                                   false, seq, c->byte_order);
            (void)translation_deliver_event(c, evt,
                                            X11_EVMASK_STRUCTURE_NOTIFY,
                                            win->x11_id);
        }

        /* SPEC §9: if the unmapped window had keyboard focus, clear
         * focus so the library emits an
         * ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED event. The bridge
         * does not yet track per-window focus; the wire message is
         * sent unconditionally for the headless path so the
         * focus-cleared event fires. Real focus tracking belongs in
         * a later wave. */
        if (win->has_surface && isz != NULL) {
            (void)isz_client_send_seat_set_keyboard_focus(isz,
                                                          isz->seat_id,
                                                          0u);
        }
        break;
    }
    case X11_REQ_CONFIGURE_WINDOW: {
        if (payload_len < 8) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t window = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, window);
        if (win == NULL) {
            x11_client_send_error(c, X11_ERR_WINDOW, window, opcode, 0u);
            break;
        }
        uint16_t mask = x11_get_u16(payload + 4, c->byte_order);
        /* value-list follows the 2-byte mask + 2 bytes padding. Each
         * present value is 4 bytes, in the order: x, y, width, height,
         * border-width, sibling, stack-mode. */
        const uint8_t *vp = payload + 8;
        size_t remaining = payload_len - 8;
        int32_t nx = win->x, ny = win->y, nw = win->w, nh = win->h;
        uint16_t nborder = win->border_width;
        uint32_t sibling = 0u;
        uint8_t  stack_mode = X11_STACK_ABOVE;
        bool have_stack = false;
        const uint16_t bits[] = {
            X11_CFG_MASK_X, X11_CFG_MASK_Y, X11_CFG_MASK_WIDTH,
            X11_CFG_MASK_HEIGHT, X11_CFG_MASK_BORDER_W,
            X11_CFG_MASK_SIBLING, X11_CFG_MASK_STACK_MODE,
        };
        int32_t *fields[] = {
            &nx, &ny, &nw, &nh, (int32_t *)&nborder, NULL, NULL,
        };
        for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
            if ((mask & bits[i]) == 0u) continue;
            if (remaining < 4u) break;
            uint32_t v = x11_get_u32(vp, c->byte_order);
            if (bits[i] == X11_CFG_MASK_SIBLING) {
                sibling = v;
            } else if (bits[i] == X11_CFG_MASK_STACK_MODE) {
                stack_mode = (uint8_t)v;
                have_stack = true;
            } else if (fields[i] != NULL) {
                *fields[i] = (int32_t)v;
            }
            vp += 4u;
            remaining -= 4u;
        }
        win->x = nx; win->y = ny; win->w = nw; win->h = nh;
        win->border_width = nborder;

        /* Stack-mode: the bridge maps the X11 modes onto a single
         * per-window zpos integer that the Ishizue surface carries.
         * Above = zpos+1, Below = zpos-1, TopIf = max int, BottomIf =
         * min int, Opposite = toggle. The bridge keeps the simple
         * model: only Above / Below adjust zpos; the rest pin to the
         * top / bottom of the range. */
        if (have_stack && win->has_surface && isz != NULL) {
            int32_t new_zpos = win->zpos;
            switch (stack_mode) {
            case X11_STACK_ABOVE:    new_zpos = win->zpos + 1; break;
            case X11_STACK_BELOW:    new_zpos = win->zpos - 1; break;
            case X11_STACK_TOP_IF:   new_zpos = (int32_t)0x7FFFFFFF; break;
            case X11_STACK_BOTTOM_IF:new_zpos = (int32_t)0x80000000; break;
            case X11_STACK_OPPOSITE: new_zpos = -win->zpos; break;
            default: break;
            }
            if (new_zpos != win->zpos) {
                win->zpos = new_zpos;
                (void)isz_client_send_surface_set_zpos(isz,
                                                       win->isz_surface_id,
                                                       new_zpos);
            }
        }
        (void)sibling;

        translation_on_x11_configure_window(isz, c, win, nx, ny, nw, nh);
        xc_log("info", "ConfigureWindow: wid=0x%x geom=(%d,%d,%dx%d)",
               (unsigned)window, (int)nx, (int)ny, (int)nw, (int)nh);

        /* Generate ConfigureNotify. The event's `event` field is the
         * window XID (StructureNotify subscriber on the window
         * itself). `above-sibling` is 0 (None) since the bridge does
         * not track sibling ordering yet. */
        {
            uint8_t evt[32];
            x11_build_configure_notify(evt, win->x11_id, win->x11_id, 0u,
                                       (int16_t)nx, (int16_t)ny,
                                       (uint16_t)nw, (uint16_t)nh,
                                       (uint16_t)nborder,
                                       win->override_redirect,
                                       seq, c->byte_order);
            (void)translation_deliver_event(c, evt,
                                            X11_EVMASK_STRUCTURE_NOTIFY,
                                            win->x11_id);
        }
        break;
    }
    case X11_REQ_DESTROY_WINDOW: {
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

        /* Generate DestroyNotify before we tear down the window so
         * the event carries the window's XID. The event's `event`
         * field is the window XID (StructureNotify subscriber on the
         * window itself); the parent's SubstructureNotify subscribers
         * would also receive it with event=parent, but v1 does not
         * deliver across clients. */
        {
            uint8_t evt[32];
            x11_build_destroy_notify(evt, win->x11_id, win->x11_id,
                                     seq, c->byte_order);
            (void)translation_deliver_event(c, evt,
                                            X11_EVMASK_STRUCTURE_NOTIFY,
                                            win->x11_id);
        }

        /* X11: DestroyWindow destroys all descendants recursively.
         * The bridge walks the window table and destroys any window
         * whose parent_xid chain leads to this one. */
        translation_on_x11_destroy_window(isz, c, win);
        x11_window_props_destroy(win);
        x11_window_backing_image_clear(win);
        win->in_use = false;

        /* Recurse: destroy all direct and indirect children. The
         * bridge does not generate DestroyNotify for the descendants
         * in v1 (the X11 spec requires it; this is a known gap). */
        {
            bool changed = true;
            while (changed) {
                changed = false;
                for (size_t i = 0; i < X11_CLIENT_MAX_WIN; i++) {
                    struct x11_window *cw = &c->windows[i];
                    if (!cw->in_use) continue;
                    if (cw->parent_xid == window) {
                        translation_on_x11_destroy_window(isz, c, cw);
                        x11_window_props_destroy(cw);
                        x11_window_backing_image_clear(cw);
                        cw->in_use = false;
                        changed = true;
                    }
                }
            }
        }
        xc_log("info", "DestroyWindow: wid=0x%x", (unsigned)window);
        break;
    }
    case X11_REQ_INTERN_ATOM: {
        /* InternAtom payload:
         *   1 only-if-exists (header byte 1 = data)
         *   2 length
         *   2 name length (n)
         *   2 unused
         *   n name, padded to 4 */
        bool only_if_exists = (data != 0u);
        if (payload_len < 4u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint16_t name_len = x11_get_u16(payload, c->byte_order);
        if ((size_t)name_len > payload_len - 4u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        const char *name = (const char *)(payload + 4u);
        uint32_t atom = x11_atom_intern(name, name_len, only_if_exists);
        uint8_t reply[32];
        x11_build_intern_atom_reply(reply, atom, seq, c->byte_order);
        xc_log("info", "InternAtom: name=%.*s only=%d -> atom=%u",
               (int)name_len, name, (int)only_if_exists, (unsigned)atom);
        if (x11_client_send_raw(c, reply, sizeof(reply)) < 0) {
            return -1;
        }
        break;
    }
    case X11_REQ_CHANGE_PROPERTY: {
        /* ChangeProperty payload (after the 4-byte request header):
         *   4 window
         *   4 property (atom)
         *   4 type (atom)
         *   1 format (8/16/32)
         *   3 unused
         *   4 nunits (in format units)
         *   n*padded data
         * Fixed payload is 20 bytes; data follows at offset 20. */
        uint8_t mode = data;
        if (payload_len < 20u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t wid     = x11_get_u32(payload,      c->byte_order);
        uint32_t prop    = x11_get_u32(payload + 4,  c->byte_order);
        uint32_t type    = x11_get_u32(payload + 8,  c->byte_order);
        uint8_t  fmt     = payload[12];
        uint32_t nunits  = x11_get_u32(payload + 16, c->byte_order);
        if (fmt != 8u && fmt != 16u && fmt != 32u) {
            x11_client_send_error(c, X11_ERR_VALUE, (uint32_t)fmt,
                                  opcode, 0u);
            break;
        }
        if (prop == 0u) {
            x11_client_send_error(c, X11_ERR_ATOM, prop, opcode, 0u);
            break;
        }
        size_t unit_bytes = fmt / 8u;
        size_t data_bytes = (size_t)nunits * unit_bytes;
        if (data_bytes > payload_len - 20u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        if (data_bytes > X11_PROP_MAX_BYTES) {
            x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
            break;
        }
        const uint8_t *data_buf = payload + 20u;
        struct x11_window *win = x11_client_find_window(c, wid);
        if (win == NULL) {
            x11_client_send_error(c, X11_ERR_WINDOW, wid, opcode, 0u);
            break;
        }

        struct x11_property *slot = x11_window_prop_find(win, prop);
        if (slot == NULL) {
            slot = x11_window_prop_alloc(win);
            if (slot == NULL) {
                x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
                break;
            }
        }

        if (mode == X11_PROP_MODE_REPLACE || !slot->in_use) {
            /* Replace: discard any existing value and store the new. */
            x11_window_prop_clear(slot);
            uint8_t *copy = NULL;
            if (data_bytes > 0u) {
                copy = malloc(data_bytes);
                if (copy == NULL) {
                    x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
                    break;
                }
                memcpy(copy, data_buf, data_bytes);
            }
            slot->in_use    = true;
            slot->property  = prop;
            slot->type      = type;
            slot->format    = fmt;
            slot->value_len = data_bytes;
            slot->value     = copy;
        } else if (mode == X11_PROP_MODE_PREPEND ||
                   mode == X11_PROP_MODE_APPEND) {
            /* Prepend/Append: concatenate at the format-unit
             * granularity. The new total is value_len + data_bytes;
             * cap at X11_PROP_MAX_BYTES. */
            if (slot->type != type || slot->format != fmt) {
                /* X11: type/format mismatch is a Match error. */
                x11_client_send_error(c, X11_ERR_MATCH, 0u, opcode, 0u);
                break;
            }
            size_t new_len = slot->value_len + data_bytes;
            if (new_len > X11_PROP_MAX_BYTES) {
                x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
                break;
            }
            uint8_t *grown = realloc(slot->value, new_len);
            if (grown == NULL && new_len > 0u) {
                x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
                break;
            }
            slot->value = grown;
            if (mode == X11_PROP_MODE_PREPEND) {
                memmove(slot->value + data_bytes, slot->value,
                        slot->value_len);
                memcpy(slot->value, data_buf, data_bytes);
            } else {  /* Append */
                memcpy(slot->value + slot->value_len, data_buf, data_bytes);
            }
            slot->value_len = new_len;
        } else {
            x11_client_send_error(c, X11_ERR_VALUE, (uint32_t)mode,
                                  opcode, 0u);
            break;
        }

        xc_log("info",
               "ChangeProperty: wid=0x%x prop=%u type=%u fmt=%u "
               "nunits=%u mode=%u",
               (unsigned)wid, (unsigned)prop, (unsigned)type,
               (unsigned)fmt, (unsigned)nunits, (unsigned)mode);

        /* Generate PropertyNotify (state = 0 NewValue). Delivered to
         * clients selecting PropertyChange on this window. */
        {
            uint8_t evt[32];
            x11_build_property_notify(evt, win->x11_id, prop, 0u, 0u,
                                      seq, c->byte_order);
            (void)translation_deliver_event(c, evt,
                                            X11_EVMASK_PROPERTY_CHANGE,
                                            win->x11_id);
        }
        /* ChangeProperty is a void request: no reply, errors only. */
        break;
    }
    case X11_REQ_GET_PROPERTY: {
        /* GetProperty payload (after the 4-byte request header):
         *   4 window
         *   4 property (atom)
         *   4 type (atom, 0 = AnyType)
         *   4 long-offset (in 4-byte units)
         *   4 long-length (max 4-byte units to return)
         * Fixed payload is 20 bytes. */
        uint8_t delete = data;
        if (payload_len < 20u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t wid    = x11_get_u32(payload,      c->byte_order);
        uint32_t prop   = x11_get_u32(payload + 4,  c->byte_order);
        uint32_t type   = x11_get_u32(payload + 8,  c->byte_order);
        uint32_t loff   = x11_get_u32(payload + 12, c->byte_order);
        uint32_t llen   = x11_get_u32(payload + 16, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, wid);
        if (win == NULL) {
            x11_client_send_error(c, X11_ERR_WINDOW, wid, opcode, 0u);
            break;
        }
        struct x11_property *slot = x11_window_prop_find(win, prop);
        bool found = (slot != NULL && slot->in_use);
        if (found && type != 0u && slot->type != type) {
            /* Type mismatch: X11 treats this as "property does not
             * exist" for this reply, returning type=0/format=0. */
            found = false;
        }

        if (!found) {
            uint8_t reply[32];
            x11_build_get_property_reply(reply, sizeof(reply),
                                         0u, 0u, 0u, 0u, NULL, 0u,
                                         seq, c->byte_order);
            xc_log("info", "GetProperty: wid=0x%x prop=%u -> not found",
                   (unsigned)wid, (unsigned)prop);
            if (x11_client_send_raw(c, reply, sizeof(reply)) < 0) {
                return -1;
            }
            break;
        }

        /* Compute the reply slice. loff and llen are in 4-byte units
         * ("longs" in X11 parlance). The value bytes returned are
         * [offset*4, offset*4 + length*4) clamped to value_len. */
        size_t offset_bytes = (size_t)loff * 4u;
        size_t max_bytes    = (size_t)llen * 4u;
        size_t total_bytes  = slot->value_len;
        size_t avail_bytes  = (offset_bytes < total_bytes)
                              ? total_bytes - offset_bytes : 0u;
        size_t return_bytes = (avail_bytes < max_bytes)
                              ? avail_bytes : max_bytes;
        size_t bytes_after  = (avail_bytes > return_bytes)
                              ? avail_bytes - return_bytes : 0u;
        const uint8_t *value_ptr = (return_bytes > 0u)
                                   ? slot->value + offset_bytes : NULL;
        /* value_len in the reply is in format units. */
        size_t unit_bytes = slot->format / 8u;
        uint32_t value_len_units = (unit_bytes > 0u)
            ? (uint32_t)(return_bytes / unit_bytes) : 0u;

        uint8_t reply[32 + 4096];
        size_t rlen = x11_build_get_property_reply(reply, sizeof(reply),
                                                   slot->format, slot->type,
                                                   (uint32_t)bytes_after,
                                                   value_len_units,
                                                   value_ptr, return_bytes,
                                                   seq, c->byte_order);
        xc_log("info",
               "GetProperty: wid=0x%x prop=%u type=%u fmt=%u -> %zu bytes",
               (unsigned)wid, (unsigned)prop, (unsigned)slot->type,
               (unsigned)slot->format, return_bytes);
        if (x11_client_send_raw(c, reply, rlen) < 0) {
            return -1;
        }

        /* If delete=true and the entire property was returned (no
         * bytes_after), remove it and emit a PropertyNotify with
         * state=Deleted. */
        if (delete && bytes_after == 0u) {
            x11_window_prop_clear(slot);
            uint8_t evt[32];
            x11_build_property_notify(evt, win->x11_id, prop, 0u, 1u,
                                      seq, c->byte_order);
            (void)translation_deliver_event(c, evt,
                                            X11_EVMASK_PROPERTY_CHANGE,
                                            win->x11_id);
        }
        break;
    }
    case X11_REQ_GET_WINDOW_ATTRS: {
        /* GetWindowAttributes payload: 4-byte window XID. Reply is
         * 44 bytes. The bridge fills in the per-window state it
         * tracks; everything else defaults per X11. */
        if (payload_len < 4u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t wid = x11_get_u32(payload, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, wid);
        if (win == NULL) {
            x11_client_send_error(c, X11_ERR_WINDOW, wid, opcode, 0u);
            break;
        }
        uint8_t map_state = win->mapped ? X11_MAP_STATE_VIEWABLE
                                        : X11_MAP_STATE_UNMAPPED;
        uint8_t reply[44];
        x11_build_get_window_attributes_reply(reply,
                                              win->backing_store,
                                              win->visual_id,
                                              (uint16_t)win->window_class,
                                              win->bit_gravity,
                                              win->win_gravity,
                                              win->backing_planes,
                                              win->backing_pixel,
                                              win->override_redirect,
                                              win->save_under,
                                              map_state,
                                              win->mapped,  /* map-installed */
                                              win->colormap,
                                              win->event_mask,  /* all-event-masks */
                                              win->event_mask,  /* your-event-mask */
                                              win->do_not_propagate_mask,
                                              seq, c->byte_order);
        xc_log("info",
               "GetWindowAttributes: wid=0x%x map-state=%u class=%u visual=0x%x",
               (unsigned)wid, (unsigned)map_state, (unsigned)win->window_class,
               (unsigned)win->visual_id);
        if (x11_client_send_raw(c, reply, sizeof(reply)) < 0) {
            return -1;
        }
        break;
    }
    case X11_REQ_QUERY_TREE: {
        /* QueryTree payload: 4-byte window XID. Reply is 32 bytes
         * plus 4 bytes per child. Root's parent is 0; for any other
         * window, parent is its parent_xid. Children are all windows
         * whose parent_xid == the queried window's x11_id. */
        if (payload_len < 4u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t wid = x11_get_u32(payload, c->byte_order);
        uint32_t root_xid = X11_ROOT_WINDOW_ID;
        uint32_t parent_xid;
        if (wid == X11_ROOT_WINDOW_ID) {
            parent_xid = 0u;
        } else {
            struct x11_window *win = x11_client_find_window(c, wid);
            if (win == NULL) {
                x11_client_send_error(c, X11_ERR_WINDOW, wid, opcode, 0u);
                break;
            }
            parent_xid = win->parent_xid;
        }
        /* Collect children. Cap at the buffer the bridge is willing
         * to send: X11_CLIENT_MAX_WIN windows * 4 bytes plus the
         * 32-byte header. */
        uint32_t children[X11_CLIENT_MAX_WIN];
        uint16_t n = 0u;
        for (size_t i = 0; i < X11_CLIENT_MAX_WIN; i++) {
            if (!c->windows[i].in_use) continue;
            if (c->windows[i].parent_xid == wid) {
                children[n++] = c->windows[i].x11_id;
            }
        }
        uint8_t reply[32u + 4u * X11_CLIENT_MAX_WIN];
        size_t rlen = x11_build_query_tree_reply(reply, sizeof(reply),
                                                 root_xid, parent_xid,
                                                 children, n,
                                                 seq, c->byte_order);
        xc_log("info", "QueryTree: wid=0x%x parent=0x%x children=%u",
               (unsigned)wid, (unsigned)parent_xid, (unsigned)n);
        if (x11_client_send_raw(c, reply, rlen) < 0) {
            return -1;
        }
        break;
    }
    case X11_REQ_GET_ATOM_NAME: {
        /* GetAtomName payload: 4-byte atom. Reply is 32 + pad4(name)
         * bytes. Atom 0 (None) is a BadAtom error. */
        if (payload_len < 4u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t atom = x11_get_u32(payload, c->byte_order);
        if (atom == 0u) {
            x11_client_send_error(c, X11_ERR_ATOM, atom, opcode, 0u);
            break;
        }
        char namebuf[256];
        size_t name_len = x11_atom_get_name(atom, namebuf, sizeof(namebuf));
        if (name_len == 0u) {
            /* Atom is outside the predefined range and was never
             * interned. X11 says BadAtom. */
            x11_client_send_error(c, X11_ERR_ATOM, atom, opcode, 0u);
            break;
        }
        /* Cap name_len at u16 so the reply's name-length field fits.
         * Predefined atoms are all short; this is a defensive cap. */
        if (name_len > 65535u) name_len = 65535u;
        uint8_t reply[32u + 260u];
        size_t rlen = x11_build_get_atom_name_reply(reply, sizeof(reply),
                                                    namebuf,
                                                    (uint16_t)name_len,
                                                    seq, c->byte_order);
        xc_log("info", "GetAtomName: atom=%u -> \"%.*s\"",
               (unsigned)atom, (int)name_len, namebuf);
        if (x11_client_send_raw(c, reply, rlen) < 0) {
            return -1;
        }
        break;
    }
    case X11_REQ_DELETE_PROPERTY: {
        /* DeleteProperty payload: 4 window + 4 property. No reply.
         * Emit PropertyNotify (state=Deleted) to clients that
         * selected PropertyChange on the window. */
        if (payload_len < 8u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t wid  = x11_get_u32(payload,     c->byte_order);
        uint32_t prop = x11_get_u32(payload + 4, c->byte_order);
        if (prop == 0u) {
            x11_client_send_error(c, X11_ERR_ATOM, prop, opcode, 0u);
            break;
        }
        struct x11_window *win = x11_client_find_window(c, wid);
        if (win == NULL) {
            x11_client_send_error(c, X11_ERR_WINDOW, wid, opcode, 0u);
            break;
        }
        struct x11_property *slot = x11_window_prop_find(win, prop);
        if (slot != NULL && slot->in_use) {
            x11_window_prop_clear(slot);
            uint8_t evt[32];
            x11_build_property_notify(evt, win->x11_id, prop, 0u, 1u,
                                      seq, c->byte_order);
            (void)translation_deliver_event(c, evt,
                                            X11_EVMASK_PROPERTY_CHANGE,
                                            win->x11_id);
        }
        xc_log("info", "DeleteProperty: wid=0x%x prop=%u",
               (unsigned)wid, (unsigned)prop);
        break;
    }
    case X11_REQ_SET_SELECTION_OWNER: {
        /* SetSelectionOwner payload: 4 window + 4 selection + 4 time.
         * No reply. If timestamp is non-zero and earlier than the
         * current ownership timestamp, ignore (stale). If window is
         * 0, clear ownership. Generate SelectionClear to the previous
         * owner if ownership changed. */
        if (payload_len < 12u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t owner = x11_get_u32(payload,     c->byte_order);
        uint32_t sel   = x11_get_u32(payload + 4, c->byte_order);
        uint32_t time  = x11_get_u32(payload + 8, c->byte_order);
        if (sel == 0u) {
            x11_client_send_error(c, X11_ERR_ATOM, sel, opcode, 0u);
            break;
        }
        struct x11_selection *s = x11_client_find_selection(c, sel);
        bool stale = (s != NULL && s->in_use &&
                      time != 0u && s->timestamp != 0u &&
                      time < s->timestamp);
        if (stale) {
            xc_log("debug",
                   "SetSelectionOwner: stale time=%u current=%u for sel=%u",
                   (unsigned)time, (unsigned)s->timestamp, (unsigned)sel);
            break;
        }
        uint32_t prev_owner = (s != NULL && s->in_use) ? s->owner_xid : 0u;
        if (owner == 0u) {
            /* Clear ownership. */
            if (s != NULL) {
                s->owner_xid = 0u;
                s->timestamp = time;
            }
        } else {
            /* The owner window must be tracked by this client. X11
             * allows setting owner to an arbitrary window XID, but
             * the bridge only knows about tracked windows; an
             * unknown XID is a Window error. */
            struct x11_window *win = x11_client_find_window(c, owner);
            if (win == NULL) {
                x11_client_send_error(c, X11_ERR_WINDOW, owner, opcode, 0u);
                break;
            }
            if (s == NULL) {
                s = x11_client_alloc_selection(c, sel);
                if (s == NULL) {
                    x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
                    break;
                }
            }
            s->owner_xid = owner;
            s->timestamp = time;
        }
        /* If ownership changed and there was a previous owner,
         * deliver SelectionClear to it. SelectionClear is not subject
         * to event-mask selection: the bridge uses required_mask=0
         * so translation_deliver_event sends it unconditionally to
         * the previous-owner window (if tracked). */
        if (prev_owner != 0u && prev_owner != owner) {
            uint8_t evt[32];
            x11_build_selection_clear(evt, time, prev_owner, sel,
                                      seq, c->byte_order);
            (void)translation_deliver_event(c, evt, 0u, prev_owner);
        }
        xc_log("info",
               "SetSelectionOwner: sel=%u owner=0x%x time=%u prev=0x%x",
               (unsigned)sel, (unsigned)owner, (unsigned)time,
               (unsigned)prev_owner);
        break;
    }
    case X11_REQ_GET_SELECTION_OWNER: {
        /* GetSelectionOwner payload: 4-byte selection atom. Reply is
         * 32 bytes; owner is 0 if no current owner. */
        if (payload_len < 4u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t sel = x11_get_u32(payload, c->byte_order);
        if (sel == 0u) {
            x11_client_send_error(c, X11_ERR_ATOM, sel, opcode, 0u);
            break;
        }
        struct x11_selection *s = x11_client_find_selection(c, sel);
        uint32_t owner = (s != NULL && s->in_use) ? s->owner_xid : 0u;
        uint8_t reply[32];
        x11_build_get_selection_owner_reply(reply, owner, seq, c->byte_order);
        xc_log("info", "GetSelectionOwner: sel=%u -> owner=0x%x",
               (unsigned)sel, (unsigned)owner);
        if (x11_client_send_raw(c, reply, sizeof(reply)) < 0) {
            return -1;
        }
        break;
    }
    case X11_REQ_QUERY_POINTER: {
        /* QueryPointer payload: 4-byte window XID. Reply is 32 bytes.
         * v1 headless: same_screen=1, child=0 (no window under
         * pointer), all coords 0, mask 0. The bridge does not yet
         * track pointer state from Ishizue input events. */
        if (payload_len < 4u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t wid = x11_get_u32(payload, c->byte_order);
        if (wid != X11_ROOT_WINDOW_ID) {
            struct x11_window *win = x11_client_find_window(c, wid);
            if (win == NULL) {
                x11_client_send_error(c, X11_ERR_WINDOW, wid, opcode, 0u);
                break;
            }
        }
        uint8_t reply[32];
        x11_build_query_pointer_reply(reply, true,
                                      X11_ROOT_WINDOW_ID, 0u,
                                      0, 0, 0, 0, 0u,
                                      seq, c->byte_order);
        xc_log("info", "QueryPointer: wid=0x%x -> (0,0) headless",
               (unsigned)wid);
        if (x11_client_send_raw(c, reply, sizeof(reply)) < 0) {
            return -1;
        }
        break;
    }
    case X11_REQ_SET_INPUT_FOCUS: {
        /* SetInputFocus payload: 1 revert-to (header byte 1 = data)
         * + 4 focus + 4 time. No reply. Store the focus state; call
         * isz_client_send_seat_set_keyboard_focus on the focused
         * surface (or 0 if focus=0). */
        uint8_t revert_to = data;
        if (revert_to > 2u) {
            x11_client_send_error(c, X11_ERR_VALUE, (uint32_t)revert_to,
                                  opcode, 0u);
            break;
        }
        if (payload_len < 8u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t focus = x11_get_u32(payload,     c->byte_order);
        uint32_t time  = x11_get_u32(payload + 4, c->byte_order);
        /* PointerRoot (1) and None (0) are always valid. Any other
         * XID must be tracked. */
        if (focus != 0u && focus != 1u) {
            struct x11_window *win = x11_client_find_window(c, focus);
            if (win == NULL) {
                x11_client_send_error(c, X11_ERR_WINDOW, focus, opcode, 0u);
                break;
            }
        }
        c->focus_xid       = focus;
        c->focus_revert_to = revert_to;
        /* Translate to Ishizue seat focus. focus=0 -> surface 0
         * (clears focus). PointerRoot (1) maps to "any surface"; the
         * bridge has no equivalent, so it leaves the focus as-is on
         * the library side. */
        if (isz != NULL && focus != 1u) {
            uint32_t surf_id = 0u;
            if (focus != 0u) {
                struct x11_window *fwin = x11_client_find_window(c, focus);
                if (fwin != NULL) {
                    surf_id = fwin->isz_surface_id;
                }
            }
            (void)isz_client_send_seat_set_keyboard_focus(isz,
                                                          isz->seat_id,
                                                          surf_id);
        }
        xc_log("info",
               "SetInputFocus: focus=0x%x revert-to=%u time=%u",
               (unsigned)focus, (unsigned)revert_to, (unsigned)time);
        break;
    }
    case X11_REQ_CREATE_GC: {
        /* CreateGC payload: 4 gc + 4 drawable + 4 value-mask + 4n
         * value-list. No reply. Store the GC entry. */
        if (payload_len < 12u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t gc_xid = x11_get_u32(payload,     c->byte_order);
        uint32_t draw   = x11_get_u32(payload + 4, c->byte_order);
        uint32_t vmask  = x11_get_u32(payload + 8, c->byte_order);
        size_t vremaining = (payload_len > 12u) ? payload_len - 12u : 0u;
        /* The drawable must be root, a tracked window, or a tracked
         * pixmap. */
        if (x11_client_check_drawable(c, draw) == 0) {
            x11_client_send_error(c, X11_ERR_DRAWABLE, draw, opcode, 0u);
            break;
        }
        if (x11_client_find_gc(c, gc_xid) != NULL) {
            x11_client_send_error(c, X11_ERR_IDCHOICE, gc_xid, opcode, 0u);
            break;
        }
        struct x11_gc *gc = x11_client_alloc_gc(c, gc_xid);
        if (gc == NULL) {
            x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
            break;
        }
        gc->drawable_xid = draw;
        gc->value_mask   = vmask;
        /* Validate the value-list length up front: one 4-byte slot per
         * set bit in vmask. Counts the bits via Brian-Kernighan. */
        uint32_t bits = vmask;
        size_t slots_needed = 0u;
        while (bits != 0u) {
            bits &= bits - 1u;
            slots_needed++;
        }
        if (slots_needed * 4u > vremaining) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        /* Walk the value-list and pull out the fields the bridge
         * inspects later (graphics-exposure for PutImage). The rest
         * is accepted and discarded. */
        const uint8_t *vp = payload + 12u;
        for (uint32_t bit = 1u; bit != 0u && bit <= X11_GC_ARC_MODE;
             bit <<= 1u) {
            if ((vmask & bit) == 0u) continue;
            uint32_t v = x11_get_u32(vp, c->byte_order);
            switch (bit) {
            case X11_GC_FUNCTION:       gc->function        = (uint8_t)v; break;
            case X11_GC_PLANE_MASK:     /* accepted, not stored */         break;
            case X11_GC_FOREGROUND:     gc->foreground      = v;           break;
            case X11_GC_BACKGROUND:     gc->background      = v;           break;
            case X11_GC_LINE_WIDTH:     gc->line_width      = (uint16_t)v; break;
            case X11_GC_LINE_STYLE:     gc->line_style      = (uint8_t)v;  break;
            case X11_GC_CAP_STYLE:      gc->cap_style       = (uint8_t)v;  break;
            case X11_GC_JOIN_STYLE:     gc->join_style      = (uint8_t)v;  break;
            case X11_GC_FILL_STYLE:     gc->fill_style      = (uint8_t)v;  break;
            case X11_GC_FILL_RULE:      gc->fill_rule       = (uint8_t)v;  break;
            case X11_GC_TILE:           /* accepted, not stored */         break;
            case X11_GC_STIPPLE:        /* accepted, not stored */         break;
            case X11_GC_TILE_STIPPLE_X: /* accepted, not stored */         break;
            case X11_GC_TILE_STIPPLE_Y: /* accepted, not stored */         break;
            case X11_GC_FONT:           /* accepted, not stored */         break;
            case X11_GC_SUBWINDOW_MODE: gc->subwindow_mode  = (uint8_t)v;  break;
            case X11_GC_GRAPHICS_EXPOSURE:
                gc->graphics_exposure = (v != 0u);
                break;
            case X11_GC_CLIP_X_ORIGIN:  gc->clip_x_origin   = (int16_t)v;  break;
            case X11_GC_CLIP_Y_ORIGIN:  gc->clip_y_origin   = (int16_t)v;  break;
            case X11_GC_CLIP_MASK:      /* accepted, not stored */         break;
            case X11_GC_DASH_OFFSET:    /* accepted, not stored */         break;
            case X11_GC_DASHES:         /* accepted, not stored */         break;
            case X11_GC_ARC_MODE:       gc->arc_mode        = (uint8_t)v;  break;
            default: break;
            }
            vp += 4u;
        }
        xc_log("info",
               "CreateGC: gc=0x%x drawable=0x%x mask=0x%x graphics-exposure=%d",
               (unsigned)gc_xid, (unsigned)draw, (unsigned)vmask,
               (int)gc->graphics_exposure);
        break;
    }
    case X11_REQ_PUT_IMAGE: {
        /* PutImage payload:
         *   1 format (header byte 1 = data)
         *   2 length
         *   4 drawable
         *   4 gc
         *   2 width
         *   2 height
         *   2 dst-x
         *   2 dst-y
         *   1 left-pad
         *   1 depth
         *   2 unused
         *   n image data, padded to 4
         * No reply. v1 stashes the data on the drawable's backing
         * store. GraphicsExposure generation is skipped (the v1
         * PutImage path consumes the request only). */
        uint8_t format = data;
        if (format > 2u) {
            x11_client_send_error(c, X11_ERR_VALUE, (uint32_t)format,
                                  opcode, 0u);
            break;
        }
        if (payload_len < 20u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t draw    = x11_get_u32(payload,      c->byte_order);
        uint32_t gc_xid  = x11_get_u32(payload + 4,  c->byte_order);
        uint16_t width   = x11_get_u16(payload + 8,  c->byte_order);
        uint16_t height  = x11_get_u16(payload + 10, c->byte_order);
        int16_t  dst_x   = (int16_t)x11_get_u16(payload + 12, c->byte_order);
        int16_t  dst_y   = (int16_t)x11_get_u16(payload + 14, c->byte_order);
        uint8_t  left_pad = payload[16];
        uint8_t  depth   = payload[17];
        /* data bytes = total - 24 (header 4 + 20 fixed payload) */
        size_t data_bytes = (payload_len > 20u) ? payload_len - 20u : 0u;
        if (data_bytes > X11_PUT_IMAGE_MAX_BYTES) {
            x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
            break;
        }
        (void)gc_xid; (void)dst_x; (void)dst_y; (void)left_pad;
        if (draw == X11_ROOT_WINDOW_ID) {
            xc_log("info",
                   "PutImage: on root, %zu bytes discarded",
                   data_bytes);
            break;
        }
        if (x11_client_check_drawable(c, draw) == 0) {
            x11_client_send_error(c, X11_ERR_DRAWABLE, draw, opcode, 0u);
            break;
        }
        struct x11_window *win = x11_client_find_window(c, draw);
        if (win == NULL) {
            /* Pixmap: accept and discard. */
            xc_log("debug",
                   "PutImage: on pixmap 0x%x, %zu bytes discarded",
                   (unsigned)draw, data_bytes);
            break;
        }
        /* Stash the image data on the window. */
        x11_window_backing_image_clear(win);
        if (data_bytes > 0u) {
            uint8_t *copy = malloc(data_bytes);
            if (copy == NULL) {
                x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
                break;
            }
            memcpy(copy, payload + 20u, data_bytes);
            win->backing_image        = copy;
            win->backing_image_len    = data_bytes;
            win->backing_image_w      = width;
            win->backing_image_h      = height;
            win->backing_image_depth  = depth;
            win->backing_image_format = format;
        }
        xc_log("info",
               "PutImage: drawable=0x%x %ux%u depth=%u format=%u data=%zu",
               (unsigned)draw, (unsigned)width, (unsigned)height,
               (unsigned)depth, (unsigned)format, data_bytes);
        break;
    }
    case X11_REQ_FREE_GC: {
        /* FreeGC payload: 4 gc. Total 8 bytes. No reply. Marks the
         * GC slot free. BadGC if the XID is not tracked. */
        if (total < 8) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t gc_xid = x11_get_u32(buf + 4, c->byte_order);
        struct x11_gc *gc = x11_client_find_gc(c, gc_xid);
        if (gc == NULL) {
            x11_client_send_error(c, X11_ERR_GCONTEXT, gc_xid, opcode, 0u);
            break;
        }
        gc->in_use = false;
        xc_log("info", "FreeGC: gc=0x%x", (unsigned)gc_xid);
        break;
    }
    case X11_REQ_CLEAR_AREA: {
        /* ClearArea payload: 1 exposures (header byte 1 = data),
         * 4 window, 2 x, 2 y, 2 width, 2 height. Total 16 bytes. No
         * reply. v1: if the window has a ZPixmap depth-24 backing
         * image, paint the rect with background_pixel. width=0 or
         * height=0 means "to the end" per X11. If exposures=true,
         * emit Expose to clients that selected Exposure. */
        uint8_t exposures = data;
        if (total < 16) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t wid = x11_get_u32(buf + 4, c->byte_order);
        int16_t  x   = (int16_t)x11_get_u16(buf + 8,  c->byte_order);
        int16_t  y   = (int16_t)x11_get_u16(buf + 10, c->byte_order);
        uint16_t w   = x11_get_u16(buf + 12, c->byte_order);
        uint16_t h   = x11_get_u16(buf + 14, c->byte_order);
        struct x11_window *win = x11_client_find_window(c, wid);
        if (win == NULL) {
            x11_client_send_error(c, X11_ERR_WINDOW, wid, opcode, 0u);
            break;
        }
        if (win->backing_image != NULL &&
            win->backing_image_format == X11_IMAGE_FORMAT_ZPIXMAP &&
            win->backing_image_depth == 24u) {
            uint32_t bg = win->background_pixel;
            int32_t bw = (int32_t)win->backing_image_w;
            int32_t bh = (int32_t)win->backing_image_h;
            int32_t rw = (w == 0u) ? bw - (int32_t)x : (int32_t)w;
            int32_t rh = (h == 0u) ? bh - (int32_t)y : (int32_t)h;
            if (rw < 0) rw = 0;
            if (rh < 0) rh = 0;
            for (int32_t row = 0; row < rh; row++) {
                int32_t py = (int32_t)y + row;
                if (py < 0 || py >= bh) continue;
                for (int32_t col = 0; col < rw; col++) {
                    int32_t px = (int32_t)x + col;
                    if (px < 0 || px >= bw) continue;
                    size_t off = ((size_t)py * (size_t)bw + (size_t)px) * 4u;
                    if (off + 4u > win->backing_image_len) continue;
                    x11_put_u32(win->backing_image + off, bg, c->byte_order);
                }
            }
        }
        if (exposures != 0u) {
            uint16_t ex = (x < 0) ? 0u : (uint16_t)x;
            uint16_t ey = (y < 0) ? 0u : (uint16_t)y;
            uint8_t evt[32];
            x11_build_expose(evt, win->x11_id, ex, ey, w, h, 0u,
                             seq, c->byte_order);
            (void)translation_deliver_event(c, evt,
                                            X11_EVMASK_EXPOSURE,
                                            win->x11_id);
        }
        xc_log("info",
               "ClearArea: wid=0x%x rect=(%d,%d,%ux%u) exposures=%d",
               (unsigned)wid, (int)x, (int)y, (unsigned)w, (unsigned)h,
               (int)exposures);
        break;
    }
    case X11_REQ_COPY_AREA: {
        /* CopyArea payload: 4 src drawable, 4 dst drawable, 4 gc,
         * 2 src-x, 2 src-y, 2 dst-x, 2 dst-y, 2 width, 2 height.
         * Total 28 bytes. No reply. v1: accept and no-op the pixel
         * copy. If the GC has graphics-exposure=false, emit NoExpose
         * to the originating client. */
        if (total < 28) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t src_draw = x11_get_u32(buf + 4,  c->byte_order);
        uint32_t dst_draw = x11_get_u32(buf + 8,  c->byte_order);
        uint32_t gc_xid   = x11_get_u32(buf + 12, c->byte_order);
        if (x11_client_check_drawable(c, src_draw) == 0) {
            x11_client_send_error(c, X11_ERR_DRAWABLE, src_draw, opcode, 0u);
            break;
        }
        if (x11_client_check_drawable(c, dst_draw) == 0) {
            x11_client_send_error(c, X11_ERR_DRAWABLE, dst_draw, opcode, 0u);
            break;
        }
        struct x11_gc *gc = x11_client_find_gc(c, gc_xid);
        if (gc == NULL) {
            x11_client_send_error(c, X11_ERR_GCONTEXT, gc_xid, opcode, 0u);
            break;
        }
        if (!gc->graphics_exposure) {
            uint8_t evt[32];
            x11_build_no_expose(evt, dst_draw, 0u, opcode, seq,
                                c->byte_order);
            (void)x11_client_send_event(c,
                                        (const struct x11_event_32 *)evt);
        }
        xc_log("info",
               "CopyArea: src=0x%x dst=0x%x gc=0x%x (no-op, ge=%d)",
               (unsigned)src_draw, (unsigned)dst_draw,
               (unsigned)gc_xid, (int)gc->graphics_exposure);
        break;
    }
    case X11_REQ_POLY_FILL_RECTANGLE: {
        /* PolyFillRectangle payload: 4 drawable, 4 gc, then n
         * rectangles each 8 bytes (2x, 2y, 2w, 2h). Total = 8 +
         * n*8 bytes payload. No reply. v1: accept and discard; the
         * bridge does no vector rendering. */
        if (total < 12) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t draw   = x11_get_u32(buf + 4, c->byte_order);
        uint32_t gc_xid = x11_get_u32(buf + 8, c->byte_order);
        if (x11_client_check_drawable(c, draw) == 0) {
            x11_client_send_error(c, X11_ERR_DRAWABLE, draw, opcode, 0u);
            break;
        }
        if (x11_client_find_gc(c, gc_xid) == NULL) {
            x11_client_send_error(c, X11_ERR_GCONTEXT, gc_xid, opcode, 0u);
            break;
        }
        size_t payload_len = total - 4u;
        size_t nrects = (payload_len > 8u) ? (payload_len - 8u) / 8u : 0u;
        xc_log("info",
               "PolyFillRectangle: drawable=0x%x gc=0x%x rects=%zu (no-op)",
               (unsigned)draw, (unsigned)gc_xid, nrects);
        break;
    }
    case X11_REQ_GET_IMAGE: {
        /* GetImage payload: 1 format (header byte 1 = data), 4
         * drawable, 2 x, 2 y, 2 width, 2 height, 4 plane-mask.
         * Total 20 bytes. Reply: 32-byte header (code, depth, seq,
         * length, visual, 20 bytes pad) then width*height*bpp/8
         * bytes of image data, padded to 4. v1: for a window with a
         * ZPixmap depth-24 backing image, return the backing image
         * rect; elsewhere return zeros. */
        uint8_t format = data;
        if (format > 2u) {
            x11_client_send_error(c, X11_ERR_VALUE, (uint32_t)format,
                                  opcode, 0u);
            break;
        }
        if (total < 20) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t draw = x11_get_u32(buf + 4,  c->byte_order);
        int16_t  x    = (int16_t)x11_get_u16(buf + 8,  c->byte_order);
        int16_t  y    = (int16_t)x11_get_u16(buf + 10, c->byte_order);
        uint16_t w    = x11_get_u16(buf + 12, c->byte_order);
        uint16_t h    = x11_get_u16(buf + 14, c->byte_order);
        /* plane_mask at buf + 16; ignored in v1 (all planes returned). */

        uint8_t  depth  = 24u;
        uint32_t visual = X11_ROOT_VISUAL_ID;
        struct x11_window *win = NULL;
        if (draw == X11_ROOT_WINDOW_ID) {
            depth = 24u;
        } else {
            win = x11_client_find_window(c, draw);
            if (win != NULL) {
                depth  = (win->depth == 0u) ? 24u : win->depth;
                visual = (win->visual_id == 0u)
                          ? X11_ROOT_VISUAL_ID : win->visual_id;
            } else {
                struct x11_pixmap *pix = x11_client_find_pixmap(c, draw);
                if (pix == NULL) {
                    x11_client_send_error(c, X11_ERR_DRAWABLE, draw,
                                          opcode, 0u);
                    break;
                }
                depth = pix->depth;
            }
        }

        /* v1 assumes ZPixmap depth 24 (4 bpp) for the data size.
         * Other depths return zeros of the same computed size; the
         * test only uses depth 24. */
        size_t bpp = 4u;
        size_t data_bytes = (size_t)w * (size_t)h * bpp;
        size_t padded = x11_pad4(data_bytes);
        size_t reply_len = 32u + padded;

        uint8_t *reply = malloc(reply_len);
        if (reply == NULL) {
            x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
            break;
        }
        memset(reply, 0, reply_len);
        reply[0] = 1u;  /* reply */
        reply[1] = depth;
        x11_put_u16(reply + 2, seq, c->byte_order);
        x11_put_u32(reply + 4, (uint32_t)(padded / 4u), c->byte_order);
        x11_put_u32(reply + 8, visual, c->byte_order);
        /* bytes 12..31: pad, already zeroed. */

        if (win != NULL && win->backing_image != NULL &&
            win->backing_image_format == X11_IMAGE_FORMAT_ZPIXMAP &&
            win->backing_image_depth == 24u &&
            format == X11_IMAGE_FORMAT_ZPIXMAP) {
            int32_t bw = (int32_t)win->backing_image_w;
            int32_t bh = (int32_t)win->backing_image_h;
            for (int32_t row = 0; row < (int32_t)h; row++) {
                int32_t py = (int32_t)y + row;
                if (py < 0 || py >= bh) continue;
                for (int32_t col = 0; col < (int32_t)w; col++) {
                    int32_t px = (int32_t)x + col;
                    if (px < 0 || px >= bw) continue;
                    size_t src_off = ((size_t)py * (size_t)bw + (size_t)px) * 4u;
                    size_t dst_off = 32u + ((size_t)row * (size_t)w + (size_t)col) * 4u;
                    if (src_off + 4u > win->backing_image_len) continue;
                    if (dst_off + 4u > reply_len) continue;
                    memcpy(reply + dst_off,
                           win->backing_image + src_off, 4u);
                }
            }
        }

        xc_log("info",
               "GetImage: drawable=0x%x fmt=%u rect=(%d,%d,%ux%u) -> %zu bytes",
               (unsigned)draw, (unsigned)format, (int)x, (int)y,
               (unsigned)w, (unsigned)h, data_bytes);
        int rc = x11_client_send_raw(c, reply, reply_len);
        free(reply);
        if (rc < 0) return -1;
        break;
    }
    case 53: {  /* CreatePixmap */
        /* CreatePixmap payload: 1 depth (header byte 1), 4 pid, 4
         * drawable, 2 width, 2 height. Total 16 bytes. No reply. */
        if (total < 16) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint8_t  depth = data;
        uint32_t pid   = x11_get_u32(buf + 4, c->byte_order);
        uint32_t draw  = x11_get_u32(buf + 8, c->byte_order);
        uint16_t w     = x11_get_u16(buf + 12, c->byte_order);
        uint16_t h     = x11_get_u16(buf + 14, c->byte_order);
        if (x11_client_check_drawable(c, draw) == 0) {
            x11_client_send_error(c, X11_ERR_DRAWABLE, draw, opcode, 0u);
            break;
        }
        if (x11_client_find_pixmap(c, pid) != NULL) {
            x11_client_send_error(c, X11_ERR_IDCHOICE, pid, opcode, 0u);
            break;
        }
        struct x11_pixmap *pix = x11_client_alloc_pixmap(c, pid);
        if (pix == NULL) {
            x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
            break;
        }
        pix->drawable_xid = draw;
        pix->width  = w;
        pix->height = h;
        pix->depth  = depth;
        break;
    }
    case 54: {  /* FreePixmap */
        /* FreePixmap payload: 4 pixmap. Total 8 bytes. No reply. */
        if (total < 8) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t pid = x11_get_u32(buf + 4, c->byte_order);
        struct x11_pixmap *pix = x11_client_find_pixmap(c, pid);
        if (pix == NULL) {
            x11_client_send_error(c, X11_ERR_PIXMAP, pid, opcode, 0u);
            break;
        }
        pix->in_use = false;
        break;
    }
    case X11_REQ_CREATE_COLORMAP: {  /* 78 */
        /* CreateColormap payload (after the 4-byte header):
         *   1 alloc (header byte 1 = data: 0 AllocNone, 1 AllocAll)
         *   4 colormap XID (client-allocated)
         *   4 window
         *   4 visual
         * Total 12 bytes payload = 16 bytes request. No reply. */
        if (total < 16u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint8_t  alloc  = data;
        uint32_t cmap   = x11_get_u32(buf + 4, c->byte_order);
        uint32_t win_x  = x11_get_u32(buf + 8, c->byte_order);
        uint32_t visual = x11_get_u32(buf + 12, c->byte_order);
        if (alloc > 1u) {
            x11_client_send_error(c, X11_ERR_VALUE, (uint32_t)alloc,
                                  opcode, 0u);
            break;
        }
        if (win_x != X11_ROOT_WINDOW_ID &&
            x11_client_find_window(c, win_x) == NULL) {
            x11_client_send_error(c, X11_ERR_WINDOW, win_x, opcode, 0u);
            break;
        }
        if (x11_client_find_colormap(c, cmap) != NULL) {
            x11_client_send_error(c, X11_ERR_IDCHOICE, cmap, opcode, 0u);
            break;
        }
        struct x11_colormap *cm = x11_client_alloc_colormap(c, cmap);
        if (cm == NULL) {
            x11_client_send_error(c, X11_ERR_ALLOC, 0u, opcode, 0u);
            break;
        }
        cm->window_xid = win_x;
        cm->visual_id  = visual;
        cm->alloc_flag = alloc;
        xc_log("info",
               "CreateColormap: cmap=0x%x window=0x%x visual=0x%x alloc=%u",
               (unsigned)cmap, (unsigned)win_x, (unsigned)visual,
               (unsigned)alloc);
        break;
    }
    case X11_REQ_FREE_COLORMAP: {  /* 79 */
        /* FreeColormap payload: 4 colormap. Total 8 bytes. No reply.
         * Look up the colormap; mark its slot free. BadColormap if
         * not tracked. */
        if (total < 8u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t cmap = x11_get_u32(buf + 4, c->byte_order);
        struct x11_colormap *cm = x11_client_find_colormap(c, cmap);
        if (cm == NULL) {
            x11_client_send_error(c, X11_ERR_COLORMAP, cmap, opcode, 0u);
            break;
        }
        cm->in_use = false;
        xc_log("info", "FreeColormap: cmap=0x%x", (unsigned)cmap);
        break;
    }
    case X11_REQ_ALLOC_COLOR: {  /* 84 */
        /* AllocColor payload (after the 4-byte header):
         *   4 colormap
         *   2 red
         *   2 green
         *   2 blue
         *   2 unused
         * Total 12 bytes payload = 16 bytes request. Reply is 32 bytes.
         * v1 headless: echo back the requested RGB and return
         * pixel = 0x00RRGGBB. The bridge does no real allocation. */
        if (total < 16u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t cmap = x11_get_u32(buf + 4, c->byte_order);
        uint16_t r    = x11_get_u16(buf + 8,  c->byte_order);
        uint16_t g    = x11_get_u16(buf + 10, c->byte_order);
        uint16_t b    = x11_get_u16(buf + 12, c->byte_order);
        if (x11_client_find_colormap(c, cmap) == NULL) {
            x11_client_send_error(c, X11_ERR_COLORMAP, cmap, opcode, 0u);
            break;
        }
        uint32_t pixel = ((uint32_t)(r & 0xFFu) << 16) |
                         ((uint32_t)(g & 0xFFu) << 8)  |
                          (uint32_t)(b & 0xFFu);
        uint8_t reply[32];
        memset(reply, 0, sizeof(reply));
        reply[0] = 1u;  /* reply indicator */
        x11_put_u16(reply + 2, seq, c->byte_order);
        x11_put_u32(reply + 4, 0u, c->byte_order);  /* reply length */
        x11_put_u16(reply + 8,  r, c->byte_order);
        x11_put_u16(reply + 10, g, c->byte_order);
        x11_put_u16(reply + 12, b, c->byte_order);
        /* bytes 14..15 pad */
        x11_put_u32(reply + 16, pixel, c->byte_order);
        xc_log("info",
               "AllocColor: cmap=0x%x rgb=(%u,%u,%u) -> pixel=0x%x",
               (unsigned)cmap, (unsigned)r, (unsigned)g, (unsigned)b,
               (unsigned)pixel);
        if (x11_client_send_raw(c, reply, sizeof(reply)) < 0) {
            return -1;
        }
        break;
    }
    case X11_REQ_QUERY_COLORS: {  /* 91 */
        /* QueryColors payload (after the 4-byte header):
         *   4 colormap
         *   n pixels (each 4 bytes)
         * Total = 4 + n*4 bytes payload = 8 + n*4 bytes request.
         * Reply: 32 bytes header + n*8 bytes (each color is
         *   2 red, 2 green, 2 blue, 2 pad).
         * v1: unpack each pixel as 0x00RRGGBB. Works for 24-bit
         * TrueColor visuals (the only visual the bridge advertises). */
        if (total < 8u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t cmap = x11_get_u32(buf + 4, c->byte_order);
        if (x11_client_find_colormap(c, cmap) == NULL) {
            x11_client_send_error(c, X11_ERR_COLORMAP, cmap, opcode, 0u);
            break;
        }
        size_t pixel_bytes = (total > 8u) ? total - 8u : 0u;
        size_t n = pixel_bytes / 4u;
        /* Cap n so the reply buffer fits. 256 colors = 2048 bytes of
         * color data, plenty for any v1 client. */
        if (n > 256u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint16_t n16 = (uint16_t)n;
        uint8_t reply[32u + 256u * 8u];
        memset(reply, 0, sizeof(reply));
        reply[0] = 1u;  /* reply indicator */
        x11_put_u16(reply + 2, seq, c->byte_order);
        /* reply length = additional 4-byte units after first 32 bytes
         * = (n * 8) / 4 = 2 * n. */
        x11_put_u32(reply + 4, (uint32_t)(2u * n), c->byte_order);
        x11_put_u16(reply + 8, n16, c->byte_order);
        /* bytes 10..31 pad */
        for (uint16_t i = 0; i < n16; i++) {
            uint32_t px = x11_get_u32(buf + 8u + (size_t)i * 4u,
                                      c->byte_order);
            uint16_t r = (uint16_t)((px >> 16) & 0xFFu);
            uint16_t g = (uint16_t)((px >> 8)  & 0xFFu);
            uint16_t b = (uint16_t)(px        & 0xFFu);
            size_t off = 32u + (size_t)i * 8u;
            x11_put_u16(reply + off,     r, c->byte_order);
            x11_put_u16(reply + off + 2, g, c->byte_order);
            x11_put_u16(reply + off + 4, b, c->byte_order);
            /* bytes off+6..off+7 pad */
        }
        xc_log("info", "QueryColors: cmap=0x%x n=%u",
               (unsigned)cmap, (unsigned)n16);
        if (x11_client_send_raw(c, reply, 32u + (size_t)n16 * 8u) < 0) {
            return -1;
        }
        break;
    }
    case X11_REQ_LOOKUP_COLOR: {  /* 92 */
        /* LookupColor payload (after the 4-byte header):
         *   4 colormap
         *   2 name_len
         *   2 unused
         *   name (padded to 4)
         * Total = 12 + pad4(name_len) bytes request. Reply is 32 bytes.
         * v1: the bridge ships no Xrgb.txt. Return exact RGB =
         * screen RGB = (0, 0, 0) for every name. Wrong but does not
         * crash; clients that need real color lookup get black. */
        if (total < 12u) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint32_t cmap     = x11_get_u32(buf + 4, c->byte_order);
        uint16_t name_len = x11_get_u16(buf + 8, c->byte_order);
        if (x11_client_find_colormap(c, cmap) == NULL) {
            x11_client_send_error(c, X11_ERR_COLORMAP, cmap, opcode, 0u);
            break;
        }
        if (name_len == 0u) {
            x11_client_send_error(c, X11_ERR_VALUE, 0u, opcode, 0u);
            break;
        }
        size_t name_padded = (size_t)((name_len + 3u) & ~3u);
        if (total < 12u + name_padded) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint8_t reply[32];
        memset(reply, 0, sizeof(reply));
        reply[0] = 1u;  /* reply indicator */
        x11_put_u16(reply + 2, seq, c->byte_order);
        x11_put_u32(reply + 4, 0u, c->byte_order);  /* reply length */
        /* exact-red, exact-green, exact-blue at 8, 10, 12 */
        /* screen-red, screen-green, screen-blue at 14, 16, 18 */
        /* All zero: black. */
        xc_log("info", "LookupColor: cmap=0x%x name_len=%u -> black",
               (unsigned)cmap, (unsigned)name_len);
        if (x11_client_send_raw(c, reply, sizeof(reply)) < 0) {
            return -1;
        }
        break;
    }
    case 98: {  /* QueryExtension */
        if (total < 8) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        uint16_t name_len = x11_get_u16(buf + 4, c->byte_order);
        if (total < (size_t)(8 + ((name_len + 3u) & ~3u))) {
            x11_client_send_error(c, X11_ERR_LENGTH, 0u, opcode, 0u);
            break;
        }
        const char *name = (const char *)(buf + 8);
        char ext_name[256];
        size_t copy = name_len < sizeof(ext_name) - 1 ? name_len : sizeof(ext_name) - 1;
        memcpy(ext_name, name, copy);
        ext_name[copy] = '\0';

        /* Reply with present=0 so the client falls back to core
         * protocol. The bridge supports no extensions yet. */
        uint8_t reply[32];
        memset(reply, 0, sizeof(reply));
        reply[0] = 1;  /* reply */
        x11_put_u16(reply + 2, seq, c->byte_order);
        x11_put_u32(reply + 4, 0, c->byte_order);  /* length */
        reply[8] = 0;  /* present = false */
        x11_client_send_raw(c, reply, sizeof(reply));
        xc_log("debug", "QueryExtension: '%s' -> not present", ext_name);
        break;
    }
    case 99: {  /* ListExtensions */
        uint8_t reply[32];
        memset(reply, 0, sizeof(reply));
        reply[0] = 1;  /* reply */
        reply[1] = 0;  /* count = 0 */
        x11_put_u16(reply + 2, seq, c->byte_order);
        x11_put_u32(reply + 4, 0, c->byte_order);
        x11_client_send_raw(c, reply, sizeof(reply));
        xc_log("debug", "ListExtensions -> 0 extensions");
        break;
    }
    case 43: {  /* GetInputFocus */
        uint8_t reply[32];
        memset(reply, 0, sizeof(reply));
        reply[0] = 1;  /* reply */
        reply[1] = 0;  /* revert-to = None */
        x11_put_u16(reply + 2, seq, c->byte_order);
        x11_put_u32(reply + 4, 0, c->byte_order);
        x11_put_u32(reply + 8, 0, c->byte_order);  /* focus = None */
        x11_client_send_raw(c, reply, sizeof(reply));
        break;
    }
    case 119: {  /* NoOperation */
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
