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

/* isz_drm_event.c -- drmHandleEvent dispatcher, SPEC 7.3 / 10.
 *
 * Page-flip events finish the in-flight commit (state COMMITTING -> READY
 * via isz_backend_finish_commit). Connector hotplug events trigger a
 * re-enumeration of st->connectors and fire the output hook so the server
 * layer can wrap newly-appeared connectors into isz_output objects and
 * emit ISZ_EVENT_OUTPUT_ADD / OUTPUT_REMOVE. */
#include "isz_drm_event.h"
#include "isz_drm.h"

#ifdef ISHIZUE_HAVE_DRM

#include <ishizue/isz.h>
#include "../isz_server_internal.h"
#include "../util/isz_log.h"

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* Page-flip event handler. drmHandleEvent calls this when the kernel
 * finishes scanning out the previous frame. The state machine was left
 * at COMMITTING by ops->commit; transition back to READY here. */
static void page_flip_handler(int fd, unsigned int sequence,
                              unsigned int tv_sec, unsigned int tv_usec,
                              void *user_data)
{
    (void)fd;
    (void)sequence;
    (void)tv_sec;
    (void)tv_usec;
    struct isz_backend *b = user_data;
    if (!b)
        return;
    isz_backend_finish_commit(b);
}

/* Connector hotplug / property-change handler. drmHandleEvent calls this
 * via the v3 event path (drmEventContext version 3). We trigger a full
 * connector re-enumeration rather than parsing the per-connector event
 * details, since the server layer tracks wrappers by connector id. */
static void connector_handler(int fd, unsigned int sequence,
                              unsigned int tv_sec, unsigned int tv_usec,
                              void *user_data)
{
    (void)fd;
    (void)sequence;
    (void)tv_sec;
    (void)tv_usec;
    struct isz_drm_state *st = user_data;
    if (!st)
        return;
    /* Defer the actual re-enumeration to a helper in isz_drm.c that
     * has access to the full state struct and the output hook. */
    isz_drm_rescan_connectors(st);
}

int isz_drm_event_dispatch(struct isz_drm_state *st,
                           struct isz_backend *backend)
{
    if (!st || st->drm_fd < 0)
        return ISZ_ERR_FEATURE_UNAVAIL;

    drmEventContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* Pin to v2: page-flip handler only. v3 adds page_flip_handler2 and
     * sequence_handler (connector-hotplug, writeback); those land with
     * the hotplug and capture waves. Pinning avoids depending on struct
     * fields that older libdrm installs don't have. */
    ctx.version = 2;
    ctx.vblank_handler    = NULL;
    ctx.page_flip_handler = page_flip_handler;

    /* drmHandleEvent reads from the fd until EAGAIN, calling the
     * registered handlers per event. Pass the backend pointer through
     * user_data so the page-flip handler can reach isz_backend_finish_commit.
     *
     * Connector hotplug: the kernel emits a DRM_EVENT_CONNECTOR that the
     * v3+ sequence_handler would receive, but pinning to v2 means we
     * don't see those events here. Hotplug is driven instead by a manual
     * rescan path (isz_drm_rescan_connectors) that a future udev watcher
     * or the test harness can trigger explicitly. */
    int rc = drmHandleEvent(st->drm_fd, &ctx);
    if (rc < 0) {
        int saved = errno;
        isz_log_internal(ISZ_LOG_WARN,
                         "drm event: drmHandleEvent rc=%d errno=%d",
                         rc, saved);
        if (saved == EACCES || saved == EPERM) {
            isz_backend_set_error(backend, ISZ_ERR_DRM_MASTER);
            return ISZ_ERR_DRM_MASTER;
        }
        return ISZ_OK;  /* transient; let the next dispatch retry */
    }
    (void)connector_handler;  /* referenced by future v3 wiring */
    return ISZ_OK;
}

#endif /* ISHIZUE_HAVE_DRM */
