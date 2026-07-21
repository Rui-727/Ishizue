/* SPDX-License-Identifier: MIT */
/* tinyisz/input.h - keyboard/pointer handling, keybindings, and the
 * central WM context struct. */
#ifndef TINYISZ_INPUT_H
#define TINYISZ_INPUT_H

#include <ishizue/isz.h>

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "layout.h"
#include "window.h"

#define TINYISZ_MAX_OUTPUTS 8

/* Central WM state. Defined here because input.c consumes it
 * directly; tinyisz.c includes this header to allocate it. */
struct tinyisz_ctx {
    isz_server *srv;
    isz_seat    *seat;
    isz_output  *outputs[TINYISZ_MAX_OUTPUTS];
    size_t       n_outputs;
    struct tinyisz_window_list windows;
    struct tinyisz_layout      layout;
    int      x11_display, output_w, output_h;
    uint32_t mods_depressed, mods_latched, mods_locked;
    int      pointer_x, pointer_y;
    bool     session_active;
    volatile sig_atomic_t *exit_flag;
};

void tinyisz_ctx_init(struct tinyisz_ctx *c, isz_server *srv, isz_seat *seat,
                      int output_w, int output_h, int x11_display,
                      volatile sig_atomic_t *exit_flag);
void tinyisz_ctx_destroy(struct tinyisz_ctx *c);

void tinyisz_ctx_add_output(struct tinyisz_ctx *c, isz_output *out);
void tinyisz_ctx_retile(struct tinyisz_ctx *c);
void tinyisz_ctx_reap_children(struct tinyisz_ctx *c);
void tinyisz_ctx_on_client_connect(struct tinyisz_ctx *c);
void tinyisz_ctx_on_client_disconnect(struct tinyisz_ctx *c);

/* Per-event handlers, registered with isz_add_listener. */
void tinyisz_input_keyboard_key(struct tinyisz_ctx *ctx, const isz_event *ev);
void tinyisz_input_keyboard_modifiers(struct tinyisz_ctx *ctx,
                                      const isz_event *ev);
void tinyisz_input_pointer_motion(struct tinyisz_ctx *ctx, const isz_event *ev);

#endif /* TINYISZ_INPUT_H */
