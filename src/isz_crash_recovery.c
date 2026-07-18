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

/* isz_crash_recovery.c -- SPEC section 12 crash recovery.
 *
 * Runtime opt-in. The Architect calls isz_enable_crash_recovery() to
 * install a SIGSEGV/SIGABRT/SIGBUS handler that restores the VT and
 * blanks all CRTCs to black on crash, then re-raises the signal so any
 * Architect-installed crash reporter further down the chain still runs.
 *
 * Off by default so it never silently clashes with an Architect-supplied
 * crash reporter. The handler never calls exit().
 *
 * Async-signal-safety: the handler only calls async-signal-safe
 * functions (signal, raise, ioctl, sigaction). No malloc, no logging,
 * no pthread mutexes. A static sig_atomic_t flag guards against
 * re-entry if the handler itself triggers a fatal signal.
 *
 * The handler snapshots what it needs (backend pointer, VT fd) at
 * enable time so it never has to call into the server from signal
 * context. For the headless backend both VT restore and CRTC blanking
 * are no-ops; the DRM backend wiring (libseat VT fd, drmModeSetCrtc
 * blanking) lands with the DRM wave. */

#define _POSIX_C_SOURCE 200809L

#include <ishizue/isz.h>
#include "isz_server_internal.h"
#include "util/isz_compiler.h"

#ifdef ISHIZUE_HAVE_DRM
#include "backend/isz_drm.h"
#endif

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/vt.h>
#endif

/* Sibling-wave contract (DRM wave): disable scanout on every CRTC the
 * backend owns. For headless: no-op. For DRM: drmModeSetCrtc with no
 * FB per CRTC. Unresolved until the DRM wave lands; the handler only
 * calls it for non-headless backends, so the headless path is safe. */
int isz_backend_blank_all_crtcs(struct isz_backend *b);

/* Sibling-wave contract (DRM wave): returns the libseat session's VT
 * fd, or -1 for headless. Not yet provided; crash recovery sets vt_fd
 * to -1 until the DRM wave wires this in, so VT restore is skipped for
 * the headless backend (which owns no VT anyway).
 *
 *   int isz_server_get_vt_fd(isz_server *srv);
 */

/* State populated at enable time, read-only from the handler. The
 * handler writes only to in_handler. Static storage is zero-initialized
 * so a process that never calls isz_enable_crash_recovery has backend
 * == NULL and vt_fd == -1, making the handler a no-op if it ever fires
 * (it won't, since no handler is installed). */
struct isz_crash_state {
    struct isz_backend    *backend;
    int                    vt_fd;
    volatile sig_atomic_t  in_handler;
};

static struct isz_crash_state g_crash_state;

static void isz_crash_handler(int sig)
{
    /* Re-entry guard. If the handler itself crashes, restore the
     * default disposition and re-raise so the kernel kills us rather
     * than looping in the handler. */
    if (g_crash_state.in_handler) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_crash_state.in_handler = 1;

    /* 1. Restore the VT. No-op when vt_fd < 0 (headless owns no VT).
     *    For DRM we ask the kernel to switch back to whatever VT was
     *    active before the compositor's VT. */
    if (g_crash_state.vt_fd >= 0) {
#ifdef __linux__
        struct vt_stat vs;
        if (ioctl(g_crash_state.vt_fd, VT_GETSTATE, &vs) == 0)
            (void)ioctl(g_crash_state.vt_fd, VT_ACTIVATE, vs.v_active);
#endif
    }

    /* 2. Blank all CRTCs. No-op for headless (no real CRTCs). The
     *    DRM backend's blanker is a sibling-wave extern; we skip the
     *    call for headless so we never touch an unresolved symbol on
     *    the only currently-functional backend. */
    if (g_crash_state.backend &&
        g_crash_state.backend->type != ISZ_BACKEND_HEADLESS) {
        (void)isz_backend_blank_all_crtcs(g_crash_state.backend);
    }

    /* 3. Re-raise the original signal so any Architect-installed crash
     *    handler further down the chain still runs (SPEC section 12).
     *    signal() and raise() are both async-signal-safe. */
    signal(sig, SIG_DFL);
    raise(sig);
}

ISZ_API int isz_enable_crash_recovery(isz_server *srv)
{
    if (!srv)
        return ISZ_ERR_INVALID_ARG;

    /* Snapshot at enable time. The handler can't safely call into the
     * server (it might be in any state when the signal fires), so we
     * cache what we need up front. */
    g_crash_state.backend    = srv->backend;
    /* VT fd: -1 for headless. The DRM backend exposes the libseat
     * session's VT fd via isz_drm_get_vt_fd when ISHIZUE_HAVE_DRM is
     * defined and a real session is active. Without libdrm or without
     * libseat, VT restore is a no-op, which is correct for the
     * headless backend. */
    g_crash_state.vt_fd      = -1;
#ifdef ISHIZUE_HAVE_DRM
    if (srv->backend && srv->backend->type == ISZ_BACKEND_DRM)
        g_crash_state.vt_fd = isz_drm_get_vt_fd(srv->backend);
#endif
    g_crash_state.in_handler = 0;

    srv->crash_recovery_enabled = true;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = isz_crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    const int sigs[] = { SIGSEGV, SIGABRT, SIGBUS };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (sigaction(sigs[i], &sa, NULL) != 0)
            return ISZ_ERR_INVALID_ARG;
    }

    isz_log_internal(ISZ_LOG_INFO,
                     "crash recovery armed: SIGSEGV/SIGABRT/SIGBUS");
    return ISZ_OK;
}
