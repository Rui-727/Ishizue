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

/* isz_drm.h -- DRM/KMS backend (SPEC 3, 7.2, 10).
 *
 * Real implementation lives in isz_drm.c behind ISHIZUE_HAVE_DRM.
 * Without ISHIZUE_HAVE_DRM the backend's init returns
 * ISZ_ERR_FEATURE_UNAVAIL so the build still links and tests run on
 * the headless backend.
 *
 * The internal API surface below is built only when ISHIZUE_HAVE_DRM
 * is defined; it lets the server lifecycle layer wire the DRM fd into
 * its epoll set, route session-active / session-inactive events, and
 * expose the VT fd to the crash recovery handler. */
#ifndef ISZ_BACKEND_DRM_H
#define ISZ_BACKEND_DRM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ISHIZUE_HAVE_DRM
#include <drm_mode.h>
#include <xf86drmMode.h>
#endif

#include "isz_backend.h"
#include "../util/isz_list.h"

const struct isz_backend_ops *isz_drm_get_ops(void);

#ifdef ISHIZUE_HAVE_DRM

struct isz_server;
struct isz_drm_state;

/* Forward decl: defined in isz_drm_atomic.h. */
struct isz_drm_prop_cache;

#define ISZ_DRM_MAX_CONNECTORS   32
#define ISZ_DRM_MAX_CRTCS         8
#define ISZ_DRM_MAX_PLANES       32
#define ISZ_DRM_MAX_RENDER_NODES  8
#define ISZ_DRM_GPU_NODE_PATH_MAX 64

struct isz_drm_connector {
    uint32_t connector_id;
    uint32_t crtc_id;            /* 0 if not bound */
    uint32_t crtc_mask;          /* bit index for plane possible_crtcs matching */
    uint32_t encoder_id;
    bool     connected;
    bool     enabled;
    char     name[32];
    uint8_t *edid;               /* backend-owned; freed on destroy/rescan */
    size_t   edid_size;
    uint32_t width;              /* first mode's width, 0 if disconnected */
    uint32_t height;
    uint32_t refresh_mhz;
    bool     hdr_capable;
    bool     vrr_capable;
    /* Full KMS mode for the first connector mode. The atomic commit
     * path needs the real clock + hsync/vsync/htotal/vtotal timing.
     * Fabricating these from width/height/refresh makes the kernel
     * reject the modeset with EINVAL. */
    drmModeModeInfo mode;
};

struct isz_drm_crtc {
    uint32_t crtc_id;
    uint32_t bitmask;            /* bit in possible_crtcs masks */
};

struct isz_drm_plane {
    uint32_t plane_id;
    uint32_t possible_crtcs;
    int      type;               /* 0=overlay, 1=primary, 2=cursor */
};

/* Forward decl of the output info; full definition is below. */
struct isz_drm_output_info;

typedef void (*isz_drm_output_hook_fn)(void *userdata,
                                       const struct isz_drm_output_info *info,
                                       bool added);

struct isz_drm_state {
    int  drm_fd;
    bool is_master;
    bool session_active;
    bool atomic_ok;

    /* W5-B: probe result of DRM_CAP_SYNCOBJ at init. False on kernels
     * or drivers that don't support drm_syncobj; the atomic commit
     * then skips IN_FENCE_FD / OUT_FENCE_PTR and relies on the DMA-BUF's
     * implicit fencing per SPEC 11. */
    bool syncobj_supported;

    /* W5-B: back-pointer to the backend object that owns this state.
     * Set in isz_drm_init so the page-flip event handler can reach
     * isz_backend_finish_commit without re-deriving the pointer. */
    struct isz_backend *backend;

    /* W5-B: buffers scanned out by the most recent commit, awaiting
     * the page-flip event. The page-flip handler walks this list and
     * either sends ISZ_MSG_RELEASE (implicit sync) or moves the buffer
     * to the server's pending_releases list (explicit sync, polled by
     * isz_buffer_poll_out_fences). The list owns one ref per buffer. */
    isz_list in_flight_releases;

    /* libseat session. NULL when ISHIZUE_HAVE_LIBSEAT is undefined or
     * libseat_open_seat failed (direct open fallback). */
#ifdef ISHIZUE_HAVE_LIBSEAT
    struct libseat *seat;
    int              vt_fd;
#else
    int              vt_fd;      /* -1 without libseat */
#endif

    /* Server back-channel. NULL until isz_drm_set_server. */
    struct isz_server *srv;

    /* KMS snapshots. */
    struct isz_drm_connector connectors[ISZ_DRM_MAX_CONNECTORS];
    size_t                   connector_count;
    struct isz_drm_crtc      crtcs[ISZ_DRM_MAX_CRTCS];
    size_t                   crtc_count;
    struct isz_drm_plane     planes[ISZ_DRM_MAX_PLANES];
    size_t                   plane_count;

    /* Render nodes (SPEC 10). Filled at init by scanning
     * /dev/dri/renderD*. */
    char   render_nodes[ISZ_DRM_MAX_RENDER_NODES][ISZ_DRM_GPU_NODE_PATH_MAX];
    size_t render_node_count;

    /* Property id cache (populated lazily by the atomic builder). */
    struct isz_drm_prop_cache *prop_cache;

    /* Output hook (mirrors headless backend pattern). */
    isz_drm_output_hook_fn  hook_fn;
    void                   *hook_userdata;
};

/* Connector snapshot handed to the server-layer output hook, mirroring
 * isz_headless_output_info. id is the DRM connector id; width/height/
 * refresh_mhz come from the connector's first mode (or zero for a
 * disconnected connector). edid points into backend-owned storage; the
 * server layer copies the bytes into isz_output.edid. */
struct isz_drm_output_info {
    uint32_t connector_id;
    uint32_t crtc_id;          /* preferred CRTC id, 0 if unbound */
    uint32_t width;
    uint32_t height;
    uint32_t refresh_mhz;
    bool     connected;
    bool     hdr_capable;
    bool     vrr_capable;
    char     name[32];
    const uint8_t *edid;       /* may be NULL */
    size_t         edid_size;
};

/* Register the output add/remove hook. Called by isz_init after the
 * backend is created. The hook fires synchronously for each initial
 * connector in init()'s enumeration and again on hotplug rescan. */
void isz_drm_set_output_hook(struct isz_backend *b,
                             isz_drm_output_hook_fn fn, void *userdata);

/* Hand the backend a back-pointer to the server. Used to add the DRM
 * fd to srv->epoll_fd with the backend_tag and to emit events. */
int  isz_drm_set_server(struct isz_backend *b, struct isz_server *srv);

/* Collect pollable fds. The DRM fd is the only one in v1 (vblank events
 * arrive on it). Returns the count written into fds[]. */
int  isz_drm_get_fds(struct isz_backend *b, int *fds, size_t max);

/* SPEC 3: on SESSION_INACTIVE the library calls drmDropMaster. On
 * SESSION_ACTIVE it re-acquires master; if that fails it surfaces
 * ISZ_ERR_DRM_MASTER via isz_backend_set_error and all subsequent
 * commits are blocked until the next ACTIVE transition. */
void isz_drm_on_session_inactive(struct isz_backend *b);
void isz_drm_on_session_active(struct isz_backend *b);

/* Used by the crash recovery handler (SPEC 12). Returns the libseat VT
 * fd, or -1 when not running under libseat. */
int  isz_drm_get_vt_fd(struct isz_backend *b);

/* Return the cached connector snapshots. The pointer is valid until
 * the next libseat dispatch or backend destroy. Count is written to
 * *n. Used by isz_init to wrap initial connectors after wiring the
 * output hook (mirrors isz_headless_outputs). */
const struct isz_drm_connector *isz_drm_get_connectors(struct isz_backend *b,
                                                       size_t *n);

/* Check for pending VT switch signals (SIGUSR1/SIGUSR2) and
 * drop/acquire DRM master accordingly. Must be called on every
 * dispatch iteration, not just when the DRM fd has events.
 * The signal handler sets a flag; this function checks it.
 * No-op when libseat is handling VT switching (vt_fd < 0). */
void isz_drm_vt_dispatch(struct isz_backend *b);

/* Hotplug rescan. Re-reads the connector list, fires the output hook
 * for any newly-appeared or disappeared connectors. Exposed here so
 * isz_drm_event.c can drive it from the page-flip / hotplug path. */
void isz_drm_rescan_connectors(struct isz_drm_state *st);

/* isz_event_listener_fn shims. The server's isz_init registers these on
 * ISZ_EVENT_SESSION_ACTIVE / _INACTIVE when the DRM backend is selected;
 * each forwards to isz_drm_on_session_active / _inactive. userdata is
 * the struct isz_backend pointer. */
void isz_drm_session_active_listener(void *userdata, const struct isz_event *ev);
void isz_drm_session_inactive_listener(void *userdata, const struct isz_event *ev);

#endif /* ISHIZUE_HAVE_DRM */

#endif /* ISZ_BACKEND_DRM_H */
