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

/* isz_drm.c - DRM/KMS backend stub (SPEC §10).
 *
 * Real implementation is a later wave: libseat session, drmSetMaster,
 * atomic-KMS capability check, connector/CRTC enumeration, page-flip
 * event loop, multi-GPU render node scan. This stub wires the ops table
 * so the build resolves and reports the feature as unavailable. */
#include "isz_drm.h"
#include "isz_backend.h"
#include "isz_log_bridge.h"

#include <ishizue/isz.h>

static int isz_drm_init(struct isz_backend *self, void *config)
{
    (void)self;
    (void)config;
    isz_log_internal(ISZ_LOG_ERROR, "DRM backend not yet implemented");
    return ISZ_ERR_FEATURE_UNAVAIL;
}

static int isz_drm_commit(struct isz_backend *self, struct isz_output *out,
                          uint32_t flags)
{
    (void)self; (void)out; (void)flags;
    return ISZ_ERR_FEATURE_UNAVAIL;
}

static int isz_drm_read_events(struct isz_backend *self)
{
    (void)self;
    return ISZ_ERR_FEATURE_UNAVAIL;
}

static void isz_drm_destroy(struct isz_backend *self)
{
    (void)self;
    /* init never allocates impl, so there's nothing to free. */
}

static void isz_drm_dump(const struct isz_backend *self, FILE *fp)
{
    (void)self;
    fprintf(fp, "  drm: stub (not yet implemented)\n");
}

static const struct isz_backend_ops isz_drm_ops = {
    .init        = isz_drm_init,
    .commit      = isz_drm_commit,
    .read_events = isz_drm_read_events,
    .destroy     = isz_drm_destroy,
    .dump        = isz_drm_dump,
};

const struct isz_backend_ops *isz_drm_get_ops(void)
{
    return &isz_drm_ops;
}
