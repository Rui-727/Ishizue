/* SPDX-License-Identifier: MIT */
/* tinyisz/window.c - window tracking implementation. */
#include "window.h"
#include <ishizue/isz.h>

void tinyisz_wins_init(struct tinyisz_window_list *l)
{
    l->count = 0;
    l->focused_index = -1;
}

static void destroy_win(struct tinyisz_window *w)
{
    if (w->surf) { isz_surface_destroy(w->surf); w->surf = NULL; }
}

int tinyisz_wins_add(struct tinyisz_window_list *l, isz_server *srv,
                     isz_output *out)
{
    if (l->count >= TINYISZ_MAX_WINDOWS)
        return -1;
    isz_surface *s = isz_surface_create(srv);
    if (!s)
        return -1;
    /* v1: all windows share plane slot 0 on the primary plane. */
    isz_surface_set_plane_type(s, ISZ_PLANE_PRIMARY);
    isz_surface_set_plane_slot(s, 0);
    isz_surface_set_output(s, out);

    size_t i = l->count;
    l->wins[i] = (struct tinyisz_window){ .surf = s, .out = out };
    l->count++;

    /* Rotate the new entry to index 0 so it becomes the master; the
     * old master drops to index 1 (stack top). */
    if (i > 0) {
        struct tinyisz_window tmp = l->wins[i];
        for (size_t k = i; k > 0; k--)
            l->wins[k] = l->wins[k - 1];
        l->wins[0] = tmp;
    }

    tinyisz_wins_focus_index(l, 0);
    return 0;
}

void tinyisz_wins_remove_focused(struct tinyisz_window_list *l)
{
    if (l->count == 0 || l->focused_index < 0)
        return;
    size_t fi = (size_t)l->focused_index;
    destroy_win(&l->wins[fi]);
    /* Compact: shifting later entries down promotes the old stack
     * top (index 1) into index 0 when the master is removed. */
    for (size_t k = fi; k + 1 < l->count; k++)
        l->wins[k] = l->wins[k + 1];
    l->count--;
    l->focused_index = (l->count > 0) ? 0 : -1;
    if (l->focused_index >= 0)
        tinyisz_wins_focus_index(l, (size_t)l->focused_index);
}

struct tinyisz_window *tinyisz_wins_focused(struct tinyisz_window_list *l)
{
    if (l->focused_index < 0 || (size_t)l->focused_index >= l->count)
        return NULL;
    return &l->wins[l->focused_index];
}

void tinyisz_wins_focus_index(struct tinyisz_window_list *l, size_t i)
{
    if (l->count == 0) { l->focused_index = -1; return; }
    if (i >= l->count) i = l->count - 1;
    for (size_t k = 0; k < l->count; k++)
        l->wins[k].focused = false;
    l->wins[i].focused = true;
    l->focused_index = (int)i;
}

void tinyisz_wins_cycle(struct tinyisz_window_list *l, int dir)
{
    if (l->count == 0)
        return;
    int cur = (l->focused_index < 0) ? 0 : l->focused_index;
    int next = cur + dir;
    while (next < 0)
        next += (int)l->count;
    next %= (int)l->count;
    tinyisz_wins_focus_index(l, (size_t)next);
}

struct tinyisz_window *tinyisz_wins_at_point(struct tinyisz_window_list *l,
                                             int px, int py)
{
    for (size_t i = 0; i < l->count; i++) {
        struct tinyisz_window *w = &l->wins[i];
        if (px >= w->x && px < w->x + w->w &&
            py >= w->y && py < w->y + w->h)
            return w;
    }
    return NULL;
}

void tinyisz_wins_destroy_all(struct tinyisz_window_list *l)
{
    for (size_t i = 0; i < l->count; i++)
        destroy_win(&l->wins[i]);
    l->count = 0;
    l->focused_index = -1;
}
