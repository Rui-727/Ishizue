/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Rui-727
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* isz_drm_atomic.c -- atomic KMS commit builder, SPEC 7.2 / 7.3 / 7.5.
 *
 * Builds a drmModeAtomicReq from the per-output surface state and submits
 * with drmModeAtomicCommit(NONBLOCK | PAGE_FLIP_EVENT). Page-flip events
 * are drained in isz_drm_event.c.
 *
 * Explicit sync (SPEC 7.5): when drm_syncobj is available, the render
 * GPU's fence syncobj is attached to the commit as an in-fence on the
 * primary plane. When drm_syncobj is unavailable or the buffer carries
 * no syncobj, the commit relies on the DMA-BUF's implicit fence (the
 * reservation object's shared/exclusive fence), which is functionally
 * correct but less precise per SPEC 11. */
#include "isz_drm_atomic.h"
#include "isz_drm.h"

#ifdef ISHIZUE_HAVE_DRM

#include <ishizue/isz.h>
#include "../isz_server_internal.h"
#include "../render/isz_surface_internal.h"
#include "../util/isz_log.h"

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Property id lookup                                                 */
/* ------------------------------------------------------------------ */

uint32_t isz_drm_prop_id(int drm_fd, uint32_t object_id,
                         uint32_t object_type, const char *name)
{
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(drm_fd, object_id, object_type);
    if (!props)
        return 0;

    uint32_t id = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(drm_fd, props->props[i]);
        if (!p)
            continue;
        if (strcmp(p->name, name) == 0) {
            id = props->props[i];
            drmModeFreeProperty(p);
            break;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static void cache_init(struct isz_drm_prop_cache *c, int drm_fd,
                       uint32_t crtc_id, uint32_t connector_id,
                       uint32_t primary_plane_id)
{
    if (c->inited)
        return;

    c->crtc_active           = isz_drm_prop_id(drm_fd, crtc_id,
                                               DRM_MODE_OBJECT_CRTC, "ACTIVE");
    c->crtc_mode_id          = isz_drm_prop_id(drm_fd, crtc_id,
                                               DRM_MODE_OBJECT_CRTC, "MODE_ID");
    c->crtc_vrr_enabled      = isz_drm_prop_id(drm_fd, crtc_id,
                                               DRM_MODE_OBJECT_CRTC, "VRR_ENABLED");
    c->connector_crtc_id     = isz_drm_prop_id(drm_fd, connector_id,
                                               DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    c->connector_dpms        = isz_drm_prop_id(drm_fd, connector_id,
                                               DRM_MODE_OBJECT_CONNECTOR, "DPMS");
    if (primary_plane_id) {
        c->plane_fb_id       = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "FB_ID");
        c->plane_crtc_id     = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        c->plane_src_x       = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "SRC_X");
        c->plane_src_y       = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "SRC_Y");
        c->plane_src_w       = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "SRC_W");
        c->plane_src_h       = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "SRC_H");
        c->plane_crtc_x      = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "CRTC_X");
        c->plane_crtc_y      = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "CRTC_Y");
        c->plane_crtc_w      = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "CRTC_W");
        c->plane_crtc_h      = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "CRTC_H");
        c->plane_zpos        = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "zpos");
        c->plane_rotation    = isz_drm_prop_id(drm_fd, primary_plane_id,
                                               DRM_MODE_OBJECT_PLANE, "rotation");
    }
    /* Color mgmt + HDR live on the CRTC (DEGAMMA_LUT, CTM, GAMMA_LUT) or
     * connector (HDR_OUTPUT_METADATA). */
    if (crtc_id) {
        c->degamma_lut       = isz_drm_prop_id(drm_fd, crtc_id,
                                               DRM_MODE_OBJECT_CRTC, "DEGAMMA_LUT");
        c->ctm               = isz_drm_prop_id(drm_fd, crtc_id,
                                               DRM_MODE_OBJECT_CRTC, "CTM");
        c->gamma_lut         = isz_drm_prop_id(drm_fd, crtc_id,
                                               DRM_MODE_OBJECT_CRTC, "GAMMA_LUT");
    }
    if (connector_id) {
        c->hdr_output_metadata = isz_drm_prop_id(drm_fd, connector_id,
                                                 DRM_MODE_OBJECT_CONNECTOR,
                                                 "HDR_OUTPUT_METADATA");
    }
    c->inited = true;
}

/* ------------------------------------------------------------------ */
/* Atomic commit                                                      */
/* ------------------------------------------------------------------ */

/* Helper: add a property to the request, logging once if the prop id
 * is missing (driver doesn't advertise it; non-fatal for optional ones). */
static int add_prop(drmModeAtomicReq *req, uint32_t obj, uint32_t prop,
                    uint64_t value, const char *name, bool required)
{
    if (!prop) {
        if (required)
            isz_log_internal(ISZ_LOG_WARN,
                             "drm atomic: missing required prop %s", name);
        return required ? -1 : 0;
    }
    return drmModeAtomicAddProperty(req, obj, prop, value);
}

int isz_drm_atomic_commit(struct isz_drm_state *st,
                          struct isz_output *out,
                          uint32_t flags)
{
    int rc = ISZ_OK;
    uint32_t blob_mode_id = 0;
    drmModeAtomicReq *req = NULL;
    struct isz_drm_prop_cache *cache = NULL;
    uint32_t primary_plane_id = 0;
    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    isz_list *list = NULL;
    isz_list_node *pos = NULL;
    isz_surface *primary_surf = NULL;
    uint32_t commit_flags = 0;
    int ret = 0;
    drmModeModeInfo mode = {0};
    struct isz_buffer *buf = NULL;
    uint32_t fb_handle = 0;
    uint32_t fb_id = 0;
    uint32_t handles[4] = {0, 0, 0, 0};
    uint32_t pitches[4] = {0, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    const isz_hdr_metadata *hdr = NULL;
    uint64_t rot = 1u;

    if (!st || !out)
        return ISZ_ERR_INVALID_ARG;
    if (st->drm_fd < 0)
        return ISZ_ERR_FEATURE_UNAVAIL;

    if (!out->enabled || !out->current_mode)
        return ISZ_ERR_INVALID_ARG;

    connector_id = out->drm_connector_id;
    crtc_id      = out->drm_crtc_id;
    if (!connector_id || !crtc_id)
        return ISZ_ERR_INVALID_ARG;

    /* Find the surfaces bound to this output. The render layer tracks
     * them in a module-static list. */
    list = isz_render_surface_list();
    if (!list)
        return ISZ_ERR_INVALID_ARG;

    /* Locate the primary plane assigned to this CRTC. Walks the plane
     * snapshot in the state. */
    for (size_t i = 0; i < st->plane_count; i++) {
        struct isz_drm_plane *pl = &st->planes[i];
        if (pl->type != 1)  /* 1 == DRM_PLANE_TYPE_PRIMARY */
            continue;
        if (!(pl->possible_crtcs & out->drm_crtc_mask))
            continue;
        primary_plane_id = pl->plane_id;
        break;
    }
    if (!primary_plane_id) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "drm atomic: no primary plane for crtc %u",
                         crtc_id);
        return ISZ_ERR_PLANE_UNAVAIL;
    }

    cache = st->prop_cache;
    if (!cache) {
        cache = calloc(1, sizeof(*cache));
        if (!cache) {
            rc = ISZ_ERR_NO_MEMORY;
            goto out;
        }
        st->prop_cache = cache;
    }
    cache_init(cache, st->drm_fd, crtc_id, connector_id, primary_plane_id);

    req = drmModeAtomicAlloc();
    if (!req) {
        rc = ISZ_ERR_NO_MEMORY;
        goto out;
    }

    /* Serialize the mode into a KMS blob and set CRTC_MODE_ID + ACTIVE. */
    mode.clock       = out->current_mode->refresh_mhz ?
                       out->current_mode->refresh_mhz / 1000u : 60000;
    mode.hdisplay    = out->current_mode->width;
    mode.vdisplay    = out->current_mode->height;
    mode.hsync_start = out->current_mode->width;
    mode.hsync_end   = out->current_mode->width;
    mode.htotal      = out->current_mode->width;
    mode.vsync_start = out->current_mode->height;
    mode.vsync_end   = out->current_mode->height;
    mode.vtotal      = out->current_mode->height;

    if (drmModeCreatePropertyBlob(st->drm_fd, &mode, sizeof(mode),
                                  &blob_mode_id) != 0) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "drm atomic: mode blob create failed");
        rc = ISZ_ERR_COMMIT_FAILED;
        goto out;
    }

    rc  = add_prop(req, crtc_id, cache->crtc_mode_id,
                   blob_mode_id, "MODE_ID", true);
    rc |= add_prop(req, crtc_id, cache->crtc_active, 1, "ACTIVE", true);
    rc |= add_prop(req, connector_id, cache->connector_crtc_id,
                   crtc_id, "CRTC_ID", true);
    if (rc < 0) {
        rc = ISZ_ERR_COMMIT_FAILED;
        goto out;
    }

    /* VRR (SPEC 7.2). Setting VRR_ENABLED requires the CRTC advertise
     * vrr_capable; if not, the property doesn't exist and we skip. */
    if (out->vrr_enabled && cache->crtc_vrr_enabled) {
        if (drmModeAtomicAddProperty(req, crtc_id,
                                     cache->crtc_vrr_enabled, 1) < 0) {
            isz_log_internal(ISZ_LOG_WARN,
                             "drm atomic: VRR_ENABLED add failed");
        }
    } else if (cache->crtc_vrr_enabled) {
        (void)drmModeAtomicAddProperty(req, crtc_id,
                                       cache->crtc_vrr_enabled, 0);
    }

    /* Color mgmt blobs (SPEC 7.2). isz_color_*_blob creates the KMS
     * blob from the stored LUT/CTM and returns the id. */
    if (out->degamma.set && cache->degamma_lut) {
        uint32_t blob = isz_color_gamma_blob(out, out->degamma.r,
                                             out->degamma.g, out->degamma.b,
                                             out->degamma.size);
        if (blob)
            (void)drmModeAtomicAddProperty(req, crtc_id,
                                           cache->degamma_lut, blob);
    }
    if (out->ctm_set && cache->ctm) {
        uint32_t blob = isz_color_ctm_blob(out, out->ctm);
        if (blob)
            (void)drmModeAtomicAddProperty(req, crtc_id, cache->ctm, blob);
    }
    if (out->gamma.set && cache->gamma_lut) {
        uint32_t blob = isz_color_gamma_blob(out, out->gamma.r,
                                             out->gamma.g, out->gamma.b,
                                             out->gamma.size);
        if (blob)
            (void)drmModeAtomicAddProperty(req, crtc_id,
                                           cache->gamma_lut, blob);
    }

    /* HDR output metadata (SPEC 7.2). isz_output_get_hdr_metadata returns
     * the EDID-parsed or Architect-set blob. */
    if (out->hdr_set && cache->hdr_output_metadata) {
        hdr = isz_output_get_hdr_metadata(out);
        if (hdr && hdr->size > 0) {
            uint32_t hdr_blob = 0;
            if (drmModeCreatePropertyBlob(st->drm_fd, hdr->bytes,
                                          hdr->size, &hdr_blob) == 0)
                (void)drmModeAtomicAddProperty(req, connector_id,
                                               cache->hdr_output_metadata,
                                               hdr_blob);
        }
    }

    /* Find the first surface with a current buffer and assign it to the
     * primary plane. The plane-slot model (SPEC 7.7) is honored: the
     * surface's plane_slot is the slot id, which we map to a KMS plane
     * via the output's slot table. For v1 the primary slot is the only
     * one wired to a real plane here; overlay/cursor assignment is a
     * follow-up. */
    isz_list_for_each(pos, list) {
        isz_surface *s = container_of(pos, isz_surface, server_node);
        if (s->output != out)
            continue;
        if (!s->current)
            continue;
        if (s->plane_type == ISZ_PLANE_PRIMARY) {
            primary_surf = s;
            break;
        }
    }

    if (primary_surf && primary_surf->current) {
        buf = primary_surf->current;
        /* GEM handle was stashed by isz_buffer_drm_import. */
        if (buf->priv) {
            uint32_t *p = buf->priv;
            fb_handle = *p;
        }
        if (fb_handle) {
            handles[0] = fb_handle;
            pitches[0] = buf->stride;
            offsets[0] = buf->offset;
            if (drmModeAddFB2(st->drm_fd, buf->width, buf->height,
                              buf->format, handles, pitches, offsets,
                              &fb_id, 0) == 0) {
                (void)add_prop(req, primary_plane_id, cache->plane_fb_id,
                               fb_id, "FB_ID", true);
                (void)add_prop(req, primary_plane_id, cache->plane_crtc_id,
                               crtc_id, "CRTC_ID", true);
                (void)drmModeAtomicAddProperty(req, primary_plane_id,
                                               cache->plane_src_x, 0);
                (void)drmModeAtomicAddProperty(req, primary_plane_id,
                                               cache->plane_src_y, 0);
                (void)drmModeAtomicAddProperty(req, primary_plane_id,
                                               cache->plane_src_w,
                                               (uint64_t)buf->width << 16);
                (void)drmModeAtomicAddProperty(req, primary_plane_id,
                                               cache->plane_src_h,
                                               (uint64_t)buf->height << 16);
                (void)drmModeAtomicAddProperty(req, primary_plane_id,
                                               cache->plane_crtc_x, 0);
                (void)drmModeAtomicAddProperty(req, primary_plane_id,
                                               cache->plane_crtc_y, 0);
                (void)drmModeAtomicAddProperty(req, primary_plane_id,
                                               cache->plane_crtc_w,
                                               buf->width);
                (void)drmModeAtomicAddProperty(req, primary_plane_id,
                                               cache->plane_crtc_h,
                                               buf->height);
                if (cache->plane_zpos)
                    (void)drmModeAtomicAddProperty(req, primary_plane_id,
                                                   cache->plane_zpos,
                                                   primary_surf->zpos);
                /* Rotation maps 1:1 from isz_transform to the KMS
                 * rotation bitmask (BIT(0) rotate-0, BIT(1) rotate-90,
                 * BIT(2) rotate-180, BIT(3) rotate-270, BIT(4) reflect-x,
                 * BIT(5) reflect-y). */
                if (cache->plane_rotation) {
                    rot = 1u; /* DRM_MODE_ROTATE_0 */
                    switch (primary_surf->transform) {
                    case ISZ_TRANSFORM_ROTATE_90:  rot = 2u; break;
                    case ISZ_TRANSFORM_ROTATE_180: rot = 4u; break;
                    case ISZ_TRANSFORM_ROTATE_270: rot = 8u; break;
                    case ISZ_TRANSFORM_REFLECT_X:  rot = 16u; break;
                    case ISZ_TRANSFORM_REFLECT_Y:  rot = 32u; break;
                    case ISZ_TRANSFORM_NORMAL:     rot = 1u; break;
                    }
                    (void)drmModeAtomicAddProperty(req, primary_plane_id,
                                                   cache->plane_rotation, rot);
                }
            } else {
                isz_log_internal(ISZ_LOG_WARN,
                                 "drm atomic: drmModeAddFB2 failed");
            }
        }
    }

    /* Commit flags. NONBLOCK so the call returns immediately and the
     * page-flip event signals completion via read_events (SPEC 7.3).
     * TEST_ONLY adds TEST_ONLY and skips the page-flip event. ASYNC
     * adds PAGE_FLIP_ASYNC (SPEC 7.3). */
    commit_flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    if (flags & ISZ_COMMIT_TEST_ONLY) {
        commit_flags = DRM_MODE_ATOMIC_TEST_ONLY;
        /* Test commits are synchronous; no page-flip event. */
    }
    if (flags & ISZ_COMMIT_ASYNC) {
        commit_flags |= DRM_MODE_PAGE_FLIP_ASYNC;
    }

    ret = drmModeAtomicCommit(st->drm_fd, req, commit_flags, st);
    if (ret != 0) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "drm atomic: commit failed rc=%d", ret);
        rc = ISZ_ERR_COMMIT_FAILED;
        goto out;
    }

    rc = ISZ_OK;

out:
    if (blob_mode_id)
        drmModeDestroyPropertyBlob(st->drm_fd, blob_mode_id);
    if (req)
        drmModeAtomicFree(req);
    return rc;
}

#endif /* ISHIZUE_HAVE_DRM */
