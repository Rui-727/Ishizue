/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_capture.c -- screen capture via writeback connector. SPEC 7.11,
 * 6.11 (portal-style consent).
 *
 * Consent is owned by W2-D (isz_capture_consent.c): the Architect calls
 * isz_capture_grant(srv, out) when the user approves; this layer calls
 * isz_capture_check_consent(srv, out) before programming the writeback
 * connector. Without consent, capture_start returns
 * ISZ_ERR_ACCESS_DENIED and closes the dmabuf fd.
 *
 * The writeback programming path is behind ISHIZUE_HAVE_DRM. For the
 * headless backend, capture_start records the dmabuf fd and desc;
 * capture_stop hands the fd back via isz_render_send_capture_done (the
 * client dispatch wave provides the wire send path). */

#define _POSIX_C_SOURCE 200809L

#include "isz_surface_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../util/isz_log.h"

#ifdef ISHIZUE_HAVE_DRM
extern int isz_capture_drm_start(isz_output *out, int dmabuf_fd,
                                 isz_buffer_desc *desc);
extern int isz_capture_drm_stop(isz_output *out);
#endif

/* ------------------------------------------------------------------ */
/* Capture state                                                      */
/* ------------------------------------------------------------------ */

#define ISZ_CAPTURE_MAX 8

static struct {
    isz_output      *out;
    int              dmabuf_fd;
    isz_buffer_desc  desc;
    bool             active;
} s_capture[ISZ_CAPTURE_MAX];
static size_t s_capture_count;

static size_t capture_find(isz_output *out)
{
    for (size_t i = 0; i < s_capture_count; i++)
        if (s_capture[i].out == out && s_capture[i].active)
            return i;
    return (size_t)-1;
}

/* ------------------------------------------------------------------ */
/* Public API (isz.h)                                                 */
/* ------------------------------------------------------------------ */

ISZ_API int isz_output_capture_start(isz_output *out, int dmabuf_fd,
                                     isz_buffer_desc *desc)
{
    if (!out) {
        if (dmabuf_fd >= 0) close(dmabuf_fd);
        return ISZ_ERR_INVALID_ARG;
    }
    if (dmabuf_fd < 0 || !desc) return ISZ_ERR_INVALID_ARG;

    isz_server *srv = out->srv;
    if (!srv) {
        close(dmabuf_fd);
        return ISZ_ERR_INVALID_ARG;
    }

    if (!isz_capture_check_consent(srv, out)) {
        isz_log_internal(ISZ_LOG_INFO,
                         "capture_start: consent not granted");
        close(dmabuf_fd);
        return ISZ_ERR_ACCESS_DENIED;
    }

    if (capture_find(out) != (size_t)-1) {
        close(dmabuf_fd);
        return ISZ_ERR_INVALID_ARG;  /* already capturing */
    }

    if (s_capture_count >= ISZ_CAPTURE_MAX) {
        close(dmabuf_fd);
        return ISZ_ERR_RESOURCE_LIMIT;
    }

#ifdef ISHIZUE_HAVE_DRM
    int rc = isz_capture_drm_start(out, dmabuf_fd, desc);
    if (rc < 0) {
        close(dmabuf_fd);
        return rc;
    }
#endif

    s_capture[s_capture_count].out = out;
    s_capture[s_capture_count].dmabuf_fd = dmabuf_fd;
    s_capture[s_capture_count].desc = *desc;
    s_capture[s_capture_count].active = true;
    s_capture_count++;
    return ISZ_OK;
}

ISZ_API int isz_output_capture_stop(isz_output *out)
{
    if (!out) return ISZ_ERR_INVALID_ARG;

    size_t idx = capture_find(out);
    if (idx == (size_t)-1)
        return ISZ_ERR_INVALID_ARG;

#ifdef ISHIZUE_HAVE_DRM
    int rc = isz_capture_drm_stop(out);
    if (rc < 0) return rc;
#endif

    int fd = s_capture[idx].dmabuf_fd;
    isz_buffer_desc desc = s_capture[idx].desc;
    s_capture[idx].active = false;

    /* Compact the slot. */
    s_capture_count--;
    if (idx != s_capture_count) {
        s_capture[idx] = s_capture[s_capture_count];
    }
    s_capture[s_capture_count].active = false;

    isz_render_send_capture_done(out->srv, out, fd, &desc);
    return ISZ_OK;
}
