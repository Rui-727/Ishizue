/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_surface.c -- surface lifecycle, buffer attach, damage, setters.
 * SPEC 7.6 (API skeleton), 6.6/6.7 (surface types), 8 (buffer attach
 * semantics).
 *
 * Wave 2 scope: the surface struct and all setters are real. KMS plane
 * reservation and the atomic commit happen in isz_commit.c.
 *
 * Surfaces are tracked in a module-static list (isz_render_surface_list)
 * since W2-A's struct isz_server does not yet carry a surfaces field.
 * Single-threaded per SPEC 5; the lifecycle wave can migrate this onto
 * the server struct. */

#define _POSIX_C_SOURCE 200809L

#include "isz_surface_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../util/isz_log.h"

/* ------------------------------------------------------------------ */
/* Global surface list                                                */
/* ------------------------------------------------------------------ */

static isz_list s_surface_list;
static bool s_surface_list_inited;

static void ensure_list_inited(void)
{
    if (!s_surface_list_inited) {
        isz_list_init(&s_surface_list);
        s_surface_list_inited = true;
    }
}

isz_list *isz_render_surface_list(void)
{
    ensure_list_inited();
    return &s_surface_list;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static void surface_init_nodes(struct isz_surface *s)
{
    /* Self-linked = not in any list. isz_list_remove on these is a
     * no-op, so destroy is safe whether or not the surface was linked. */
    s->server_node.prev = &s->server_node;
    s->server_node.next = &s->server_node;
    s->parent_node.prev = &s->parent_node;
    s->parent_node.next = &s->parent_node;
    isz_list_init(&s->children);
}

ISZ_API isz_surface *isz_surface_create(isz_server *srv)
{
    if (!srv) return NULL;
    struct isz_surface *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->srv = srv;
    s->kind = ISZ_SURFACE_NORMAL;
    s->plane_slot = -1;
    s->transform = ISZ_TRANSFORM_NORMAL;
    s->owning_conn = NULL;
    s->object_id = 0;
    surface_init_nodes(s);

    ensure_list_inited();
    isz_list_push_back(&s_surface_list, &s->server_node);

    return s;
}

ISZ_API void isz_surface_destroy(isz_surface *surf)
{
    if (!surf) return;

    isz_list_remove(&surf->server_node);
    if (surf->kind == ISZ_SURFACE_SUBSURFACE)
        isz_list_remove(&surf->parent_node);

    if (surf->plane_slot_set && surf->output)
        isz_plane_slot_release(surf->output, surf->plane_slot, surf);

    if (surf->current) {
        isz_buffer_release(surf->current);
        isz_buffer_unref(surf->current);
    }
    if (surf->pending_release) {
        isz_buffer_release(surf->pending_release);
        isz_buffer_unref(surf->pending_release);
    }

    free(surf->damage);
    /* Children are independent surfaces; not freed here. */
    free(surf);
}

/* ------------------------------------------------------------------ */
/* Buffer attach / detach (SPEC 8)                                    */
/* ------------------------------------------------------------------ */

ISZ_API int isz_surface_attach_buffer(isz_surface *surf, int dmabuf_fd,
                                      isz_buffer_desc *desc)
{
    if (!surf) {
        if (dmabuf_fd >= 0) close(dmabuf_fd);
        return ISZ_ERR_INVALID_ARG;
    }
    if (dmabuf_fd < 0 || !desc)
        return ISZ_ERR_INVALID_ARG;

    struct isz_buffer *buf = NULL;
    int rc = isz_buffer_import(surf->client_id, dmabuf_fd, desc, &buf);
    if (rc < 0)
        return rc;  /* import closed the fd on failure */

    /* Double-buffer swap. If pending_release still holds a buffer, its
     * fence should have signalled by now (headless: always; DRM: checked
     * by the backend wave's fence path). Release it. */
    if (surf->pending_release) {
        isz_buffer_release(surf->pending_release);
        isz_buffer_unref(surf->pending_release);
        surf->pending_release = NULL;
    }

    surf->pending_release = surf->current;
    surf->current = buf;

    /* SPEC 8: logical size comes from the buffer unless set_size was
     * called. width == 0 means "not set". */
    if (surf->width == 0 || surf->height == 0) {
        surf->width = (int32_t)buf->width;
        surf->height = (int32_t)buf->height;
    }

    return ISZ_OK;
}

ISZ_API int isz_surface_detach_buffer(isz_surface *surf)
{
    if (!surf) return ISZ_ERR_INVALID_ARG;

    if (surf->current) {
        isz_buffer_release(surf->current);
        isz_buffer_unref(surf->current);
        surf->current = NULL;
    }
    /* Leave pending_release in place; its fence may still be outstanding. */
    return ISZ_OK;
}

/* ------------------------------------------------------------------ */
/* Damage (SPEC 7.9)                                                  */
/* ------------------------------------------------------------------ */

ISZ_API int isz_surface_damage(isz_surface *surf, isz_rect *rects,
                               size_t count)
{
    if (!surf) return ISZ_ERR_INVALID_ARG;
    if (count == 0) return ISZ_OK;
    if (!rects) return ISZ_ERR_INVALID_ARG;

    if (surf->damage_count + count > ISZ_SURFACE_MAX_DAMAGE)
        return ISZ_ERR_RESOURCE_LIMIT;

    if (surf->damage_count + count > surf->damage_cap) {
        size_t ncap = surf->damage_cap ? surf->damage_cap : 8;
        while (ncap < surf->damage_count + count)
            ncap *= 2;
        isz_rect *nd = realloc(surf->damage, ncap * sizeof(*nd));
        if (!nd) return ISZ_ERR_NO_MEMORY;
        surf->damage = nd;
        surf->damage_cap = ncap;
    }

    memcpy(surf->damage + surf->damage_count, rects, count * sizeof(*rects));
    surf->damage_count += count;
    return ISZ_OK;
}

/* ------------------------------------------------------------------ */
/* Setters                                                            */
/* ------------------------------------------------------------------ */

ISZ_API int isz_surface_set_output(isz_surface *surf, isz_output *out)
{
    if (!surf || !out) return ISZ_ERR_INVALID_ARG;
    surf->output = out;
    return ISZ_OK;
}

ISZ_API int isz_surface_clear_output(isz_surface *surf)
{
    if (!surf) return ISZ_ERR_INVALID_ARG;
    if (surf->plane_slot_set && surf->output)
        isz_plane_slot_release(surf->output, surf->plane_slot, surf);
    surf->output = NULL;
    return ISZ_OK;
}

ISZ_API int isz_surface_set_position(isz_surface *surf, int x, int y)
{
    if (!surf) return ISZ_ERR_INVALID_ARG;
    surf->x = x;
    surf->y = y;
    return ISZ_OK;
}

ISZ_API int isz_surface_set_size(isz_surface *surf, int width, int height)
{
    if (!surf) return ISZ_ERR_INVALID_ARG;
    if (width <= 0 || height <= 0) return ISZ_ERR_INVALID_ARG;
    surf->width = width;
    surf->height = height;
    return ISZ_OK;
}

ISZ_API int isz_surface_set_plane_type(isz_surface *surf, int type)
{
    if (!surf) return ISZ_ERR_INVALID_ARG;
    if (type < 0 || type > (int)ISZ_PLANE_CURSOR) return ISZ_ERR_INVALID_ARG;
    surf->plane_type = (enum isz_plane_type)type;
    surf->plane_type_set = true;
    return ISZ_OK;
}

ISZ_API int isz_surface_set_plane_slot(isz_surface *surf, int slot)
{
    if (!surf) return ISZ_ERR_INVALID_ARG;
    if (slot < 0) return ISZ_ERR_INVALID_ARG;
    surf->plane_slot = slot;
    surf->plane_slot_set = true;
    return ISZ_OK;
}

ISZ_API int isz_surface_set_zpos(isz_surface *surf, int zpos)
{
    if (!surf) return ISZ_ERR_INVALID_ARG;
    surf->zpos = zpos;
    return ISZ_OK;
}

ISZ_API int isz_surface_set_transform(isz_surface *surf, enum isz_transform t)
{
    if (!surf) return ISZ_ERR_INVALID_ARG;
    if (t < ISZ_TRANSFORM_NORMAL || t > ISZ_TRANSFORM_REFLECT_Y)
        return ISZ_ERR_INVALID_ARG;
    surf->transform = t;
    return ISZ_OK;
}

/* ------------------------------------------------------------------ */
/* Subsurfaces (6.6)                                                  */
/* ------------------------------------------------------------------ */

ISZ_API isz_surface *isz_surface_create_subsurface(isz_surface *parent)
{
    if (!parent) return NULL;
    isz_surface *sub = isz_surface_create(parent->srv);
    if (!sub) return NULL;
    sub->kind = ISZ_SURFACE_SUBSURFACE;
    sub->parent = parent;
    isz_list_push_back(&parent->children, &sub->parent_node);
    return sub;
}

ISZ_API int isz_surface_set_subsurface_flags(isz_surface *sub, uint32_t flags)
{
    if (!sub) return ISZ_ERR_INVALID_ARG;
    if (sub->kind != ISZ_SURFACE_SUBSURFACE) return ISZ_ERR_INVALID_ARG;
    sub->subsurface_flags = flags;
    return ISZ_OK;
}

/* ------------------------------------------------------------------ */
/* Popups (6.7)                                                       */
/* ------------------------------------------------------------------ */

ISZ_API isz_surface *isz_surface_create_popup(isz_surface *parent,
                                              int x, int y)
{
    if (!parent) return NULL;
    isz_surface *pop = isz_surface_create(parent->srv);
    if (!pop) return NULL;
    pop->kind = ISZ_SURFACE_POPUP;
    pop->parent = parent;
    pop->popup_x = x;
    pop->popup_y = y;
    return pop;
}

/* ------------------------------------------------------------------ */
/* Layer-shell (6.7)                                                  */
/* ------------------------------------------------------------------ */

ISZ_API isz_surface *isz_surface_create_layer(isz_output *out,
                                              enum isz_layer layer)
{
    if (!out) return NULL;
    isz_server *srv = out->srv;
    if (!srv) return NULL;
    isz_surface *s = isz_surface_create(srv);
    if (!s) return NULL;
    s->kind = ISZ_SURFACE_LAYER;
    s->layer = layer;
    s->output = out;
    return s;
}
