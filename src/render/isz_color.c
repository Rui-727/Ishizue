/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_color.c -- KMS color management blob helpers. SPEC 7.2.
 *
 * Used by the output layer (W2-A) to turn LUT arrays and CTM matrices
 * into KMS property blobs. The output layer owns the LUT-size validation
 * against DEGAMMA_LUT_SIZE / GAMMA_LUT_SIZE; these helpers only handle
 * the blob creation and lifetime.
 *
 * Headless: all functions are no-ops returning 0 (no KMS device). DRM:
 * behind ISHIZUE_HAVE_DRM, calls drmModeCreatePropertyBlob /
 * drmModeDestroyBlob on the output's DRM fd. */

#include "isz_surface_internal.h"

#include <stdlib.h>
#include <string.h>

#include "../util/isz_log.h"

#ifdef ISHIZUE_HAVE_DRM
#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* KMS struct drm_color_lut is { uint16_t r, g, b, reserved }. */
struct isz_color_lut_entry {
    uint16_t r, g, b, reserved;
};
#endif

uint32_t isz_color_gamma_blob(isz_output *out, const uint16_t *r,
                              const uint16_t *g, const uint16_t *b,
                              size_t size)
{
    if (!out || !r || !g || !b || size == 0)
        return 0;

#ifdef ISHIZUE_HAVE_DRM
    struct isz_color_lut_entry *lut = calloc(size, sizeof(*lut));
    if (!lut)
        return 0;

    for (size_t i = 0; i < size; i++) {
        lut[i].r = r[i];
        lut[i].g = g[i];
        lut[i].b = b[i];
        lut[i].reserved = 0;
    }

    uint32_t blob_id = 0;
    int drm_fd = isz_output_get_drm_fd(out);
    if (drm_fd >= 0) {
        int rc = drmModeCreatePropertyBlob(drm_fd, lut,
                    size * sizeof(*lut), &blob_id);
        if (rc < 0) {
            isz_log_internal(ISZ_LOG_WARN,
                "color: gamma blob creation failed (rc=%d)", rc);
            blob_id = 0;
        }
    }
    free(lut);
    return blob_id;
#else
    return 0;
#endif
}

uint32_t isz_color_ctm_blob(isz_output *out, const float matrix[9])
{
    if (!out || !matrix)
        return 0;

#ifdef ISHIZUE_HAVE_DRM
    /* KMS CTM is a 3x3 matrix of s.0.32 fixed-point. Each entry is a
     * 64-bit value where the low 32 bits are fractional. Clamp to
     * [0, 1.0] for now; the full [-2, ~2) range needs sign handling. */
    uint64_t ctm[9];
    for (int i = 0; i < 9; i++) {
        double v = (double)matrix[i];
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        ctm[i] = (uint64_t)(v * (double)(1ull << 32));
    }

    uint32_t blob_id = 0;
    int drm_fd = isz_output_get_drm_fd(out);
    if (drm_fd >= 0) {
        int rc = drmModeCreatePropertyBlob(drm_fd, ctm, sizeof(ctm), &blob_id);
        if (rc < 0) {
            isz_log_internal(ISZ_LOG_WARN,
                "color: ctm blob creation failed (rc=%d)", rc);
            blob_id = 0;
        }
    }
    return blob_id;
#else
    return 0;
#endif
}

void isz_color_destroy_blob(isz_output *out, uint32_t blob_id)
{
    if (!out || blob_id == 0)
        return;

#ifdef ISHIZUE_HAVE_DRM
    int drm_fd = isz_output_get_drm_fd(out);
    if (drm_fd < 0)
        return;
    drmModeDestroyPropertyBlob(drm_fd, blob_id);
#else
    (void)out;
#endif
}
