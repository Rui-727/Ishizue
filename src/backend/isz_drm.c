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

/* isz_drm.c -- DRM/KMS backend (SPEC 3, 7.2, 10).
 *
 * Without ISHIZUE_HAVE_DRM the backend's init returns
 * ISZ_ERR_FEATURE_UNAVAIL so the build links and tests run on the
 * headless backend. With ISHIZUE_HAVE_DRM the backend:
 *
 *   - opens the primary DRM node via libseat (or ISZ_DRM_NODE env, or
 *     /dev/dri/card0 as a last-resort direct open)
 *   - calls drmSetMaster and fails fast with ISZ_ERR_DRM_MASTER on
 *     EBUSY / EPERM (SPEC 3)
 *   - verifies DRM_CLIENT_CAP_ATOMIC and fails fast with a clear
 *     error if atomic KMS is unavailable (SPEC 3: "atomic KMS only,
 *     hard requirement, not a fallback path")
 *   - enumerates connectors + CRTCs + planes via drmModeGetResources /
 *     drmModeGetPlaneResources and surfaces ISZ_EVENT_OUTPUT_ADD for
 *     each connected connector (initial + hotplug via
 *     isz_drm_rescan_connectors)
 *   - commits via isz_drm_atomic_commit (drmModeAtomicCommit with
 *     NONBLOCK | PAGE_FLIP_EVENT, SPEC 7.3)
 *   - drains the DRM fd via isz_drm_event_dispatch (drmHandleEvent),
 *     transitioning COMMITTING -> READY when the page-flip event arrives
 *   - blank_all_crtcs disables every CRTC via drmModeSetCrtc with NULL
 *     fb (used by the SPEC 12 crash handler)
 *   - on SESSION_INACTIVE calls drmDropMaster; on SESSION_ACTIVE it
 *     re-acquires master, surfacing ISZ_ERR_DRM_MASTER and blocking
 *     commits if re-acquisition fails (SPEC 3)
 *   - enumerates /dev/dri/renderD* at init for multi-GPU (SPEC 10)
 *
 * The cache of GEM handles for imported DMA-BUFs lives here too (the
 * isz_buffer_drm_import / isz_buffer_drm_release externs). isz_color.c's
 * isz_output_get_drm_fd is also defined here so the color-mgmt blob
 * helpers can reach the DRM fd. */

/* _GNU_SOURCE is required for scandir/alphasort (used in the DRM path
 * to enumerate render nodes) and O_CLOEXEC. Defining it at the top of
 * the TU before any system header is included means both the no-DRM
 * stub path and the real path see the same feature-test macro. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "isz_drm.h"
#include "isz_drm_atomic.h"
#include "isz_drm_event.h"
#include "isz_drm_edid.h"
#include "isz_backend.h"
#include "isz_log_bridge.h"

#include <ishizue/isz.h>

#include <stdlib.h>
#include <string.h>

/* The sandbox has no libdrm. The fallback path keeps the current
 * stub behavior: init returns ISZ_ERR_FEATURE_UNAVAIL, the other
 * ops are no-ops, and the headless backend is the only functional
 * one. The real implementation only compiles in when libdrm is
 * detected by the Makefile's auto-detect step. */
#ifndef ISHIZUE_HAVE_DRM

static int isz_drm_init(struct isz_backend *self, void *config)
{
    (void)self;
    (void)config;
    isz_log_internal(ISZ_LOG_ERROR,
                     "drm: libdrm not available at build time");
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
}

static void isz_drm_dump(const struct isz_backend *self, FILE *fp)
{
    (void)self;
    fprintf(fp, "  drm: not built (ISHIZUE_HAVE_DRM undefined)\n");
}

static void isz_drm_blank_all_crtcs(struct isz_backend *self)
{
    (void)self;
}

static const struct isz_backend_ops isz_drm_ops = {
    .init            = isz_drm_init,
    .commit          = isz_drm_commit,
    .read_events     = isz_drm_read_events,
    .destroy         = isz_drm_destroy,
    .dump            = isz_drm_dump,
    .blank_all_crtcs = isz_drm_blank_all_crtcs,
};

const struct isz_backend_ops *isz_drm_get_ops(void)
{
    return &isz_drm_ops;
}

/* isz_output_get_drm_fd is declared unconditionally in
 * isz_surface_internal.h so isz_color.c can call it. Without libdrm
 * it always returns -1 (no DRM device). */
int isz_output_get_drm_fd(isz_output *out)
{
    (void)out;
    return -1;
}

#else  /* ISHIZUE_HAVE_DRM */

#include "../isz_server_internal.h"
#include "../render/isz_surface_internal.h"
#include "../buffer/isz_buffer.h"
#include "../buffer/isz_syncobj.h"
#include "../util/isz_log.h"

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifdef ISHIZUE_HAVE_LIBSEAT
#include <libseat.h>
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>

/* ------------------------------------------------------------------ */
/* Backend-private state                                              */
/* ------------------------------------------------------------------ */

/* Global so isz_buffer_drm_import can reach the DRM fd without
 * threading it through the buffer API. The DRM backend is a
 * singleton per server; single-threaded per SPEC 5, so no lock. */
static struct isz_drm_state *g_drm_state;

/* ------------------------------------------------------------------ */
/* Render-node enumeration (SPEC 10)                                  */
/* ------------------------------------------------------------------ */

static int is_dri_render_node(const struct dirent *e)
{
    if (strncmp(e->d_name, "renderD", 7) != 0)
        return 0;
    if (e->d_name[7] < '0' || e->d_name[7] > '9')
        return 0;
    return 1;
}

static void enumerate_render_nodes(struct isz_drm_state *st)
{
    st->render_node_count = 0;
    struct dirent **list = NULL;
    int n = scandir("/dev/dri", &list, is_dri_render_node, alphasort);
    if (n < 0)
        return;
    for (int i = 0; i < n; i++) {
        if (st->render_node_count >= ISZ_DRM_MAX_RENDER_NODES) {
            free(list[i]);
            continue;
        }
        char *dst = st->render_nodes[st->render_node_count];
        int rc = snprintf(dst, ISZ_DRM_GPU_NODE_PATH_MAX,
                          "/dev/dri/%s", list[i]->d_name);
        if (rc > 0 && rc < ISZ_DRM_GPU_NODE_PATH_MAX)
            st->render_node_count++;
        free(list[i]);
    }
    free(list);
    isz_log_internal(ISZ_LOG_INFO,
                     "drm: enumerated %zu render node(s)",
                     st->render_node_count);
}

/* ------------------------------------------------------------------ */
/* Connector / CRTC / plane snapshot                                  */
/* ------------------------------------------------------------------ */

static void free_connector(struct isz_drm_connector *c)
{
    free(c->edid);
    c->edid      = NULL;
    c->edid_size = 0;
}

static void snapshot_connector(int drm_fd, drmModeConnector *conn,
                               struct isz_drm_connector *out)
{
    memset(out, 0, sizeof(*out));
    out->connector_id = conn->connector_id;
    out->encoder_id   = conn->encoder_id;
    out->connected    = (conn->connection == DRM_MODE_CONNECTED);
    out->enabled      = false;

    const char *type_name = drmModeGetConnectorTypeName(conn->connector_type);
    snprintf(out->name, sizeof(out->name), "%s-%u",
             type_name ? type_name : "Unknown", conn->connector_type_id);

    if (out->connected && conn->count_modes > 0) {
        drmModeModeInfo *m = &conn->modes[0];
        out->width       = m->hdisplay;
        out->height      = m->vdisplay;
        out->refresh_mhz = m->vrefresh ? m->vrefresh * 1000u : 60000u;
    }

    /* EDID + HDR + VRR. */
    if (out->connected) {
        uint8_t *edid = NULL;
        size_t   edid_size = 0;
        if (isz_drm_edid_read(drm_fd, conn->connector_id,
                              &edid, &edid_size) == 0 &&
            edid && edid_size > 0) {
            if (isz_drm_edid_checksum_ok(edid, edid_size)) {
                out->edid      = edid;
                out->edid_size = edid_size;
                bool hdr_present = false;
                uint8_t hdr_buf[28];
                size_t  hdr_len = 0;
                isz_drm_edid_parse_hdr(edid, edid_size, &hdr_present,
                                       hdr_buf, sizeof(hdr_buf), &hdr_len);
                out->hdr_capable = hdr_present;
                (void)hdr_len;
            } else {
                isz_log_internal(ISZ_LOG_WARN,
                                 "drm: connector %u EDID checksum bad",
                                 conn->connector_id);
                free(edid);
            }
        }
        /* VRR: connector-level vrr_capable property. */
        out->vrr_capable =
            isz_drm_prop_id(drm_fd, conn->connector_id,
                            DRM_MODE_OBJECT_CONNECTOR, "vrr_capable") != 0;
    }
}

static void snapshot_crtcs(drmModeRes *res,
                           struct isz_drm_state *st)
{
    st->crtc_count = 0;
    if (!res)
        return;
    size_t n = res->count_crtcs;
    if (n > ISZ_DRM_MAX_CRTCS)
        n = ISZ_DRM_MAX_CRTCS;
    for (size_t i = 0; i < n; i++) {
        st->crtcs[i].crtc_id = res->crtcs[i];
        st->crtcs[i].bitmask = (uint32_t)(1u << i);
    }
    st->crtc_count = n;
}

static void snapshot_planes(int drm_fd, struct isz_drm_state *st)
{
    st->plane_count = 0;
    drmModePlaneRes *pres = drmModeGetPlaneResources(drm_fd);
    if (!pres)
        return;
    size_t n = pres->count_planes;
    if (n > ISZ_DRM_MAX_PLANES)
        n = ISZ_DRM_MAX_PLANES;
    for (size_t i = 0; i < n; i++) {
        drmModePlane *p = drmModeGetPlane(drm_fd, pres->planes[i]);
        if (!p)
            continue;
        st->planes[i].plane_id       = p->plane_id;
        st->planes[i].possible_crtcs = p->possible_crtcs;
        /* Plane type: look up the (immutable) "type" property. */
        drmModeObjectProperties *props =
            drmModeObjectGetProperties(drm_fd, p->plane_id,
                                       DRM_MODE_OBJECT_PLANE);
        int type = 0;
        if (props) {
            for (uint32_t j = 0; j < props->count_props; j++) {
                drmModePropertyRes *pr =
                    drmModeGetProperty(drm_fd, props->props[j]);
                if (!pr)
                    continue;
                if (strcmp(pr->name, "type") == 0) {
                    type = (int)props->prop_values[j];
                    drmModeFreeProperty(pr);
                    break;
                }
                drmModeFreeProperty(pr);
            }
            drmModeFreeObjectProperties(props);
        }
        st->planes[i].type = type;
        drmModeFreePlane(p);
    }
    st->plane_count = n;
    drmModeFreePlaneResources(pres);
}

/* Find an unused CRTC for a connector by walking its encoder's
 * possible_crtcs mask. Returns 0 if none available. */
static uint32_t pick_crtc_for_connector(struct isz_drm_state *st,
                                        drmModeConnector *conn)
{
    if (!conn->encoder_id)
        return 0;
    drmModeEncoder *enc = drmModeGetEncoder(st->drm_fd, conn->encoder_id);
    if (!enc)
        return 0;
    uint32_t chosen = 0;
    for (size_t i = 0; i < st->crtc_count; i++) {
        if (enc->possible_crtcs & (1u << i)) {
            chosen = st->crtcs[i].crtc_id;
            break;
        }
    }
    drmModeFreeEncoder(enc);
    return chosen;
}

/* ------------------------------------------------------------------ */
/* Output hook dispatch                                               */
/* ------------------------------------------------------------------ */

static void fire_output_hook(struct isz_drm_state *st,
                             const struct isz_drm_connector *c, bool added)
{
    if (!st->hook_fn)
        return;
    struct isz_drm_output_info info;
    memset(&info, 0, sizeof(info));
    info.connector_id = c->connector_id;
    info.crtc_id      = c->crtc_id;
    info.width        = c->width;
    info.height       = c->height;
    info.refresh_mhz  = c->refresh_mhz;
    info.connected    = c->connected;
    info.hdr_capable  = c->hdr_capable;
    info.vrr_capable  = c->vrr_capable;
    info.edid         = c->edid;
    info.edid_size    = c->edid_size;
    memcpy(info.name, c->name, sizeof(info.name));
    st->hook_fn(st->hook_userdata, &info, added);
}

/* ------------------------------------------------------------------ */
/* Hotplug rescan (SPEC 10)                                           */
/* ------------------------------------------------------------------ */

void isz_drm_rescan_connectors(struct isz_drm_state *st)
{
    if (!st || st->drm_fd < 0)
        return;

    drmModeRes *res = drmModeGetResources(st->drm_fd);
    if (!res) {
        isz_log_internal(ISZ_LOG_WARN,
                         "drm rescan: drmModeGetResources failed");
        return;
    }

    /* Build the new connector list, then diff against the cached one. */
    struct isz_drm_connector fresh[ISZ_DRM_MAX_CONNECTORS];
    size_t fresh_count = 0;
    size_t n = res->count_connectors;
    if (n > ISZ_DRM_MAX_CONNECTORS)
        n = ISZ_DRM_MAX_CONNECTORS;

    for (size_t i = 0; i < n; i++) {
        drmModeConnector *conn =
            drmModeGetConnector(st->drm_fd, res->connectors[i]);
        if (!conn)
            continue;
        snapshot_connector(st->drm_fd, conn, &fresh[fresh_count]);
        fresh[fresh_count].crtc_id = pick_crtc_for_connector(st, conn);
        /* Preserve crtc_id from the cached snapshot if the connector
         * was already known and the cached CRTC is still valid. */
        for (size_t j = 0; j < st->connector_count; j++) {
            if (st->connectors[j].connector_id == conn->connector_id) {
                fresh[fresh_count].crtc_id = st->connectors[j].crtc_id;
                break;
            }
        }
        fresh_count++;
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);

    /* Fire OUTPUT_ADD for connectors in fresh[] that weren't in the cache. */
    for (size_t i = 0; i < fresh_count; i++) {
        bool seen = false;
        for (size_t j = 0; j < st->connector_count; j++) {
            if (st->connectors[j].connector_id ==
                fresh[i].connector_id) {
                seen = true;
                break;
            }
        }
        if (!seen && fresh[i].connected)
            fire_output_hook(st, &fresh[i], true);
    }

    /* Fire OUTPUT_REMOVE for cached connectors that disappeared. */
    for (size_t j = 0; j < st->connector_count; j++) {
        bool still = false;
        for (size_t i = 0; i < fresh_count; i++) {
            if (fresh[i].connector_id == st->connectors[j].connector_id) {
                still = true;
                break;
            }
        }
        if (!still)
            fire_output_hook(st, &st->connectors[j], false);
    }

    /* Swap in the fresh snapshot. */
    for (size_t j = 0; j < st->connector_count; j++)
        free_connector(&st->connectors[j]);
    for (size_t i = 0; i < fresh_count; i++)
        st->connectors[i] = fresh[i];
    st->connector_count = fresh_count;
}

/* ------------------------------------------------------------------ */
/* DRM fd acquisition                                                 */
/* ------------------------------------------------------------------ */

#ifdef ISHIZUE_HAVE_LIBSEAT
static int open_drm_via_libseat(struct isz_drm_state *st,
                                const char *device_path)
{
    /* libseat_open_device takes a path and returns a fd managed by the
     * session. The seat handle was set up by the input/session layer;
     * for the DRM backend we open our own seat here to keep the
     * backend self-contained. */
    (void)device_path;
    if (st->seat)
        return libseat_open_device(st->seat, device_path, NULL);
    return -1;
}
#endif

static int open_primary_drm_node(struct isz_drm_state *st)
{
    const char *env = getenv("ISZ_DRM_NODE");
    char path[64];
#ifndef ISHIZUE_HAVE_LIBSEAT
    (void)st;  /* libseat path is the only consumer of st */
#endif

    if (env && *env) {
        /* Architect-specified node. */
#ifdef ISHIZUE_HAVE_LIBSEAT
        if (st->seat) {
            int fd = open_drm_via_libseat(st, env);
            if (fd >= 0)
                return fd;
        }
#endif
        return open(env, O_RDWR | O_CLOEXEC);
    }

    /* Scan /dev/dri/card* for the first primary node we can open. */
    for (int i = 0; i < 8; i++) {
        int rc = snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        if (rc <= 0 || rc >= (int)sizeof(path))
            break;
#ifdef ISHIZUE_HAVE_LIBSEAT
        if (st->seat) {
            int fd = open_drm_via_libseat(st, path);
            if (fd >= 0)
                return fd;
            continue;
        }
#endif
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0)
            return fd;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Backend ops                                                        */
/* ------------------------------------------------------------------ */

static int isz_drm_init(struct isz_backend *self, void *config)
{
    (void)config;
    struct isz_drm_state *st = calloc(1, sizeof(*st));
    if (!st)
        return ISZ_ERR_NO_MEMORY;
    self->impl = st;
    st->drm_fd = -1;
    st->vt_fd  = -1;
    st->session_active = true;  /* assume active until libseat says otherwise */
    st->backend = self;
    isz_list_init(&st->in_flight_releases);

#ifdef ISHIZUE_HAVE_LIBSEAT
    /* Open a libseat session so drmDropMaster / drmSetMaster on VT
     * switch work without us hand-rolling logind/seatd/direct. If
     * libseat_open_seat fails we fall through to a direct open. */
    st->seat = libseat_open_seat(NULL, st);
    if (st->seat) {
        /* libseat requires dispatch to surface enable_seat. We do one
         * non-blocking drain here; the input layer's session loop
         * takes over from there. */
        (void)libseat_dispatch(st->seat, 0);
    }
#endif

    int fd = open_primary_drm_node(st);
    if (fd < 0) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "drm init: no primary DRM node openable");
        free(st->prop_cache);
        free(st);
        self->impl = NULL;
        return ISZ_ERR_FEATURE_UNAVAIL;
    }
    st->drm_fd = fd;
    g_drm_state = st;

    /* SPEC 3: acquire master, fail fast on EBUSY/EPERM. */
    if (drmSetMaster(fd) != 0) {
        int saved = errno;
        isz_log_internal(ISZ_LOG_ERROR,
                         "drm init: drmSetMaster failed errno=%d (%s)",
                         saved, strerror(saved));
        close(fd);
        st->drm_fd = -1;
        g_drm_state = NULL;
        free(st->prop_cache);
        free(st);
        self->impl = NULL;
        return ISZ_ERR_DRM_MASTER;
    }
    st->is_master = true;

    /* SPEC 3: atomic KMS is a hard requirement. */
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "drm init: DRM_CLIENT_CAP_ATOMIC unavailable");
        drmDropMaster(fd);
        close(fd);
        st->drm_fd = -1;
        g_drm_state = NULL;
        free(st->prop_cache);
        free(st);
        self->impl = NULL;
        return ISZ_ERR_FEATURE_UNAVAIL;
    }
    st->atomic_ok = true;

    /* Also request universal planes so the plane list includes primary
     * and cursor planes, not just overlays. */
    (void)drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    /* W5-B: probe DRM_CAP_SYNCOBJ for explicit-sync support. Failure
     * is silent (debug log only) and falls back to the DMA-BUF's
     * implicit kernel fencing per SPEC 11 fallback matrix. The
     * syncobj_supported flag is consulted by the atomic commit path
     * to decide whether to attach IN_FENCE_FD / OUT_FENCE_PTR. */
    st->syncobj_supported = (isz_syncobj_init(fd) == ISZ_OK);

    /* W5-B: register the fd with the buffer layer so its syncobj
     * helpers can reach it without each caller threading the fd
     * through. The buffer layer caches this for the backend's
     * lifetime; we don't need to clear it on destroy since the
     * buffer layer treats a stale fd as "no DRM" gracefully. */
    isz_buffer_set_drm_fd(fd);

    /* Snapshot the KMS state. */
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "drm init: drmModeGetResources failed");
        drmDropMaster(fd);
        close(fd);
        st->drm_fd = -1;
        g_drm_state = NULL;
        free(st->prop_cache);
        free(st);
        self->impl = NULL;
        return ISZ_ERR_FEATURE_UNAVAIL;
    }
    snapshot_crtcs(res, st);
    snapshot_planes(fd, st);

    /* Initial connector enumeration. */
    size_t nconn = res->count_connectors;
    if (nconn > ISZ_DRM_MAX_CONNECTORS)
        nconn = ISZ_DRM_MAX_CONNECTORS;
    for (size_t i = 0; i < nconn; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn)
            continue;
        snapshot_connector(fd, conn, &st->connectors[st->connector_count]);
        st->connectors[st->connector_count].crtc_id =
            pick_crtc_for_connector(st, conn);
        if (st->connectors[st->connector_count].connected)
            fire_output_hook(st, &st->connectors[st->connector_count], true);
        st->connector_count++;
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);

    /* Multi-GPU render-node enumeration (SPEC 10). */
    enumerate_render_nodes(st);

    isz_log_internal(ISZ_LOG_INFO,
                     "drm init: fd=%d crtcs=%zu planes=%zu connectors=%zu",
                     fd, st->crtc_count, st->plane_count, st->connector_count);
    return ISZ_OK;
}

static int isz_drm_commit(struct isz_backend *self, struct isz_output *out,
                          uint32_t flags)
{
    struct isz_drm_state *st = self->impl;
    if (!st || !st->is_master)
        return ISZ_ERR_DRM_MASTER;
    if (!st->atomic_ok)
        return ISZ_ERR_FEATURE_UNAVAIL;

    int rc = isz_drm_atomic_commit(st, out, flags);
    if (rc < 0)
        return rc;

    /* TEST_ONLY commits are synchronous: no page-flip event will arrive,
     * so finish the state transition inline. */
    if (flags & ISZ_COMMIT_TEST_ONLY)
        isz_backend_finish_commit(self);

    /* Non-test commits stay in COMMITTING until the page-flip event
     * arrives via isz_drm_read_events. */
    return ISZ_OK;
}

static int isz_drm_read_events(struct isz_backend *self)
{
    struct isz_drm_state *st = self->impl;
    if (!st || st->drm_fd < 0)
        return ISZ_ERR_FEATURE_UNAVAIL;
    return isz_drm_event_dispatch(st, self);
}

static void isz_drm_destroy(struct isz_backend *self)
{
    if (!self->impl)
        return;
    struct isz_drm_state *st = self->impl;

    if (g_drm_state == st)
        g_drm_state = NULL;

    /* W5-B: drop any in-flight release refs the backend still holds.
     * The server's pending_releases list is drained by
     * isz_buffer_release_destroy at isz_destroy time; this list holds
     * buffers awaiting their page-flip event, which can't fire after
     * the backend is gone. */
    {
        isz_list_node *node;
        while ((node = isz_list_pop_front(&st->in_flight_releases)) != NULL) {
            struct isz_buffer *buf =
                container_of(node, struct isz_buffer, release_node);
            buf->release_pending = false;
            isz_buffer_release(buf);
            isz_buffer_unref(buf);
        }
    }

    for (size_t i = 0; i < st->connector_count; i++)
        free_connector(&st->connectors[i]);

    if (st->prop_cache) {
        free(st->prop_cache);
        st->prop_cache = NULL;
    }

    if (st->is_master && st->drm_fd >= 0)
        (void)drmDropMaster(st->drm_fd);

    if (st->drm_fd >= 0) {
        close(st->drm_fd);
        st->drm_fd = -1;
    }

#ifdef ISHIZUE_HAVE_LIBSEAT
    if (st->seat) {
        libseat_close_seat(st->seat);
        st->seat = NULL;
    }
#endif

    free(st);
    self->impl = NULL;
}

static void isz_drm_dump(const struct isz_backend *self, FILE *fp)
{
    const struct isz_drm_state *st = self->impl;
    if (!st) {
        fprintf(fp, "  drm: (no state)\n");
        return;
    }
    fprintf(fp, "  drm: fd=%d master=%d atomic=%d\n",
            st->drm_fd, (int)st->is_master, (int)st->atomic_ok);
    fprintf(fp, "    crtcs=%zu planes=%zu connectors=%zu render_nodes=%zu\n",
            st->crtc_count, st->plane_count, st->connector_count,
            st->render_node_count);
    for (size_t i = 0; i < st->connector_count; i++) {
        const struct isz_drm_connector *c = &st->connectors[i];
        fprintf(fp, "    [%s] id=%u crtc=%u %ux%u@%u mHz connected=%d hdr=%d vrr=%d\n",
                c->name, (unsigned)c->connector_id, (unsigned)c->crtc_id,
                (unsigned)c->width, (unsigned)c->height,
                (unsigned)c->refresh_mhz, (int)c->connected,
                (int)c->hdr_capable, (int)c->vrr_capable);
    }
}

/* SPEC 12: blank every CRTC. Async-signal-safe: only drmModeSetCrtc
 * and drmModeFreeCrtc (libdrm's only async-signal-safe-ish calls in
 * practice; this is best-effort under crash conditions). Called from
 * the SIGSEGV/SIGABRT handler installed by isz_enable_crash_recovery. */
static void isz_drm_blank_all_crtcs(struct isz_backend *self)
{
    struct isz_drm_state *st = self->impl;
    if (!st || st->drm_fd < 0)
        return;
    for (size_t i = 0; i < st->crtc_count; i++) {
        (void)drmModeSetCrtc(st->drm_fd, st->crtcs[i].crtc_id,
                             0, 0, 0, NULL, 0, NULL);
    }
}

static const struct isz_backend_ops isz_drm_ops = {
    .init            = isz_drm_init,
    .commit          = isz_drm_commit,
    .read_events     = isz_drm_read_events,
    .destroy         = isz_drm_destroy,
    .dump            = isz_drm_dump,
    .blank_all_crtcs = isz_drm_blank_all_crtcs,
};

const struct isz_backend_ops *isz_drm_get_ops(void)
{
    return &isz_drm_ops;
}

/* ------------------------------------------------------------------ */
/* Parent-layer glue                                                  */
/* ------------------------------------------------------------------ */

void isz_drm_set_output_hook(struct isz_backend *b,
                             isz_drm_output_hook_fn fn, void *userdata)
{
    if (!b || b->type != ISZ_BACKEND_DRM || !b->impl)
        return;
    struct isz_drm_state *st = b->impl;
    st->hook_fn      = fn;
    st->hook_userdata = userdata;
}

int isz_drm_set_server(struct isz_backend *b, struct isz_server *srv)
{
    if (!b || b->type != ISZ_BACKEND_DRM || !b->impl)
        return ISZ_ERR_INVALID_ARG;
    struct isz_drm_state *st = b->impl;
    st->srv = srv;

    /* Add the DRM fd to the server's epoll set with the backend tag so
     * isz_dispatch routes page-flip events to isz_backend_read_events. */
    if (srv && st->drm_fd >= 0) {
        struct isz_fd_tag *tag = &srv->backend_tag;
        tag->kind   = ISZ_FD_BACKEND;
        tag->opaque = NULL;
        struct epoll_event ev;
        ev.events   = EPOLLIN;
        ev.data.ptr = tag;
        if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, st->drm_fd, &ev) < 0) {
            isz_log_internal(ISZ_LOG_WARN,
                             "drm: epoll_ctl ADD drm_fd failed: %s",
                             strerror(errno));
        }
    }
    return ISZ_OK;
}

int isz_drm_get_fds(struct isz_backend *b, int *fds, size_t max)
{
    if (!b || b->type != ISZ_BACKEND_DRM || !b->impl)
        return 0;
    struct isz_drm_state *st = b->impl;
    size_t n = 0;
    if (st->drm_fd >= 0 && n < max) {
        if (fds) fds[n] = st->drm_fd;
        n++;
    }
    return (int)n;
}

void isz_drm_on_session_inactive(struct isz_backend *b)
{
    if (!b || !b->impl)
        return;
    struct isz_drm_state *st = b->impl;
    st->session_active = false;
    if (st->is_master && st->drm_fd >= 0) {
        drmDropMaster(st->drm_fd);
        st->is_master = false;
        isz_log_internal(ISZ_LOG_INFO, "drm: dropped master (session inactive)");
    }
}

void isz_drm_on_session_active(struct isz_backend *b)
{
    if (!b || !b->impl)
        return;
    struct isz_drm_state *st = b->impl;
    st->session_active = true;
    if (st->drm_fd >= 0 && !st->is_master) {
        if (drmSetMaster(st->drm_fd) != 0) {
            isz_log_internal(ISZ_LOG_ERROR,
                             "drm: re-acquire master failed: %s",
                             strerror(errno));
            isz_backend_set_error(b, ISZ_ERR_DRM_MASTER);
            return;
        }
        st->is_master = true;
        isz_log_internal(ISZ_LOG_INFO, "drm: re-acquired master");
    }
}

int isz_drm_get_vt_fd(struct isz_backend *b)
{
    if (!b || !b->impl)
        return -1;
    return ((struct isz_drm_state *)b->impl)->vt_fd;
}

/* Session-active/inactive listener shims. isz_init registers these on
 * the corresponding event types when the DRM backend is selected. The
 * userdata is the struct isz_backend pointer; ev is the SESSION_ACTIVE
 * or SESSION_INACTIVE event from W1-E's libseat listener. */
void isz_drm_session_active_listener(void *userdata, const isz_event *ev)
{
    (void)ev;
    if (userdata)
        isz_drm_on_session_active((struct isz_backend *)userdata);
}

void isz_drm_session_inactive_listener(void *userdata, const isz_event *ev)
{
    (void)ev;
    if (userdata)
        isz_drm_on_session_inactive((struct isz_backend *)userdata);
}

/* ------------------------------------------------------------------ */
/* Buffer import / release (externs declared in isz_buffer.c)         */
/* ------------------------------------------------------------------ */

int isz_buffer_drm_import(struct isz_buffer *buf)
{
    if (!g_drm_state || g_drm_state->drm_fd < 0)
        return ISZ_ERR_FEATURE_UNAVAIL;

    uint32_t handle = 0;
    int rc = drmPrimeFDToHandle(g_drm_state->drm_fd, buf->dmabuf_fd, &handle);
    if (rc < 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "drm: drmPrimeFDToHandle failed: %s",
                         strerror(errno));
        return ISZ_ERR_INVALID_DMABUF;
    }

    /* Stash the GEM handle in buf->priv. malloc'd so the SHM path's
     * munmap branch (which only runs when is_shm) doesn't touch it. */
    uint32_t *p = malloc(sizeof(*p));
    if (!p) {
        drmCloseBufferHandle(g_drm_state->drm_fd, handle);
        return ISZ_ERR_NO_MEMORY;
    }
    *p = handle;
    buf->priv = p;
    buf->priv_size = sizeof(*p);
    return ISZ_OK;
}

void isz_buffer_drm_release(struct isz_buffer *buf)
{
    if (!buf || !buf->priv)
        return;
    if (!g_drm_state || g_drm_state->drm_fd < 0) {
        /* Backend torn down; just free the stash. */
        free(buf->priv);
        buf->priv = NULL;
        buf->priv_size = 0;
        return;
    }
    uint32_t *p = buf->priv;
    (void)drmCloseBufferHandle(g_drm_state->drm_fd, *p);
    free(p);
    buf->priv = NULL;
    buf->priv_size = 0;
}

/* isz_output_get_drm_fd: declared in isz_surface_internal.h. The color
 * helpers use it to create KMS blobs. Returns -1 for non-DRM outputs. */
int isz_output_get_drm_fd(isz_output *out)
{
    if (!out || !out->backend || out->backend->type != ISZ_BACKEND_DRM)
        return -1;
    struct isz_drm_state *st = out->backend->impl;
    return st ? st->drm_fd : -1;
}

/* Screen capture via DRM writeback connector (SPEC §7.11).
 *
 * TODO: real implementation. The writeback connector should be
 * programmed to write the composited frame into the provided
 * dma-buf. For now this is a stub that returns ISZ_ERR_FEATURE_UNAVAIL
 * so the library links. The capture state tracking in
 * src/render/isz_capture.c still works; only the actual pixel
 * capture is missing. */
int isz_capture_drm_start(isz_output *out, int dmabuf_fd,
                          isz_buffer_desc *desc)
{
    (void)out; (void)dmabuf_fd; (void)desc;
    return ISZ_ERR_FEATURE_UNAVAIL;
}

int isz_capture_drm_stop(isz_output *out)
{
    (void)out;
    return ISZ_ERR_FEATURE_UNAVAIL;
}

#endif /* ISHIZUE_HAVE_DRM */
