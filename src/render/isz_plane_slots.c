/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_plane_slots.c -- plane slot reservation management. SPEC 7.7.
 *
 * W2-A owns the slot capability table (out->slots, populated by
 * isz_output_build_headless_slots for headless; DRM backend wave for
 * real hardware). That table is read-only capability data.
 *
 * This layer tracks which surface holds which slot at commit time. The
 * reservation registry is a module-static array keyed by (output, slot_id).
 * Single-threaded per SPEC 5. */

#include "isz_surface_internal.h"

#include <stdlib.h>
#include <string.h>

#include "../util/isz_log.h"

/* ------------------------------------------------------------------ */
/* Reservation registry                                               */
/* ------------------------------------------------------------------ */

#define ISZ_RESERVATION_MAX 64

static struct {
    isz_output  *out;
    int          slot_id;
    isz_surface *holder;
} s_resv[ISZ_RESERVATION_MAX];
static size_t s_resv_count;

static ssize_t resv_find(isz_output *out, int slot_id)
{
    for (size_t i = 0; i < s_resv_count; i++)
        if (s_resv[i].out == out && s_resv[i].slot_id == slot_id)
            return (ssize_t)i;
    return -1;
}

static bool resv_held_by(isz_output *out, int slot_id, isz_surface *surf)
{
    ssize_t idx = resv_find(out, slot_id);
    return idx >= 0 && s_resv[idx].holder == surf;
}

bool isz_plane_slot_held_by(isz_output *out, int slot_id, isz_surface *surf)
{
    return resv_held_by(out, slot_id, surf);
}

static int resv_set(isz_output *out, int slot_id, isz_surface *surf)
{
    ssize_t idx = resv_find(out, slot_id);
    if (idx >= 0) {
        s_resv[idx].holder = surf;
        return ISZ_OK;
    }
    if (s_resv_count >= ISZ_RESERVATION_MAX)
        return ISZ_ERR_RESOURCE_LIMIT;
    s_resv[s_resv_count].out = out;
    s_resv[s_resv_count].slot_id = slot_id;
    s_resv[s_resv_count].holder = surf;
    s_resv_count++;
    return ISZ_OK;
}

static void resv_clear(isz_output *out, int slot_id, isz_surface *surf)
{
    ssize_t idx = resv_find(out, slot_id);
    if (idx < 0) return;
    if (s_resv[idx].holder != surf) return;
    /* Compact: move the last entry down. */
    s_resv_count--;
    if ((size_t)idx != s_resv_count)
        s_resv[idx] = s_resv[s_resv_count];
}

/* ------------------------------------------------------------------ */
/* Slot lookup                                                        */
/* ------------------------------------------------------------------ */

const struct isz_output_plane_slot *
isz_plane_slot_get(isz_output *out, int slot_id)
{
    if (!out || !out->slots) return NULL;
    for (size_t i = 0; i < out->slot_count; i++)
        if (out->slots[i].id == slot_id)
            return &out->slots[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Assign / release                                                   */
/* ------------------------------------------------------------------ */

int isz_plane_slot_assign(isz_output *out, isz_surface *surf)
{
    if (!out || !surf) return ISZ_ERR_INVALID_ARG;
    if (!surf->plane_slot_set) return ISZ_ERR_SURFACE_NO_PLANE_SLOT;

    const struct isz_output_plane_slot *slot =
        isz_plane_slot_get(out, surf->plane_slot);
    if (!slot)
        return ISZ_ERR_PLANE_UNAVAIL;

    /* Idempotent: same surface re-committing keeps its slot. */
    if (resv_held_by(out, surf->plane_slot, surf))
        return slot->id;

    /* Slot held by a different surface. */
    ssize_t idx = resv_find(out, surf->plane_slot);
    if (idx >= 0 && s_resv[idx].holder != NULL)
        return ISZ_ERR_PLANE_UNAVAIL;

    if (slot->type != surf->plane_type)
        return ISZ_ERR_PLANE_UNAVAIL;

    if (surf->current) {
        bool fmt_ok = false;
        for (size_t j = 0; j < slot->format_count; j++) {
            if (slot->formats[j] == surf->current->format) {
                fmt_ok = true;
                break;
            }
        }
        if (!fmt_ok) return ISZ_ERR_PLANE_UNAVAIL;
    }

    int rc = resv_set(out, surf->plane_slot, surf);
    if (rc < 0) return rc;
    return slot->id;
}

void isz_plane_slot_release(isz_output *out, int slot_id, isz_surface *surf)
{
    if (!out) return;
    resv_clear(out, slot_id, surf);
}
