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
 *   - calls drmSetMaster on the direct-open path and fails fast with
 *     ISZ_ERR_DRM_MASTER on EBUSY / EPERM (SPEC 3); skips the call
 *     when libseat opened the fd (already master, EINVAL otherwise)
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
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/vt.h>
#include <sys/kd.h>
/* <linux/kd.h> is where KD_GRAPHICS, KD_TEXT, K_OFF, and K_UNICODE
 * are actually defined. glibc's <sys/kd.h> pulls it in transitively,
 * but we include it explicitly so the dependency is visible at the
 * call site (KDSETMODE/KDSKBMODE in setup_vt_signals). */
#include <linux/kd.h>

/* ------------------------------------------------------------------ */
/* Backend-private state                                              */
/* ------------------------------------------------------------------ */

/* Global so isz_buffer_drm_import can reach the DRM fd without
 * threading it through the buffer API. The DRM backend is a
 * singleton per server; single-threaded per SPEC 5, so no lock. */
static struct isz_drm_state *g_drm_state;

/* Direct VT handling (fallback when libseat is unavailable).
 *
 * The kernel sends SIGUSR1 when another VT is requested and
 * SIGUSR2 when switching back. We must:
 *   SIGUSR1: drmDropMaster + ioctl(VT_RELDISP, 1) to ack
 *   SIGUSR2: drmSetMaster + ioctl(VT_RELDISP, 2) to ack
 *
 * Without VT_RELDISP the kernel blocks the switch.
 * Without drmDropMaster the CRTC stays locked.
 *
 * This mirrors what weston does in libweston/compositor.c
 * when running without logind. */
static volatile sig_atomic_t g_vt_switch_away = 0;

static void on_vt_switch_away(int sig) {
    (void)sig;
    g_vt_switch_away = 1;
}

static void on_vt_switch_back(int sig) {
    (void)sig;
    g_vt_switch_away = 0;
}

static int setup_vt_signals(struct isz_drm_state *st) {
    /* Open the active VT by number, not /dev/tty. /dev/tty is the
     * controlling TTY, which may not be a VT at all when running under
     * SSH or as a daemon. The reliable pattern (Weston launcher-direct,
     * Xorg lnx_init.c) is VT_GETSTATE on /dev/tty0 to discover the
     * active VT number, then open /dev/ttyN for that specific N. */
    int ctl_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (ctl_fd < 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "drm: cannot open /dev/tty0: %s", strerror(errno));
        return -1;
    }
    struct vt_stat vs;
    memset(&vs, 0, sizeof(vs));
    if (ioctl(ctl_fd, VT_GETSTATE, &vs) < 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "drm: VT_GETSTATE failed: %s", strerror(errno));
        close(ctl_fd);
        return -1;
    }
    int active = vs.v_active;
    close(ctl_fd);

    char vt_path[32];
    int rc = snprintf(vt_path, sizeof(vt_path), "/dev/tty%d", active);
    if (rc <= 0 || rc >= (int)sizeof(vt_path)) {
        isz_log_internal(ISZ_LOG_WARN,
                         "drm: VT number %d out of range", active);
        return -1;
    }
    st->vt_fd = open(vt_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (st->vt_fd < 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "drm: cannot open %s: %s", vt_path, strerror(errno));
        return -1;
    }

    /* Switch the VT into graphics mode so the kernel stops rendering
     * text onto the scanout. Without KDSETMODE(KD_GRAPHICS) the user
     * sees the DRM output and the text console overlaid. Weston,
     * Xorg, and seatd all set this. */
    if (ioctl(st->vt_fd, KDSETMODE, KD_GRAPHICS) < 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "drm: KDSETMODE(KD_GRAPHICS) failed: %s",
                         strerror(errno));
        /* Continue: the modeset will still blank the console. */
    }

    /* Disable kernel-processed keyboard input on the VT. Without
     * KDSKBMODE(K_OFF) the keyboard sends both kernel-processed input
     * (to the text VT) and libinput events, so every keypress doubles.
     * K_OFF (vs. K_RAW) keeps the keyboard state sane for the next VT
     * owner. */
    if (ioctl(st->vt_fd, KDSKBMODE, K_OFF) < 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "drm: KDSKBMODE(K_OFF) failed: %s",
                         strerror(errno));
    }

    /* Set VT mode to VT_PROCESS so the kernel sends SIGUSR1/SIGUSR2
     * instead of auto-switching. relsig = SIGUSR1 (switch away),
     * acqsig = SIGUSR2 (switch back). */
    struct vt_mode mode;
    memset(&mode, 0, sizeof(mode));
    mode.mode = VT_PROCESS;
    mode.relsig = SIGUSR1;
    mode.acqsig = SIGUSR2;
    if (ioctl(st->vt_fd, VT_SETMODE, &mode) < 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "drm: VT_SETMODE failed: %s", strerror(errno));
        close(st->vt_fd);
        st->vt_fd = -1;
        return -1;
    }

    /* Install signal handlers. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_vt_switch_away;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_vt_switch_back;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR2, &sa, NULL);

    isz_log_internal(ISZ_LOG_INFO,
                     "drm: VT signals installed on %s (SIGUSR1/SIGUSR2)",
                     vt_path);
    return 0;
}

/* Drop every entry on st->in_flight_releases without delivering
 * ISZ_MSG_RELEASE. Defined in isz_drm_event.c; declared here so the
 * VT-resume path and the destroy path can share it. The header
 * isz_drm_event.h only exposes isz_drm_event_dispatch, so this is a
 * TU-local forward declaration. */
extern void isz_drm_drop_inflight_releases(struct isz_drm_state *st);

/* Bug 8: libinput holds evdev fds that the kernel revokes during a VT
 * switch (libseat path) or that go stale while we are not master
 * (direct VT path). libinput_suspend closes them; libinput_resume
 * re-opens them via the open_restricted callback. Without this pair,
 * input stops working after one VT switch. Mirrors wlroots
 * backend/libinput/backend.c:185-189 and Aquamarine Session.cpp:82-83,
 * 90-91. */
static void drm_suspend_input(struct isz_drm_state *st)
{
    if (!st || !st->srv)
        return;
#ifdef ISHIZUE_HAVE_LIBINPUT
    struct isz_input_state *istate = isz_server_get_input_state(st->srv);
    if (istate && istate->li)
        libinput_suspend(istate->li);
#else
    /* libinput not built in; nothing to suspend. */
#endif
}

/* Bug 8 + 9 + 10 resume sequence. Called after drmSetMaster succeeds
 * on either the direct VT path (vt_dispatch SIGUSR2 branch) or the
 * libseat path (drm_enable_seat).
 *
 *   1. libinput_resume: re-open evdev fds the kernel revoked.
 *   2. isz_backend_finish_commit: a commit that was in flight when
 *      the VT switch hit will never get its page-flip event (the
 *      CRTC was disabled). Transition COMMITTING -> READY so the next
 *      commit is not rejected with ISZ_ERR_COMMIT_PENDING. Safe no-op
 *      when the state is already READY.
 *   3. isz_drm_drop_inflight_releases: same reason, but for the
 *      per-buffer refs on st->in_flight_releases.
 *   4. Re-commit each enabled DRM output. The CRTC was disabled by
 *      the new VT owner (or by drmDropMaster); without this, the
 *      screen stays black on switch back. Mirrors wlroots
 *      backend/drm/backend.c:107-122 and Aquamarine DRM.cpp:423-470. */
static void drm_resume_after_vt(struct isz_drm_state *st)
{
    if (!st || !st->backend)
        return;

#ifdef ISHIZUE_HAVE_LIBINPUT
    if (st->srv) {
        struct isz_input_state *istate = isz_server_get_input_state(st->srv);
        if (istate && istate->li) {
            if (libinput_resume(istate->li) != 0) {
                isz_log_internal(ISZ_LOG_WARN,
                                 "drm: libinput_resume failed on VT resume");
            }
        }
    }
#endif

    isz_backend_finish_commit(st->backend);
    isz_drm_drop_inflight_releases(st);

    if (!st->srv)
        return;

    size_t n = 0;
    isz_output **outs = isz_output_list(st->srv, &n);
    if (!outs || n == 0)
        return;
    for (size_t i = 0; i < n; i++) {
        isz_output *out = outs[i];
        if (!out || !out->is_drm || !out->enabled)
            continue;
        int rc = isz_commit(out, ISZ_COMMIT_NORMAL);
        if (rc < 0) {
            isz_log_internal(ISZ_LOG_WARN,
                             "drm: re-commit output %s after VT resume rc=%d",
                             out->name, rc);
        }
    }
}

static void vt_dispatch(struct isz_drm_state *st) {
    if (st->vt_fd < 0)
        return;

    if (g_vt_switch_away && st->is_master) {
        isz_log_internal(ISZ_LOG_INFO,
                         "drm: VT switch away, dropping master");
        drm_suspend_input(st);
        drmDropMaster(st->drm_fd);
        st->is_master = false;
        st->session_active = false;
        /* Acknowledge the switch so the kernel proceeds.
         * MUST be called AFTER drmDropMaster: the kernel requires
         * DRM_IOCTL_DROP_MASTER before VT_RELDISP. */
        ioctl(st->vt_fd, VT_RELDISP, 1);
        /* Emit SESSION_INACTIVE. On the direct-VT path there is no
         * libseat seat callback to emit it, so we emit here. The
         * Architect listens for this to pause rendering. */
        if (st->srv) {
            isz_event ev = { .type = ISZ_EVENT_SESSION_INACTIVE };
            isz_server_emit_event(st->srv, &ev);
        }
    } else if (!g_vt_switch_away && !st->is_master) {
        isz_log_internal(ISZ_LOG_INFO,
                         "drm: VT switch back, acquiring master");
        if (drmSetMaster(st->drm_fd) == 0) {
            st->is_master = true;
            st->session_active = true;
            drm_resume_after_vt(st);
            /* Emit SESSION_ACTIVE only on a successful re-acquire.
             * If drmSetMaster failed, the compositor is not master
             * and the Architect should not resume rendering. */
            if (st->srv) {
                isz_event ev = { .type = ISZ_EVENT_SESSION_ACTIVE };
                isz_server_emit_event(st->srv, &ev);
            }
        } else {
            isz_log_internal(ISZ_LOG_ERROR,
                             "drm: drmSetMaster on VT return failed: %s",
                             strerror(errno));
        }
        /* Acknowledge the switch back. */
        ioctl(st->vt_fd, VT_RELDISP, 2);
    }
}

/* Public wrapper so isz_dispatch can call vt_dispatch on every
 * iteration. The signal handler sets g_vt_switch_away; this function
 * checks it and drops/acquires master. Without calling this on every
 * dispatch tick, the SIGUSR1 flag is never checked (epoll_wait returns
 * EINTR but no fd is ready, so isz_drm_read_events is never reached). */
void isz_drm_vt_dispatch(struct isz_backend *b) {
    if (!b || !b->impl)
        return;
    struct isz_drm_state *st = b->impl;
    if (st->drm_fd >= 0)
        vt_dispatch(st);
}

static void teardown_vt_signals(struct isz_drm_state *st) {
    if (st->vt_fd >= 0) {
        /* Restore VT_PROCESS -> VT_AUTO before restoring the keyboard
         * and display modes: the kernel only honours KDSETMODE /
         * KDSKBMODE on the controlling VT, and VT_AUTO is the safe
         * default for hand-off. Order matters when running on a real
         * TTY: VT_SETMODE first stops the SIGUSR1/SIGUSR2 flow. */
        struct vt_mode mode;
        memset(&mode, 0, sizeof(mode));
        mode.mode = VT_AUTO;
        ioctl(st->vt_fd, VT_SETMODE, &mode);

        /* Undo KDSKBMODE(K_OFF). K_UNICODE is the kernel default and
         * matches what a normal login getty expects. */
        ioctl(st->vt_fd, KDSKBMODE, K_UNICODE);

        /* Undo KDSETMODE(KD_GRAPHICS) so the text console is visible
         * again after teardown. */
        ioctl(st->vt_fd, KDSETMODE, KD_TEXT);

        close(st->vt_fd);
        st->vt_fd = -1;
    }
    /* Restore default signal handlers. */
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
}

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
        /* Store the full mode for the atomic commit path. The kernel
         * needs the real clock + hsync/vsync/htotal/vtotal; fabricating
         * them from width/height/refresh makes drmModeAtomicCommit
         * return EINVAL. */
        out->mode = *m;
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

/* Find an unused CRTC for a connector. Walks the connector's
 * encoder list (conn->encoders[], falling back to conn->encoder_id
 * for older kernels). Returns the CRTC id via *crtc_id and the
 * CRTC's bit mask (for plane possible_crtcs matching) via *crtc_mask.
 * Returns 0 on failure. */
static void pick_crtc_for_connector(struct isz_drm_state *st,
                                    drmModeConnector *conn,
                                    uint32_t *crtc_id,
                                    uint32_t *crtc_mask)
{
    *crtc_id = 0;
    *crtc_mask = 0;
    if (!conn || !st)
        return;

    /* Build the set of encoder ids to try. Prefer conn->encoder_id
     * (the currently-bound encoder); fall back to conn->encoders[]
     * (the full list the connector can route through). On a freshly
     * mastered TTY, encoder_id is often 0 because no encoder is
     * bound yet. */
    uint32_t enc_ids[16];
    size_t n_enc = 0;
    if (conn->encoder_id && n_enc < 16)
        enc_ids[n_enc++] = conn->encoder_id;
    for (int i = 0; i < conn->count_encoders && n_enc < 16; i++) {
        uint32_t eid = conn->encoders[i];
        bool dup = false;
        for (size_t j = 0; j < n_enc; j++)
            if (enc_ids[j] == eid) { dup = true; break; }
        if (!dup)
            enc_ids[n_enc++] = eid;
    }

    for (size_t e = 0; e < n_enc; e++) {
        drmModeEncoder *enc = drmModeGetEncoder(st->drm_fd, enc_ids[e]);
        if (!enc)
            continue;
        for (size_t i = 0; i < st->crtc_count; i++) {
            if (enc->possible_crtcs & (1u << i)) {
                *crtc_id   = st->crtcs[i].crtc_id;
                *crtc_mask = (1u << i);
                drmModeFreeEncoder(enc);
                return;
            }
        }
        drmModeFreeEncoder(enc);
    }
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
        pick_crtc_for_connector(st, conn,
                                &fresh[fresh_count].crtc_id,
                                &fresh[fresh_count].crtc_mask);
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
    /* libseat_open_device returns a device_id (>= 0) on success
     * and writes the fd to the third argument. It does NOT return
     * the fd itself. Passing NULL as the third arg causes a
     * segfault because libseat writes the fd to *fd. */
    if (!st->seat || !device_path)
        return -1;
    int fd = -1;
    int dev_id = libseat_open_device(st->seat, device_path, &fd);
    if (dev_id < 0 || fd < 0)
        return -1;
    /* TODO: store dev_id so we can libseat_close_device on teardown. */
    return fd;
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

#ifdef ISHIZUE_HAVE_LIBSEAT
/* libseat seat listener. disable_seat fires when the seatd daemon has
 * already dropped DRM master on its fd (which shares the same struct
 * file as our drm_fd) and is telling us the VT is switching away.
 * enable_seat fires after the daemon re-acquired master on switch-back.
 *
 * The compositor must NOT call drmDropMaster / drmSetMaster here: the
 * daemon already did it on its side of the shared file descriptor, so
 * a second call from us returns EINVAL (not master / already master)
 * and logs noise. wlroots and Aquamarine do not call drmSetMaster /
 * drmDropMaster in their seat callbacks either.
 *
 * What we do here: flip session_active / is_master to mirror the
 * daemon's state, acknowledge the switch so the kernel proceeds, and
 * emit ISZ_EVENT_SESSION_INACTIVE / _ACTIVE so the Architect can pause
 * and resume rendering. */
static void drm_disable_seat(struct libseat *seat, void *userdata) {
    (void)seat;
    struct isz_drm_state *st = userdata;
    if (!st || !st->seat || st->drm_fd < 0) return;
    st->session_active = false;
    st->is_master = false;
    isz_log_internal(ISZ_LOG_INFO, "drm: seat disabled (VT switch away)");
    /* Acknowledge the VT switch so the kernel can proceed. libseat
     * requires this; without it the switch hangs. */
    libseat_disable_seat(st->seat);
    /* Emit SESSION_INACTIVE so the Architect can pause rendering. This
     * is the primary emission path on the libseat seat callback; the
     * DRM backend no longer registers its own listener for the event
     * (W15-C: removed the duplicate listener from isz_lifecycle.c). */
    if (st->srv) {
        isz_event ev = { .type = ISZ_EVENT_SESSION_INACTIVE };
        isz_server_emit_event(st->srv, &ev);
    }
}

static void drm_enable_seat(struct libseat *seat, void *userdata) {
    (void)seat;
    struct isz_drm_state *st = userdata;
    if (!st || !st->seat || st->drm_fd < 0) return;
    /* The daemon re-acquired master before sending SERVER_ENABLE_SEAT.
     * If re-acquisition had failed, libseat would not have sent the
     * enable callback, so we trust the state and do not call
     * drmSetMaster ourselves. */
    st->session_active = true;
    st->is_master = true;
    isz_log_internal(ISZ_LOG_INFO, "drm: seat enabled (VT switch back)");
    if (st->srv) {
        isz_event ev = { .type = ISZ_EVENT_SESSION_ACTIVE };
        isz_server_emit_event(st->srv, &ev);
    }
}

static const struct libseat_seat_listener drm_seat_listener = {
    .enable_seat  = drm_enable_seat,
    .disable_seat = drm_disable_seat,
};
#endif

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
     * libseat_open_seat fails we fall through to a direct open.
     *
     * The seat listener is mandatory: without disable_seat, the kernel
     * blocks VT switches because drmDropMaster is never called. */
    st->seat = libseat_open_seat(&drm_seat_listener, st);
    if (st->seat) {
        /* Don't dispatch here. enable_seat would fire before drm_fd
         * is set, and the callback's guard would return early but
         * leave the seat un-acknowledged. The first
         * isz_drm_read_events call drains the seat after init is
         * complete and drm_fd is valid. */
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

    /* SPEC 3: acquire master, fail fast on EBUSY/EPERM.
     *
     * Only call drmSetMaster on the direct-open path (st->seat == NULL).
     * When libseat opens the DRM fd, the fd shares the kernel struct
     * file with seatd's fd and is already master. drmSetMaster on an
     * already-master fd returns EINVAL, which we previously treated as
     * fatal. wlroots and Aquamarine never call drmSetMaster at all when
     * running under libseat. */
    if (st->seat) {
        st->is_master = true;
    } else if (drmSetMaster(fd) != 0) {
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
    } else {
        st->is_master = true;
    }

    /* Set up VT switching. If libseat is available and working, it
     * handles VT switches via disable_seat/enable_seat callbacks.
     * If libseat is unavailable (no seatd daemon, no logind), we
     * install direct SIGUSR1/SIGUSR2 handlers so the kernel can
     * signal us to drop/acquire master on VT switch.
     *
     * Without this, the kernel blocks VT switches because DRM
     * master is held and never released. The user gets stuck. */
    if (!st->seat) {
        setup_vt_signals(st);
    }

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
        pick_crtc_for_connector(st, conn,
                                &st->connectors[st->connector_count].crtc_id,
                                &st->connectors[st->connector_count].crtc_mask);
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

    /* Drain the libseat session. enable_seat / disable_seat fire
     * inline here, triggering drmDropMaster / drmSetMaster on VT
     * switches. */
#ifdef ISHIZUE_HAVE_LIBSEAT
    if (st->seat) {
        while (libseat_dispatch(st->seat, 0) > 0) {
            /* keep draining */
        }
    }
#endif

    /* Direct VT handling: if libseat is unavailable, check the
     * SIGUSR1/SIGUSR2 flags set by the signal handlers and
     * drop/acquire master accordingly. vt_dispatch is a no-op
     * when vt_fd < 0 (libseat is handling it). */
    vt_dispatch(st);

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

    /* Restore VT auto mode and default signal handlers. */
    teardown_vt_signals(st);

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

    /* Add the libseat session fd to the epoll set so VT switch
     * events are polled. Without this, libseat_dispatch only runs
     * when the DRM fd has events (page-flips), and disable_seat
     * never fires. The kernel blocks the VT switch because
     * drmDropMaster is never called. */
#ifdef ISHIZUE_HAVE_LIBSEAT
    if (srv && st->seat) {
        int seat_fd = libseat_get_fd(st->seat);
        if (seat_fd >= 0) {
            struct isz_fd_tag *tag = &srv->seat_tag;
            tag->kind   = ISZ_FD_SEAT;
            tag->opaque = NULL;
            struct epoll_event ev;
            ev.events   = EPOLLIN;
            ev.data.ptr = tag;
            if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, seat_fd, &ev) < 0) {
                isz_log_internal(ISZ_LOG_WARN,
                                 "drm: epoll_ctl ADD seat_fd failed: %s",
                                 strerror(errno));
            }
        }
    }
#endif
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

const struct isz_drm_connector *isz_drm_get_connectors(struct isz_backend *b,
                                                       size_t *n)
{
    if (!b || !b->impl || !n)
        return NULL;
    struct isz_drm_state *st = b->impl;
    *n = st->connector_count;
    return st->connectors;
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
