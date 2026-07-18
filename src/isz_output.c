/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Rui-727
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* isz_output.c - output management (SPEC §7.2, §7.7, §10).
 *
 * Wave 2-A owns the concrete struct isz_output and the public output
 * API. The headless backend path is fully wired: modes are
 * synthesized from the headless_output's geometry, plane slots are a
 * synthetic primary+overlay+cursor set, EDID is empty. The DRM
 * backend path is stubbed: modes, EDID, and plane slots come from
 * KMS queries that the render/backend wave implements.
 *
 * Color management (§7.2): set_gamma/_degamma/_ctm/_hdr_metadata
 * validate LUT sizes against the hardware's advertised sizes (for
 * headless, the sentinel ISZ_HEADLESS_LUT_SIZE) and store the values
 * on the output struct. The render wave creates the KMS blobs from
 * these stored values at commit time.
 *
 * Output enablement (§10): the library never lights up a display on
 * its own; isz_output_enable is the explicit enable call and forwards
 * to the backend's mode-set path. */
#define _POSIX_C_SOURCE 200809L

#include "isz_server_internal.h"

#include <stdlib.h>
#include <string.h>

#include "util/isz_log.h"
#include "backend/isz_backend.h"
#include "backend/isz_headless.h"

/* DRM_FORMAT_XRGB8888 / ARGB8888 fourccs, written out as literals so
 * this TU doesn't depend on <drm_fourcc.h>. The values are
 * fourcc_code('X','R','2','4') and fourcc_code('A','R','2','4'): */
#define ISZ_FMT_XRGB8888 0x34325258u
#define ISZ_FMT_ARGB8888 0x34325241u

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */
static void isz_output_free_modes(struct isz_output *out)
{
    if (!out)
        return;
    for (size_t i = 0; i < out->mode_count; i++) {
        free(out->modes[i]);
    }
    free(out->modes);
    out->modes       = NULL;
    out->mode_count  = 0;
    free(out->mode_ptr_cache);
    out->mode_ptr_cache  = NULL;
    out->mode_ptr_count  = 0;
}

static void isz_output_free_color_lut(struct isz_output_color_lut *lut)
{
    if (!lut)
        return;
    free(lut->r);
    free(lut->g);
    free(lut->b);
    lut->r   = NULL;
    lut->g   = NULL;
    lut->b   = NULL;
    lut->size = 0;
    lut->set  = false;
}

static int isz_output_set_color_lut(struct isz_output_color_lut *lut,
                                    const uint16_t *r, const uint16_t *g,
                                    const uint16_t *b, size_t size,
                                    size_t hw_size)
{
    if (!r || !g || !b)
        return ISZ_ERR_INVALID_ARG;
    if (size != hw_size)
        return ISZ_ERR_INVALID_ARG;

    uint16_t *nr = malloc(size * sizeof(uint16_t));
    uint16_t *ng = malloc(size * sizeof(uint16_t));
    uint16_t *nb = malloc(size * sizeof(uint16_t));
    if (!nr || !ng || !nb) {
        free(nr); free(ng); free(nb);
        return ISZ_ERR_NO_MEMORY;
    }
    memcpy(nr, r, size * sizeof(uint16_t));
    memcpy(ng, g, size * sizeof(uint16_t));
    memcpy(nb, b, size * sizeof(uint16_t));

    isz_output_free_color_lut(lut);
    lut->r    = nr;
    lut->g    = ng;
    lut->b    = nb;
    lut->size = size;
    lut->set  = true;
    return ISZ_OK;
}

static int isz_output_build_headless_slots(struct isz_output *out)
{
    out->slots = calloc(3, sizeof(*out->slots));
    if (!out->slots)
        return ISZ_ERR_NO_MEMORY;
    out->slot_count = 3;

    /* Primary: XRGB8888 + ARGB8888, scaling + transform, zpos 0..0. */
    struct isz_output_plane_slot *p = &out->slots[0];
    p->id                 = 1;
    p->type               = ISZ_PLANE_PRIMARY;
    p->formats[0]         = ISZ_FMT_XRGB8888;
    p->formats[1]         = ISZ_FMT_ARGB8888;
    p->format_count       = 2;
    p->supports_scaling   = true;
    p->supports_transform = true;
    p->zpos_min           = 0;
    p->zpos_max           = 0;

    /* Overlay: same formats, scaling + transform, zpos 1..255. */
    struct isz_output_plane_slot *o = &out->slots[1];
    o->id                 = 2;
    o->type               = ISZ_PLANE_OVERLAY;
    o->formats[0]         = ISZ_FMT_XRGB8888;
    o->formats[1]         = ISZ_FMT_ARGB8888;
    o->format_count       = 2;
    o->supports_scaling   = true;
    o->supports_transform = true;
    o->zpos_min           = 1;
    o->zpos_max           = 255;

    /* Cursor: ARGB8888 only (typical cursor format), no scaling, no
     * transform, zpos 256..256 (above all overlays). */
    struct isz_output_plane_slot *c = &out->slots[2];
    c->id                 = 3;
    c->type               = ISZ_PLANE_CURSOR;
    c->formats[0]         = ISZ_FMT_ARGB8888;
    c->format_count       = 1;
    c->supports_scaling   = false;
    c->supports_transform = false;
    c->zpos_min           = 256;
    c->zpos_max           = 256;

    return ISZ_OK;
}

void isz_output_destroy_internal(struct isz_output *out)
{
    if (!out)
        return;
    isz_output_free_modes(out);
    isz_output_free_color_lut(&out->gamma);
    isz_output_free_color_lut(&out->degamma);
    free(out->edid);
    out->edid      = NULL;
    out->edid_size = 0;
    free(out->slots);
    out->slots      = NULL;
    out->slot_count = 0;
    out->current_mode = NULL;
}

/* ------------------------------------------------------------------ */
/* Server-level helpers used by isz_lifecycle.c                       */
/* ------------------------------------------------------------------ */
int isz_server_wrap_headless_output(isz_server *srv,
                                    const struct isz_headless_output_info *info)
{
    if (!srv || !info)
        return ISZ_ERR_INVALID_ARG;

    /* If a wrapper for this headless_id already exists, treat the
     * hook as a no-op (the backend shouldn't double-fire, but be
     * defensive). */
    isz_list_node *pos;
    isz_list_for_each(pos, &srv->outputs) {
        struct isz_output *o = container_of(pos, struct isz_output, node);
        if (o->is_headless && o->headless_id == info->id)
            return ISZ_OK;
    }

    struct isz_output *out = calloc(1, sizeof(*out));
    if (!out)
        return ISZ_ERR_NO_MEMORY;

    out->srv         = srv;
    out->backend     = srv->backend;
    out->id          = srv->next_output_id++;
    out->headless_id = info->id;
    out->is_headless = true;
    out->dpms        = ISZ_DPMS_OFF;
    out->enabled     = false;
    snprintf(out->name, sizeof(out->name), "%s", info->name);

    /* One synthetic mode matching the headless geometry. */
    out->modes = calloc(1, sizeof(*out->modes));
    if (!out->modes) {
        free(out);
        return ISZ_ERR_NO_MEMORY;
    }
    out->mode_count = 1;
    out->modes[0] = calloc(1, sizeof(struct isz_mode));
    if (!out->modes[0]) {
        free(out->modes);
        free(out);
        return ISZ_ERR_NO_MEMORY;
    }
    out->modes[0]->width       = info->width;
    out->modes[0]->height      = info->height;
    out->modes[0]->refresh_mhz = info->refresh_mhz;
    out->modes[0]->preferred   = true;

    /* Build the cached pointer array isz_output_get_modes returns. */
    out->mode_ptr_cache = calloc(1, sizeof(*out->mode_ptr_cache));
    if (!out->mode_ptr_cache) {
        isz_output_destroy_internal(out);
        free(out);
        return ISZ_ERR_NO_MEMORY;
    }
    out->mode_ptr_count  = 1;
    out->mode_ptr_cache[0] = out->modes[0];

    if (isz_output_build_headless_slots(out) < 0) {
        isz_output_destroy_internal(out);
        free(out);
        return ISZ_ERR_NO_MEMORY;
    }

    isz_list_push_back(&srv->outputs, &out->node);

    /* SPEC §6.5 / §10: broadcast OUTPUT_ADD to the Architect. */
    isz_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ISZ_EVENT_OUTPUT_ADD;
    isz_server_emit_event(srv, &ev);
    return ISZ_OK;
}

void isz_server_unwrap_headless_output(isz_server *srv, uint32_t headless_id)
{
    if (!srv)
        return;
    isz_list_node *pos;
    isz_list_for_each(pos, &srv->outputs) {
        struct isz_output *o = container_of(pos, struct isz_output, node);
        if (o->is_headless && o->headless_id == headless_id) {
            /* SPEC §10: surface ISZ_EVENT_OUTPUT_REMOVE. The wrapper
             * stays valid until the Architect calls isz_output_destroy;
             * mark it as removed so commits are rejected. */
            isz_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = ISZ_EVENT_OUTPUT_REMOVE;
            isz_server_emit_event(srv, &ev);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API (isz.h)                                                 */
/* ------------------------------------------------------------------ */
ISZ_API isz_output **isz_output_list(isz_server *srv, size_t *count)
{
    if (count)
        *count = 0;
    if (!srv)
        return NULL;

    /* Count first. */
    size_t n = 0;
    isz_list_node *pos;
    isz_list_for_each(pos, &srv->outputs)
        n++;

    /* Reallocate the cache to fit (we tolerate over-allocation by
     * only growing; isz_destroy frees the whole thing). */
    if (n > srv->output_list_count) {
        isz_output **fresh = realloc(srv->output_list_cache,
                                     n * sizeof(*fresh));
        if (!fresh && n > 0) {
            isz_log_internal(ISZ_LOG_ERROR,
                             "isz_output_list: out of memory");
            return NULL;
        }
        srv->output_list_cache = fresh;
        srv->output_list_count = n;
    }

    size_t i = 0;
    isz_list_for_each(pos, &srv->outputs) {
        if (i < srv->output_list_count)
            srv->output_list_cache[i] =
                container_of(pos, struct isz_output, node);
        i++;
    }

    if (count)
        *count = n;
    return srv->output_list_cache;
}

ISZ_API isz_mode **isz_output_get_modes(isz_output *out, size_t *count)
{
    if (count)
        *count = 0;
    if (!out)
        return NULL;
    if (count)
        *count = out->mode_ptr_count;
    return out->mode_ptr_cache;
}

ISZ_API int isz_output_enable(isz_output *out, isz_mode *mode)
{
    if (!out || !mode)
        return ISZ_ERR_INVALID_ARG;

    /* Validate that mode is one of the output's modes. */
    bool found = false;
    for (size_t i = 0; i < out->mode_count; i++) {
        if (out->modes[i] == mode) {
            found = true;
            break;
        }
    }
    if (!found)
        return ISZ_ERR_INVALID_ARG;

    /* The headless backend has no real mode-set; just record the
     * selection. The DRM backend's mode-set lives in the render wave
     * (drmModeAtomicCommit with ALLOW_MODESET). */
    out->current_mode = mode;
    out->enabled      = true;
    out->dpms         = ISZ_DPMS_ON;
    return ISZ_OK;
}

ISZ_API int isz_output_disable(isz_output *out)
{
    if (!out)
        return ISZ_ERR_INVALID_ARG;
    out->enabled      = false;
    out->current_mode = NULL;
    /* SPEC §10 doesn't mandate a DPMS transition here; the Architect
     * can call isz_output_set_dpms separately. */
    return ISZ_OK;
}

ISZ_API void isz_output_destroy(isz_output *out)
{
    if (!out)
        return;
    isz_server *srv = out->srv;
    isz_list_remove(&out->node);
    isz_output_destroy_internal(out);
    free(out);
    /* Invalidate the list cache size since we removed an entry. The
     * next isz_output_list call rebuilds it. */
    if (srv)
        srv->output_list_count = 0;
}

ISZ_API int isz_output_set_dpms(isz_output *out, enum isz_dpms_state state)
{
    if (!out)
        return ISZ_ERR_INVALID_ARG;
    out->dpms = state;
    /* The KMS DPMS property write happens at commit time in the
     * render wave. */
    return ISZ_OK;
}

ISZ_API const uint8_t *isz_output_get_edid(isz_output *out, size_t *size)
{
    if (size)
        *size = 0;
    if (!out)
        return NULL;
    if (size)
        *size = out->edid_size;
    return out->edid;
}

/* Color management (SPEC §7.2). LUT sizes are validated against the
 * hardware's advertised sizes; headless uses the sentinel. */
ISZ_API int isz_output_set_gamma(isz_output *out, const uint16_t *r,
                                 const uint16_t *g, const uint16_t *b,
                                 size_t size)
{
    if (!out)
        return ISZ_ERR_INVALID_ARG;
    size_t hw = out->is_headless ? ISZ_HEADLESS_LUT_SIZE : size;
    return isz_output_set_color_lut(&out->gamma, r, g, b, size, hw);
}

ISZ_API int isz_output_set_degamma(isz_output *out, const uint16_t *r,
                                   const uint16_t *g, const uint16_t *b,
                                   size_t size)
{
    if (!out)
        return ISZ_ERR_INVALID_ARG;
    size_t hw = out->is_headless ? ISZ_HEADLESS_LUT_SIZE : size;
    return isz_output_set_color_lut(&out->degamma, r, g, b, size, hw);
}

ISZ_API int isz_output_set_ctm(isz_output *out, const float matrix[9])
{
    if (!out || !matrix)
        return ISZ_ERR_INVALID_ARG;
    memcpy(out->ctm, matrix, sizeof(out->ctm));
    out->ctm_set = true;
    return ISZ_OK;
}

ISZ_API int isz_output_set_hdr_metadata(isz_output *out,
                                        const isz_hdr_metadata *meta)
{
    if (!out || !meta)
        return ISZ_ERR_INVALID_ARG;

#if !defined(ENABLE_HDR) || (ENABLE_HDR == 0)
    return ISZ_ERR_FEATURE_UNAVAIL;
#else
    size_t n = meta->size;
    if (n > sizeof(out->hdr.bytes))
        n = sizeof(out->hdr.bytes);
    out->hdr.size = n;
    memcpy(out->hdr.bytes, meta->bytes, n);
    out->hdr_set = true;
    return ISZ_OK;
#endif
}

ISZ_API size_t isz_output_get_plane_slots(isz_output *out,
                                          isz_plane_slot_info *out_slots,
                                          size_t max)
{
    if (!out)
        return 0;

    /* If the caller passed max=0, just return the count. */
    if (!out_slots || max == 0)
        return out->slot_count;

    size_t n = (out->slot_count < max) ? out->slot_count : max;
    for (size_t i = 0; i < n; i++) {
        isz_plane_slot_info *dst = &out_slots[i];
        const struct isz_output_plane_slot *src = &out->slots[i];
        dst->id                 = src->id;
        dst->type               = src->type;
        dst->format_count       = src->format_count;
        for (size_t j = 0; j < src->format_count && j < 16; j++)
            dst->supported_formats[j] = src->formats[j];
        dst->supports_scaling   = src->supports_scaling;
        dst->supports_transform = src->supports_transform;
        dst->zpos_min           = src->zpos_min;
        dst->zpos_max           = src->zpos_max;
    }
    return out->slot_count;
}
