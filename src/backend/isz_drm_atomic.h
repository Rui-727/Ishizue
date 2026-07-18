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

/* isz_drm_atomic.h -- atomic KMS commit builder, SPEC 7.2 / 7.3 / 7.5.
 *
 * Builds a drmModeAtomicReq from the per-output surface state and submits
 * it with drmModeAtomicCommit(NONBLOCK | PAGE_FLIP_EVENT). The page-flip
 * event lands in isz_drm_event.c, which transitions the backend out of
 * COMMITTING via isz_backend_finish_commit.
 *
 * Only compiled in when ISHIZUE_HAVE_DRM is defined. The header is
 * included unconditionally from isz_drm.c; the function declarations
 * resolve to no-ops or are never referenced in the no-libdrm build. */
#ifndef ISZ_BACKEND_DRM_ATOMIC_H
#define ISZ_BACKEND_DRM_ATOMIC_H

#include "isz_backend.h"

#ifdef ISHIZUE_HAVE_DRM

struct isz_drm_state;
struct isz_output;
struct isz_surface;

/* KMS property id cache. Populated lazily by isz_drm_atomic_commit on
 * the first commit per (crtc, connector, primary-plane) tuple. */
struct isz_drm_prop_cache {
    uint32_t crtc_active;
    uint32_t crtc_mode_id;
    uint32_t crtc_vrr_enabled;
    uint32_t crtc_out_fence_ptr;   /* W5-B: CRTC OUT_FENCE_PTR (sync_file fd out) */
    uint32_t connector_crtc_id;
    uint32_t connector_dpms;
    uint32_t plane_fb_id;
    uint32_t plane_crtc_id;
    uint32_t plane_src_x;
    uint32_t plane_src_y;
    uint32_t plane_src_w;
    uint32_t plane_src_h;
    uint32_t plane_crtc_x;
    uint32_t plane_crtc_y;
    uint32_t plane_crtc_w;
    uint32_t plane_crtc_h;
    uint32_t plane_zpos;
    uint32_t plane_rotation;
    uint32_t plane_in_fence_fd;   /* W5-B: plane IN_FENCE_FD (sync_file fd in) */
    uint32_t hdr_output_metadata;
    uint32_t degamma_lut;
    uint32_t ctm;
    uint32_t gamma_lut;
    bool     inited;
};

/* Build and submit the atomic commit for `out`. Surfaces bound to `out`
 * are pulled from the render-layer surface list; their plane-slot
 * assignments map to KMS plane + FB_ID + SRC/CRTC rectangles.
 *
 * flags: ISZ_COMMIT_NORMAL / _ASYNC / _TEST_ONLY. _ASYNC adds
 * DRM_MODE_PAGE_FLIP_ASYNC. _TEST_ONLY adds DRM_MODE_ATOMIC_TEST_ONLY
 * and skips the PAGE_FLIP_EVENT (no completion to wait for; the backend
 * is finish_commit'd inline).
 *
 * Returns 0 on success (state stays COMMITTING until the page-flip event
 * arrives, or already READY for TEST_ONLY), negative isz_error on
 * failure. */
int isz_drm_atomic_commit(struct isz_drm_state *st,
                          struct isz_output *out,
                          uint32_t flags);

/* KMS property name -> id cache, populated lazily. drmModeObjectGetProperties
 * is the bottleneck at commit time, so each connector/CRTC/plane has its
 * property set cached on first use. */
uint32_t isz_drm_prop_id(int drm_fd, uint32_t object_id,
                         uint32_t object_type, const char *name);

#endif /* ISHIZUE_HAVE_DRM */

#endif /* ISZ_BACKEND_DRM_ATOMIC_H */
