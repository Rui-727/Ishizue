/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* translation.h: X11 <-> Ishizue mapping logic.
 *
 * X11 -> Ishizue:
 *   translation_on_x11_create_window    -> isz_client_send_surface_create
 *   translation_on_x11_configure_window -> set_position + set_size
 *   translation_on_x11_map_window       -> set_output + commit
 *   translation_on_x11_unmap_window     -> (stub) clear_output + commit
 *   translation_on_x11_destroy_window   -> (stub) surface_destroy
 *
 * Ishizue -> X11:
 *   translation_forward_keyboard_key    -> X11 KeyPress / KeyRelease
 *   translation_forward_pointer_motion  -> X11 MotionNotify
 *   translation_forward_pointer_button  -> X11 ButtonPress / ButtonRelease
 *
 * Input-event routing: the bridge maintains no focus tracking yet.
 * Forwarded input goes to the first mapped top-level window across
 * all X11 clients. Real focus policy belongs in the Architect
 * (SPEC §1) and would arrive here as a separate "focus surface id"
 * field on the input event once the protocol formalizes it. */

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
