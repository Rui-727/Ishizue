/* SPDX-License-Identifier: MIT */
/* tinyisz/window.h - window (top-level surface) tracking. */
#ifndef TINYISZ_WINDOW_H
#define TINYISZ_WINDOW_H

#include <ishizue/isz.h>

#include <stdbool.h>
#include <stddef.h>

#define TINYISZ_MAX_WINDOWS 32

/* A tracked top-level surface plus the metadata the layout engine
 * needs. The isz_surface is created and owned by tinyisz because the
 * public API does not yet expose a surface-create event from the
 * bridge; see README.md for the limitation. */
struct tinyisz_window {
    isz_surface *surf;
    isz_output  *out;
    int x, y, w, h;
    int zpos;
    bool focused;
};

struct tinyisz_window_list {
    struct tinyisz_window wins[TINYISZ_MAX_WINDOWS];
    size_t count;
    int focused_index; /* -1 when none */
};

void tinyisz_wins_init(struct tinyisz_window_list *l);
int  tinyisz_wins_add(struct tinyisz_window_list *l, isz_server *srv,
                      isz_output *out);
void tinyisz_wins_remove_focused(struct tinyisz_window_list *l);
struct tinyisz_window *tinyisz_wins_focused(struct tinyisz_window_list *l);
void tinyisz_wins_focus_index(struct tinyisz_window_list *l, size_t i);
void tinyisz_wins_cycle(struct tinyisz_window_list *l, int dir);
struct tinyisz_window *tinyisz_wins_at_point(struct tinyisz_window_list *l,
                                             int px, int py);
void tinyisz_wins_destroy_all(struct tinyisz_window_list *l);

#endif /* TINYISZ_WINDOW_H */
