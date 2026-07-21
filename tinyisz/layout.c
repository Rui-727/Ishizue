/* SPDX-License-Identifier: MIT */
/* tinyisz/layout.c - master/stack tiling layout.
 *
 * Window order in tinyisz_window_list: index 0 is the master, indices
 * 1..count-1 are the stack top-to-bottom. Master takes the left
 * master_ratio_pct of the width; stack splits the rest equally. */
#include "layout.h"
#include <ishizue/isz.h>
#include <stddef.h>

#define MASTER_RATIO_MIN 20
#define MASTER_RATIO_MAX 80
#define MASTER_RATIO_DEFAULT 50

void tinyisz_layout_init(struct tinyisz_layout *ly)
{
    *ly = (struct tinyisz_layout){ .master_ratio_pct = MASTER_RATIO_DEFAULT };
}

void tinyisz_layout_set_output(struct tinyisz_layout *ly, int w, int h)
{
    ly->output_w = w;
    ly->output_h = h;
}

void tinyisz_layout_adjust_master(struct tinyisz_layout *ly, int delta_pct)
{
    int v = ly->master_ratio_pct + delta_pct;
    ly->master_ratio_pct = (v < MASTER_RATIO_MIN) ? MASTER_RATIO_MIN :
                           (v > MASTER_RATIO_MAX) ? MASTER_RATIO_MAX : v;
}

void tinyisz_layout_apply(struct tinyisz_layout *ly,
                          struct tinyisz_window_list *l,
                          isz_seat *seat, isz_output *out)
{
    if (l->count == 0 || ly->output_w <= 0 || ly->output_h <= 0) {
        if (out) isz_commit(out, ISZ_COMMIT_NORMAL);
        return;
    }

    int master_w = (ly->output_w * ly->master_ratio_pct) / 100;
    int stack_w = ly->output_w - master_w;
    size_t n_stack = l->count - 1;
    int stack_h = (n_stack > 0) ? (ly->output_h / (int)n_stack) : 0;

    for (size_t i = 0; i < l->count; i++) {
        struct tinyisz_window *w = &l->wins[i];
        int x, y, ww, hh;
        if (i == 0) {
            x = 0; y = 0; ww = master_w; hh = ly->output_h;
        } else {
            x = master_w;
            y = (int)(i - 1) * stack_h;
            ww = stack_w;
            hh = (i == l->count - 1) ? (ly->output_h - y) : stack_h;
        }
        w->x = x; w->y = y; w->w = ww; w->h = hh;
        int z = w->focused ? (int)l->count + 1 :
               (i == 0) ? (int)l->count : (int)(l->count - i);
        w->zpos = z;
        isz_surface_set_position(w->surf, x, y);
        isz_surface_set_size(w->surf, ww, hh);
        isz_surface_set_zpos(w->surf, z);
    }

    if (seat) {
        struct tinyisz_window *f = tinyisz_wins_focused(l);
        isz_seat_set_keyboard_focus(seat, f ? f->surf : NULL);
    }

    if (out) isz_commit(out, ISZ_COMMIT_NORMAL);
}
