/* SPDX-License-Identifier: MIT */
/* tinyisz/input.c - keyboard/pointer handling and keybindings.
 *
 * Keycodes are evdev keycodes (linux/input-event-codes.h). Modifier
 * bits: Mod4 (Super) = 0x40, Shift = 0x01, hardcoded because the
 * headless backend compiles the standard US xkbcommon layout. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "input.h"

#include <ishizue/isz.h>

#include <linux/input-event-codes.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOD_SHIFT 0x01u
#define MOD_SUPER 0x40u

void tinyisz_ctx_init(struct tinyisz_ctx *c, isz_server *srv, isz_seat *seat,
                      int output_w, int output_h, int x11_display,
                      volatile sig_atomic_t *exit_flag)
{
    *c = (struct tinyisz_ctx){
        .srv = srv, .seat = seat,
        .output_w = output_w, .output_h = output_h,
        .x11_display = x11_display,
        .exit_flag = exit_flag,
        .session_active = true,
    };
    tinyisz_wins_init(&c->windows);
    tinyisz_layout_init(&c->layout);
    tinyisz_layout_set_output(&c->layout, output_w, output_h);
}

void tinyisz_ctx_destroy(struct tinyisz_ctx *c)
{
    tinyisz_wins_destroy_all(&c->windows);
}

static void retile(struct tinyisz_ctx *c)
{
    isz_output *out = (c->n_outputs > 0) ? c->outputs[0] : NULL;
    tinyisz_layout_apply(&c->layout, &c->windows, c->seat, out);
}

/* Fork xterm with DISPLAY set so it connects to the bridge's X11
 * socket. If xterm is not on PATH, the child prints a notice and
 * exits 127; the parent does not block. */
static void spawn_terminal(struct tinyisz_ctx *c)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }
    if (pid == 0) {
        char disp[16];
        snprintf(disp, sizeof(disp), ":%d", c->x11_display);
        setenv("DISPLAY", disp, 1);
        char *argv[] = { "xterm", NULL };
        execvp("xterm", argv);
        fprintf(stderr, "tinyisz: would spawn xterm (DISPLAY=%s)\n", disp);
        _exit(127);
    }
}

static void handle_key(struct tinyisz_ctx *c, uint32_t keycode, bool press)
{
    if (!press) return;
    uint32_t mods = c->mods_depressed | c->mods_latched;
    if (!(mods & MOD_SUPER)) return;
    bool shift = (mods & MOD_SHIFT) != 0;

    if (shift) {
        switch (keycode) {
        case KEY_Q:   tinyisz_wins_remove_focused(&c->windows); retile(c); return;
        case KEY_ESC: if (c->exit_flag) *c->exit_flag = 1; return;
        }
        return;
    }
    switch (keycode) {
    case KEY_ENTER: spawn_terminal(c); break;
    case KEY_J:     tinyisz_wins_cycle(&c->windows, 1); retile(c); break;
    case KEY_K:     tinyisz_wins_cycle(&c->windows, -1); retile(c); break;
    case KEY_H:     tinyisz_layout_adjust_master(&c->layout, -5); retile(c); break;
    case KEY_L:     tinyisz_layout_adjust_master(&c->layout, 5); retile(c); break;
    default:
        if (keycode >= KEY_1 && keycode <= KEY_9) {
            tinyisz_wins_focus_index(&c->windows, (size_t)(keycode - KEY_1));
            retile(c);
        }
    }
}

void tinyisz_input_keyboard_key(struct tinyisz_ctx *c, const isz_event *ev)
{
    uint32_t keycode = 0;
    bool press = false;
    if (isz_event_get_keyboard_key(ev, &keycode, &press) == ISZ_OK)
        handle_key(c, keycode, press);
}

void tinyisz_input_keyboard_modifiers(struct tinyisz_ctx *c, const isz_event *ev)
{
    uint32_t d = 0, l = 0, lk = 0, g = 0;
    if (isz_event_get_keyboard_modifiers(ev, &d, &l, &lk, &g) == ISZ_OK) {
        c->mods_depressed = d;
        c->mods_latched   = l;
        c->mods_locked    = lk;
    }
}

void tinyisz_input_pointer_motion(struct tinyisz_ctx *c, const isz_event *ev)
{
    double dx = 0, dy = 0, ax = 0, ay = 0;
    if (isz_event_get_pointer_motion(ev, &dx, &dy, &ax, &ay) != ISZ_OK)
        return;
    /* Prefer absolute coordinates when the device supplies them;
     * otherwise integrate the relative delta. */
    if (ax != 0.0 || ay != 0.0) {
        c->pointer_x = (int)ax;
        c->pointer_y = (int)ay;
    } else {
        c->pointer_x += (int)dx;
        c->pointer_y += (int)dy;
    }
    if (c->output_w > 0)
        c->pointer_x = (c->pointer_x < 0) ? 0 :
            (c->pointer_x >= c->output_w) ? c->output_w - 1 : c->pointer_x;
    if (c->output_h > 0)
        c->pointer_y = (c->pointer_y < 0) ? 0 :
            (c->pointer_y >= c->output_h) ? c->output_h - 1 : c->pointer_y;

    /* Focus-follows-mouse. */
    struct tinyisz_window *w = tinyisz_wins_at_point(&c->windows,
                                                     c->pointer_x,
                                                     c->pointer_y);
    if (w && !w->focused) {
        tinyisz_wins_focus_index(&c->windows,
                                 (size_t)(w - c->windows.wins));
        retile(c);
    }
}

void tinyisz_ctx_add_output(struct tinyisz_ctx *c, isz_output *out)
{
    if (c->n_outputs >= TINYISZ_MAX_OUTPUTS)
        return;
    c->outputs[c->n_outputs++] = out;
    if (c->n_outputs == 1)
        tinyisz_layout_apply(&c->layout, &c->windows, c->seat, out);
}

void tinyisz_ctx_retile(struct tinyisz_ctx *c) { retile(c); }

void tinyisz_ctx_reap_children(struct tinyisz_ctx *c)
{
    (void)c;
    while (waitpid(-1, NULL, WNOHANG) > 0) { /* drain */ }
}

void tinyisz_ctx_on_client_connect(struct tinyisz_ctx *c)
{
    if (c->n_outputs == 0)
        return;
    tinyisz_wins_add(&c->windows, c->srv, c->outputs[0]);
    retile(c);
}

void tinyisz_ctx_on_client_disconnect(struct tinyisz_ctx *c)
{
    /* Remove the focused window as a stand-in for the disconnected
     * client's surface. The public API does not yet expose which
     * client owned which surface. */
    tinyisz_wins_remove_focused(&c->windows);
    retile(c);
}
