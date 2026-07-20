/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* translation.h: X11 <-> Ishizue mapping logic.
 *
 * X11 -> Ishizue:
 *   translation_on_x11_create_window    -> isz_client_send_surface_create
 *                                          + set_position + set_size
 *   translation_on_x11_change_window_attributes
 *                                      -> no wire action; just updates
 *                                         event-mask / override-redirect
 *                                         / cursor in x11_window state
 *   translation_on_x11_configure_window -> set_position + set_size (+ zpos)
 *   translation_on_x11_map_window       -> set_plane_type + set_plane_slot
 *                                          + set_output + commit
 *   translation_on_x11_unmap_window     -> clear_output + commit
 *   translation_on_x11_destroy_window   -> surface_destroy
 *
 * Ishizue -> X11:
 *   translation_forward_keyboard_key    -> X11 KeyPress / KeyRelease
 *   translation_forward_pointer_motion  -> X11 MotionNotify
 *   translation_forward_pointer_button  -> X11 ButtonPress / ButtonRelease
 *
 * Event delivery: when MapNotify / UnmapNotify / ConfigureNotify /
 * DestroyNotify / PropertyNotify needs to be delivered, the bridge
 * sends a 32-byte X11 event to the originating client only if that
 * client selected for the relevant mask on the relevant window. The
 * bridge is a single-process WM (per W7-A decision doc); inter-client
 * event delivery arrives with the multi-client case. */

#ifndef TRANSLATION_H
#define TRANSLATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "x11_proto.h"

struct isz_client;
struct x11_client;
struct x11_window;

/* X11 -> Ishizue. Each takes the bridge's isz_client (for sending
 * wire messages) and the x11_client the request came in on. */
void translation_on_x11_create_window(struct isz_client *isz,
                                      struct x11_client *xc,
                                      struct x11_window *win,
                                      int32_t x, int32_t y,
                                      int32_t w, int32_t h);
void translation_on_x11_map_window(struct isz_client *isz,
                                   struct x11_client *xc,
                                   struct x11_window *win);
void translation_on_x11_unmap_window(struct isz_client *isz,
                                     struct x11_client *xc,
                                     struct x11_window *win);
void translation_on_x11_configure_window(struct isz_client *isz,
                                         struct x11_client *xc,
                                         struct x11_window *win,
                                         int32_t x, int32_t y,
                                         int32_t w, int32_t h);
void translation_on_x11_destroy_window(struct isz_client *isz,
                                       struct x11_client *xc,
                                       struct x11_window *win);

/* Generate and deliver an X11 event to the originating client, if
 * that client selected for the relevant mask on the relevant window.
 * Each takes the (already-built) 32-byte event buffer and the event
 * mask bit that must be set on the destination window for the event
 * to be delivered. The event's `event` field names the destination
 * window; the bridge looks it up in xc's window table and checks the
 * mask. Returns 0 if delivered (or mask not selected, which is not
 * an error), -1 on hard send error. */
int  translation_deliver_event(struct x11_client *xc,
                               const uint8_t *evt32,
                               uint32_t required_mask,
                               uint32_t event_window_xid);

/* Ishizue -> X11. Each takes the array of X11 clients the bridge
 * currently holds and routes the event to the first mapped window.
 * Returns 0 if delivered, -1 if no client had a mapped window. */
int  translation_forward_keyboard_key(struct x11_client **clients, size_t n,
                                      uint32_t keycode, bool pressed);
int  translation_forward_pointer_motion(struct x11_client **clients, size_t n,
                                        int32_t x, int32_t y);
int  translation_forward_pointer_button(struct x11_client **clients, size_t n,
                                        uint32_t button, bool pressed);

#endif /* TRANSLATION_H */
