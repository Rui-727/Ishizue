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

/* translation.c: X11 <-> Ishizue mapping logic.
 *
 * See translation.h for the high-level shape. The X11 -> Ishizue
 * direction emits real wire messages via isz_client, including
 * SET_PLANE_TYPE / SET_PLANE_SLOT (SPEC §7.7: mandatory before
 * commit) and SET_ZPOS for stacking changes. The Ishizue -> X11
 * direction emits real 32-byte X11 events via x11_client. Event
 * delivery honors the per-window event-mask the client selected via
 * ChangeWindowAttributes. */

#include "translation.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <ishizue/isz.h>

#include "isz_client.h"
#include "x11_client.h"

static void tr_log(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void tr_log(const char *level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "x11bridge/tr: %s: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* X11 -> Ishizue                                                      */
/* ------------------------------------------------------------------ */

void translation_on_x11_create_window(struct isz_client *isz,
                                      struct x11_client *xc,
                                      struct x11_window *win,
                                      int32_t x, int32_t y,
                                      int32_t w, int32_t h) {
    (void)xc;
    if (isz == NULL || win == NULL) return;
    tr_log("info", "CreateWindow x11=%u -> isz surface (x=%d y=%d w=%d h=%d)",
           (unsigned)win->x11_id, (int)x, (int)y, (int)w, (int)h);
    if (isz_client_send_surface_create(isz, &win->isz_surface_id) < 0) {
        tr_log("error", "surface_create send failed");
        return;
    }
    win->has_surface = true;
    /* Send an initial position+size before the first MapWindow. */
    (void)isz_client_send_surface_set_position(isz, win->isz_surface_id,
                                               x, y);
    (void)isz_client_send_surface_set_size(isz, win->isz_surface_id,
                                           w, h);
}

void translation_on_x11_map_window(struct isz_client *isz,
                                   struct x11_client *xc,
                                   struct x11_window *win) {
    (void)xc;
    if (isz == NULL || win == NULL) return;
    tr_log("info", "MapWindow x11=%u isz=%u",
           (unsigned)win->x11_id, (unsigned)win->isz_surface_id);

    /* Only top-level windows (those with a real Ishizue surface) get
     * scanned out. Child windows stay tracked but the bridge does not
     * composite them in v1; the parent's next commit re-paints them. */
    if (!win->has_surface) {
        tr_log("debug", "MapWindow x11=%u: child window, no surface",
               (unsigned)win->x11_id);
        return;
    }

    /* SPEC §7.7: every surface must have a plane type and a plane
     * slot assigned before commit, otherwise the library rejects the
     * commit with ISZ_ERR_SURFACE_NO_PLANE_SLOT. The bridge picks
     * PRIMARY at slot 0 for v1; the Architect can rearrange later. */
    if (win->isz_surface_id != 0u) {
        (void)isz_client_send_surface_set_plane_type(isz, win->isz_surface_id,
                                                    (int)ISZ_PLANE_PRIMARY);
        (void)isz_client_send_surface_set_plane_slot(isz, win->isz_surface_id,
                                                    0);
    }

    /* SPEC §6.5: bind the surface to a real output id from the §6.5
     * globals. The handshake stub does not broadcast any yet, so
     * isz->output_id stays 0. The server will reject the commit
     * until a real output exists; that is fine for the scaffold. */
    (void)isz_client_send_surface_set_output(isz, win->isz_surface_id,
                                             isz->output_id);
    win->has_output = true;
    (void)isz_client_send_commit_flags(isz, isz->output_id,
                                       ISZ_COMMIT_NORMAL);
}

void translation_on_x11_unmap_window(struct isz_client *isz,
                                     struct x11_client *xc,
                                     struct x11_window *win) {
    (void)xc;
    if (isz == NULL || win == NULL) return;
    tr_log("info", "UnmapWindow x11=%u isz=%u",
           (unsigned)win->x11_id, (unsigned)win->isz_surface_id);
    if (!win->has_surface || !win->has_output) {
        /* Nothing on the output side to undo. */
        return;
    }
    /* Clear the surface's output binding and commit so the surface
     * stops being scanned out. SET_OUTPUT with output_id 0 is the
     * bridge's clear-output wire message (see isz_client.h). */
    (void)isz_client_send_surface_clear_output(isz, win->isz_surface_id);
    win->has_output = false;
    (void)isz_client_send_commit_flags(isz, isz->output_id,
                                       ISZ_COMMIT_NORMAL);
}

void translation_on_x11_configure_window(struct isz_client *isz,
                                         struct x11_client *xc,
                                         struct x11_window *win,
                                         int32_t x, int32_t y,
                                         int32_t w, int32_t h) {
    (void)xc;
    if (isz == NULL || win == NULL) return;
    tr_log("info", "ConfigureWindow x11=%u isz=%u (x=%d y=%d w=%d h=%d)",
           (unsigned)win->x11_id, (unsigned)win->isz_surface_id,
           (int)x, (int)y, (int)w, (int)h);
    if (!win->has_surface) return;
    (void)isz_client_send_surface_set_position(isz, win->isz_surface_id,
                                               x, y);
    (void)isz_client_send_surface_set_size(isz, win->isz_surface_id,
                                           w, h);
    /* If the window is currently scanned out, push the new geometry
     * to the output. The bridge commits unconditionally; the server
     * deduplicates no-op commits. */
    if (win->has_output) {
        (void)isz_client_send_commit_flags(isz, isz->output_id,
                                           ISZ_COMMIT_NORMAL);
    }
}

void translation_on_x11_destroy_window(struct isz_client *isz,
                                       struct x11_client *xc,
                                       struct x11_window *win) {
    (void)xc;
    if (isz == NULL || win == NULL) return;
    tr_log("info", "DestroyWindow x11=%u isz=%u",
           (unsigned)win->x11_id, (unsigned)win->isz_surface_id);
    if (!win->has_surface) return;
    /* SPEC §7.6: surface_destroy releases the server-side surface
     * and its plane slot. The bridge does not need to clear_output
     * first; the server tears down the output binding on destroy. */
    (void)isz_client_send_surface_destroy(isz, win->isz_surface_id);
    win->has_surface = false;
    win->has_output = false;
}

/* ------------------------------------------------------------------ */
/* Event delivery                                                      */
/* ------------------------------------------------------------------ */

/* Look up a window by X11 id in the client's table. */
static struct x11_window *find_win(struct x11_client *xc, uint32_t xid) {
    /* The windows table is private to x11_client.c, but the bridge
     * already exposes x11_client_first_mapped. For event delivery
     * we walk the same array; the struct layout is public in
     * x11_client.h. */
    for (size_t i = 0; i < X11_CLIENT_MAX_WIN; i++) {
        if (xc->windows[i].in_use && xc->windows[i].x11_id == xid) {
            return &xc->windows[i];
        }
    }
    return NULL;
}

int translation_deliver_event(struct x11_client *xc,
                               const uint8_t *evt32,
                               uint32_t required_mask,
                               uint32_t event_window_xid) {
    if (xc == NULL || evt32 == NULL) return -1;
    struct x11_window *win = find_win(xc, event_window_xid);
    if (win == NULL) {
        /* The destination window is not tracked (e.g. the root, or a
         * window owned by another client). v1 has no inter-client
         * event delivery, so drop. */
        return 0;
    }
    if (required_mask != 0u && (win->event_mask & required_mask) == 0u) {
        /* Client did not select for this event class on this window. */
        return 0;
    }
    return x11_client_send_event(xc, (const struct x11_event_32 *)evt32);
}

/* ------------------------------------------------------------------ */
/* Ishizue -> X11                                                      */
/* ------------------------------------------------------------------ */

/* Find the first X11 client with a mapped top-level window. */
static struct x11_client *find_target(struct x11_client **clients, size_t n,
                                      struct x11_window **win_out) {
    for (size_t i = 0; i < n; i++) {
        if (clients[i] == NULL) continue;
        struct x11_window *w = x11_client_first_mapped(clients[i]);
        if (w != NULL) {
            if (win_out) *win_out = w;
            return clients[i];
        }
    }
    return NULL;
}

/* Fill the common input-event envelope. detail is event-specific
 * (keycode for KeyPress/Release, button number for ButtonPress/
 * Release, 0 for MotionNotify). */
static void fill_input_event(struct x11_event_32 *ev,
                             struct x11_client *xc,
                             struct x11_window *win,
                             uint8_t code, uint8_t detail) {
    memset(ev, 0, sizeof(*ev));
    ev->code   = code;
    ev->detail = detail;
    uint16_t seq = x11_client_next_sequence(xc);
    x11_put_u16(ev->sequence_number, seq, xc->byte_order);
    /* time = 0: the bridge does not yet track a clock. Real X11
     * clients tolerate this; some use it for double-click detection
     * and will see all clicks as "too fast", which is harmless for
     * the scaffold. */
    x11_put_u32(ev->root, X11_ROOT_WINDOW_ID, xc->byte_order);
    x11_put_u32(ev->event, win->x11_id, xc->byte_order);
    x11_put_u32(ev->child, 0u, xc->byte_order);  /* no child */
    x11_put_u16(ev->event_x, (uint16_t)win->w, xc->byte_order);
    x11_put_u16(ev->event_y, (uint16_t)win->h, xc->byte_order);
    x11_put_u16(ev->root_x, (uint16_t)win->w, xc->byte_order);
    x11_put_u16(ev->root_y, (uint16_t)win->h, xc->byte_order);
    /* state = 0 (no modifier state tracked yet). */
    ev->same_screen = 1;
}

int translation_forward_keyboard_key(struct x11_client **clients, size_t n,
                                      uint32_t keycode, bool pressed) {
    struct x11_window *win = NULL;
    struct x11_client *xc = find_target(clients, n, &win);
    if (xc == NULL) return -1;
    struct x11_event_32 ev;
    fill_input_event(&ev, xc, win,
                     pressed ? X11_EV_KEY_PRESS : X11_EV_KEY_RELEASE,
                     (uint8_t)(keycode & 0xffu));
    tr_log("debug", "forward key code=%u pressed=%d -> x11=%u",
           (unsigned)keycode, (int)pressed, (unsigned)win->x11_id);
    return x11_client_send_event(xc, &ev);
}

int translation_forward_pointer_motion(struct x11_client **clients, size_t n,
                                       int32_t x, int32_t y) {
    struct x11_window *win = NULL;
    struct x11_client *xc = find_target(clients, n, &win);
    if (xc == NULL) return -1;
    struct x11_event_32 ev;
    fill_input_event(&ev, xc, win, X11_EV_MOTION_NOTIFY, 0);
    x11_put_u16(ev.event_x, (uint16_t)x, xc->byte_order);
    x11_put_u16(ev.event_y, (uint16_t)y, xc->byte_order);
    x11_put_u16(ev.root_x, (uint16_t)x, xc->byte_order);
    x11_put_u16(ev.root_y, (uint16_t)y, xc->byte_order);
    tr_log("debug", "forward motion x=%d y=%d -> x11=%u",
           (int)x, (int)y, (unsigned)win->x11_id);
    return x11_client_send_event(xc, &ev);
}

int translation_forward_pointer_button(struct x11_client **clients, size_t n,
                                       uint32_t button, bool pressed) {
    struct x11_window *win = NULL;
    struct x11_client *xc = find_target(clients, n, &win);
    if (xc == NULL) return -1;
    struct x11_event_32 ev;
    fill_input_event(&ev, xc, win,
                     pressed ? X11_EV_BUTTON_PRESS : X11_EV_BUTTON_RELEASE,
                     (uint8_t)(button & 0xffu));
    tr_log("debug", "forward button=%u pressed=%d -> x11=%u",
           (unsigned)button, (int)pressed, (unsigned)win->x11_id);
    return x11_client_send_event(xc, &ev);
}
