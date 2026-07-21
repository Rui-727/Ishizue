/* SPDX-License-Identifier: MIT */
/* tinyisz/layout.h - master/stack tiling layout. */
#ifndef TINYISZ_LAYOUT_H
#define TINYISZ_LAYOUT_H

#include <ishizue/isz.h>

#include "window.h"

struct tinyisz_layout {
    int output_w;
    int output_h;
    int master_ratio_pct; /* 20..80, default 50 */
};

void tinyisz_layout_init(struct tinyisz_layout *ly);
void tinyisz_layout_set_output(struct tinyisz_layout *ly, int w, int h);

/* Recompute every window's (x, y, w, h, zpos) and push the new state
 * to the library: isz_surface_set_position, _set_size, _set_zpos on
 * each surface; isz_seat_set_keyboard_focus on the focused window;
 * isz_commit on the output. Safe to call with an empty window list. */
void tinyisz_layout_apply(struct tinyisz_layout *ly,
                          struct tinyisz_window_list *l,
                          isz_seat *seat, isz_output *out);

/* Adjust master_ratio_pct by delta_pct, clamped to [20, 80]. Does not
 * re-tile; caller invokes tinyisz_layout_apply afterwards. */
void tinyisz_layout_adjust_master(struct tinyisz_layout *ly, int delta_pct);

#endif /* TINYISZ_LAYOUT_H */
