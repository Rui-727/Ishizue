/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Rui-727
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* isz_lifecycle.c - public server lifecycle (SPEC §5, §7.6, §10, §12).
 *
 * isz_init: allocate the server, create the backend (dispatches by type
 * via isz_backend_create), create the default seat, spin up the thread
 * pool (W2-C), arm the PSI monitor and add its fd to the epoll set,
 * register the headless output hook if the backend is headless, and
 * leave the epoll set ready for isz_listen to add the listen fd. Returns
 * NULL on failure with an isz_log_internal error.
 *
 * isz_dispatch: one non-blocking iteration. Drains the epoll set (1
 * iteration, timeout 0) and routes each ready fd by tag: listen fd →
 * isz_accept_connection (W3-A), client fd → isz_recv_client_messages
 * (W3-A), PSI fd → isz_psi_dispatch, backend fd → isz_backend_read_events.
 * libinput + libseat are drained unconditionally (they're no-ops until
 * the DRM wave wires their fds in). Subsystems emit events straight to
 * listeners via isz_server_emit_event; dispatch itself does not walk the
 * listener registry.
 *
 * isz_get_fds: returns the pollable fds the Architect should add to
 * their own epoll set: backend fds (currently none for headless), the
 * libinput fd, the PSI fd, and client sockets. The listen fd is owned
 * by the library's epoll set after isz_listen; the Architect doesn't
 * need to poll it themselves.
 *
 * isz_destroy (SPEC §7.6): mark DESTROYING, disconnect clients
 * (isz_conn_close + emit CLIENT_DISCONNECT), close the listen fd,
 * destroy outputs, destroy the default seat / input state, drain the
 * thread pool (blocking until queued work finishes, per W2-C), destroy
 * the backend, close epoll, free.
 *
 * The cross-wave accessors isz_server_get_input_state /
 * isz_server_set_input_state / isz_server_emit_event /
 * isz_server_get_thread_pool live here too; the W1-E input layer and
 * the W2-C thread-pool shim call them. */
#define _POSIX_C_SOURCE 200809L

#include "isz_server_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "util/isz_log.h"
#include "util/isz_thread.h"
#include "util/isz_thread_pool.h"
#include "backend/isz_backend.h"
#include "backend/isz_headless.h"
#ifdef ISHIZUE_HAVE_DRM
#include "backend/isz_drm.h"
#endif
#include "buffer/isz_buffer.h"
#include "buffer/isz_psi.h"
#include "input/isz_seat_internal.h"
#include "protocol/isz_conn.h"

/* ------------------------------------------------------------------ */
/* Headless output hook                                               */
/* ------------------------------------------------------------------ */
/* The headless backend fires this on output add/remove. We wrap the
 * info into an isz_output on add (isz_server_wrap_headless_output)
 * and emit OUTPUT_REMOVE on remove without destroying the wrapper
 * (SPEC §10: the Architect decides when to call isz_output_destroy). */
static void isz_headless_output_hook(void *userdata,
                                     const struct isz_headless_output_info *info,
                                     bool added)
{
    isz_server *srv = userdata;
    if (!srv || !info)
        return;
    if (srv->state == ISZ_SERVER_DESTROYING)
        return;  /* teardown owns the wrappers now */
    if (added)
        isz_server_wrap_headless_output(srv, info);
    else
        isz_server_unwrap_headless_output(srv, info->id);
}

#ifdef ISHIZUE_HAVE_DRM
/* DRM output hook: fires for hotplug after isz_init. Initial
 * connectors are wrapped by isz_init directly via
 * isz_drm_get_connectors; this hook covers the rescan path. */
static void isz_drm_output_hook(void *userdata,
                                const struct isz_drm_output_info *info,
                                bool added)
{
    isz_server *srv = userdata;
    if (!srv || !info)
        return;
    if (srv->state == ISZ_SERVER_DESTROYING)
        return;
    /* The DRM hook receives an isz_drm_output_info (snapshot struct),
     * but isz_server_wrap_drm_output takes an isz_drm_connector
     * (the persistent struct). Look up the connector by id and wrap
     * that, so the wrapper has the full mode list + EDID. */
    if (added) {
        size_t n = 0;
        const struct isz_drm_connector *conns =
            isz_drm_get_connectors(srv->backend, &n);
        for (size_t i = 0; i < n; i++) {
            if (conns[i].connector_id == info->connector_id) {
                isz_server_wrap_drm_output(srv, &conns[i]);
                break;
            }
        }
    } else {
        isz_server_unwrap_drm_output(srv, info->connector_id);
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Cross-wave accessors                                               */
/* ------------------------------------------------------------------ */
struct isz_input_state *isz_server_get_input_state(isz_server *srv)
{
    return srv ? srv->input_state : NULL;
}

void isz_server_set_input_state(isz_server *srv, struct isz_input_state *st)
{
    if (srv)
        srv->input_state = st;
}

struct isz_thread_pool *isz_server_get_thread_pool(isz_server *srv)
{
    return srv ? srv->thread_pool : NULL;
}

/* ------------------------------------------------------------------ */
/* isz_init (SPEC §7.6, §10)                                          */
/* ------------------------------------------------------------------ */
ISZ_API isz_server *isz_init(enum isz_backend_type backend, void *backend_config)
{
    isz_server *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        isz_log_internal(ISZ_LOG_ERROR, "isz_init: out of memory");
        return NULL;
    }

    srv->state         = ISZ_SERVER_UNINITIALIZED;
    srv->backend_type  = backend;
    srv->epoll_fd      = -1;
    srv->listen_fd     = -1;
    srv->next_output_id = 1;
    srv->log_level     = ISZ_LOG_WARN;

    isz_list_init(&srv->outputs);
    isz_list_init(&srv->clients);
    isz_list_init(&srv->allowlist_binaries);
    isz_list_init(&srv->allowlist_cgroups);
    for (size_t i = 0; i < ISZ_LISTENER_BUCKETS; i++)
        isz_list_init(&srv->listeners[i]);

    isz_buffer_cache_init(&srv->buffer_cache);
    isz_list_init(&srv->pending_releases);
    isz_psi_init(&srv->psi);

    srv->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv->epoll_fd < 0) {
        isz_log_internal(ISZ_LOG_ERROR, "isz_init: epoll_create1 failed");
        goto fail_psi;
    }

    srv->backend = isz_backend_create(backend, backend_config);
    if (!srv->backend)
        goto fail_epoll;

    /* Wire the headless output hook and wrap the default output that
     * init() already created. The hook fires for future hotplugs. */
    if (backend == ISZ_BACKEND_HEADLESS) {
        isz_headless_set_output_hook(srv->backend, isz_headless_output_hook, srv);
        size_t n = 0;
        const struct isz_headless_output *const *hs =
            isz_headless_outputs(srv->backend, &n);
        for (size_t i = 0; i < n; i++) {
            struct isz_headless_output_info info;
            info.id          = hs[i]->id;
            info.width       = hs[i]->width;
            info.height      = hs[i]->height;
            info.refresh_mhz = hs[i]->refresh_mhz;
            info.enabled     = hs[i]->enabled;
            memcpy(info.name, hs[i]->name, sizeof(info.name));
            if (isz_server_wrap_headless_output(srv, &info) < 0) {
                isz_log_internal(ISZ_LOG_WARN,
                                 "isz_init: failed to wrap headless output %u",
                                 (unsigned)hs[i]->id);
            }
        }
    }

#ifdef ISHIZUE_HAVE_DRM
    /* DRM backend: wire the backend's server back-channel so it adds its
     * DRM fd to srv->epoll_fd with the backend tag (W3-A handoff). The
     * real wiring happens inside isz_drm_set_server; the dispatch loop's
     * ISZ_FD_BACKEND handler already routes ready events to
     * isz_backend_read_events. */
    if (backend == ISZ_BACKEND_DRM) {
        isz_drm_set_server(srv->backend, srv);
        isz_drm_set_output_hook(srv->backend, isz_drm_output_hook, srv);
        /* Wrap initial connectors that isz_drm_init already enumerated.
         * The hook fires for hotplugs going forward; this loop covers
         * the connectors discovered before the hook was registered. */
        size_t n = 0;
        const struct isz_drm_connector *conns =
            isz_drm_get_connectors(srv->backend, &n);
        for (size_t i = 0; i < n; i++) {
            if (conns[i].connected) {
                isz_server_wrap_drm_output(srv, &conns[i]);
            }
        }
    }
#endif

    /* Create the default seat (W1-E). For the DRM backend, libseat +
     * libinput are wired by the DRM wave; for headless, the seat
     * exists but has no devices. */
    if (!isz_seat_default(srv)) {
        isz_log_internal(ISZ_LOG_ERROR, "isz_init: default seat creation failed");
        goto fail_backend;
    }

    /* Thread pool (W2-C). NULL on failure or when ENABLE_THREAD_POOL=0;
     * isz_thread_pool_submit returns -1 in that case. The pool is
     * optional, so failure to allocate is non-fatal; we log and move
     * on with srv->thread_pool == NULL. */
    srv->thread_pool = isz_thread_pool_create(ISZ_THREAD_POOL_SIZE);
    if (!srv->thread_pool)
        isz_log_internal(ISZ_LOG_INFO,
                         "isz_init: thread pool unavailable (ENABLE_THREAD_POOL=0 or alloc failure)");

    /* PSI monitor: if init armed the fd, add it to the epoll set so
     * isz_dispatch can drain trigger events. When PSI is unavailable
     * (kernel < 4.20 or CONFIG_PSI=n) the fd stays -1 and we skip the
     * epoll_ctl. */
    int psi_fd = isz_psi_get_fd(&srv->psi);
    if (psi_fd >= 0) {
        srv->psi_tag.kind   = ISZ_FD_PSI;
        srv->psi_tag.opaque = NULL;
        struct epoll_event pev;
        pev.events   = EPOLLIN;
        pev.data.ptr = &srv->psi_tag;
        if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, psi_fd, &pev) < 0) {
            isz_log_internal(ISZ_LOG_WARN,
                             "isz_init: epoll_ctl ADD psi failed: %s",
                             strerror(errno));
        }
    }

    /* Backend fds: the headless backend owns no fds. The DRM backend
     * wave adds the DRM fd + vblank fds here and tags them
     * ISZ_FD_BACKEND so dispatch routes them to isz_backend_read_events. */

    srv->state = ISZ_SERVER_RUNNING;
    isz_log_internal(ISZ_LOG_INFO, "isz_init: backend=%d state=RUNNING",
                     (int)backend);
    return srv;

fail_backend:
    isz_backend_destroy(srv->backend);
fail_epoll:
    if (srv->epoll_fd >= 0)
        close(srv->epoll_fd);
fail_psi:
    isz_psi_destroy(&srv->psi);
    isz_buffer_cache_destroy(&srv->buffer_cache);
    free(srv);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* isz_dispatch (SPEC §5)                                             */
/* ------------------------------------------------------------------ */
ISZ_API void isz_dispatch(isz_server *srv)
{
    if (!srv || srv->state != ISZ_SERVER_RUNNING)
        return;

    /* Drain our epoll set, one non-blocking tick. Each ready fd carries
     * a struct isz_fd_tag * in epoll_data.ptr; route by tag->kind. */
    if (srv->epoll_fd >= 0) {
        struct epoll_event evs[16];
        int n = epoll_wait(srv->epoll_fd, evs, 16, 0);
        for (int i = 0; i < n; i++) {
            struct isz_fd_tag *tag = evs[i].data.ptr;
            if (!tag)
                continue;
            switch (tag->kind) {
            case ISZ_FD_LISTEN:
                isz_accept_connection(srv);
                break;
            case ISZ_FD_CLIENT:
                isz_recv_client_messages(srv, tag->opaque);
                break;
            case ISZ_FD_PSI:
                /* §8: drain the kernel PSI record so the trigger
                 * rearms. SPEC §9 has no memory-pressure event, so
                 * we don't emit; the Architect reads /proc/meminfo
                 * or polls the PSI fd themselves if they want detail. */
                isz_psi_dispatch(&srv->psi);
                isz_log_internal(ISZ_LOG_DEBUG,
                                 "psz: pressure trigger fired");
                break;
            case ISZ_FD_BACKEND:
                /* Backend fds (DRM page-flip, etc.): hand off to
                 * the backend's read_events. */
                if (srv->backend) {
                    int rc = isz_backend_read_events(srv->backend);
                    if (rc < 0 && rc != ISZ_ERR_FEATURE_UNAVAIL)
                        isz_log_internal(ISZ_LOG_WARN,
                                         "isz_dispatch: backend read_events rc=%d", rc);
                }
                break;
            case ISZ_FD_SEAT:
                /* DRM backend's libseat session fd: dispatch the
                 * DRM backend's seat, not the input layer's session.
                 * isz_backend_read_events calls isz_drm_read_events
                 * which calls libseat_dispatch on st->seat (the DRM
                 * backend's session). This fires disable_seat/
                 * enable_seat callbacks for VT switching.
                 *
                 * Do NOT call isz_session_dispatch here: that
                 * dispatches a DIFFERENT libseat session (the input
                 * layer's), leaving the DRM session undrained. */
                if (srv->backend) {
                    (void)isz_backend_read_events(srv->backend);
                }
                break;
            }
        }
        if (n < 0) {
            /* EINTR is normal under debugger signals; log others. */
            isz_log_internal(ISZ_LOG_DEBUG,
                             "isz_dispatch: epoll_wait rc=%d", n);
        }
    }

    /* §9: libinput_next_event runs directly in the main dispatch loop.
     * §9 session: libseat_dispatch drains inline. No-ops until the DRM
     * wave wires libinput/libseat fds; the headless backend has no
     * input devices. */
    isz_session_dispatch(srv);
    isz_input_dispatch(srv);

    /* VT switch handling: check the SIGUSR1/SIGUSR2 flags on every
     * dispatch iteration. The signal handler sets g_vt_switch_away;
     * isz_drm_vt_dispatch checks it and calls drmDropMaster +
     * VT_RELDISP(1) to acknowledge the switch. Without this the flag
     * is never checked because epoll_wait returns EINTR (no fd ready)
     * and isz_drm_read_events is never called. */
#ifdef ISHIZUE_HAVE_DRM
    if (srv->backend && srv->backend->type == ISZ_BACKEND_DRM) {
        isz_drm_vt_dispatch(srv->backend);
    }
#endif

    /* W5-B: drain any pending explicit-sync buffer releases. No-op
     * without ISHIZUE_HAVE_DRM (no syncobj is ever attached to a
     * buffer, so the pending list stays empty). */
    isz_buffer_poll_out_fences(srv);
}

/* ------------------------------------------------------------------ */
/* isz_get_fds (SPEC §5)                                              */
/* ------------------------------------------------------------------ */
ISZ_API int isz_get_fds(isz_server *srv, int *fds, size_t max)
{
    if (!srv || !fds)
        return 0;

    size_t n = 0;

    /* Backend fds. Headless has none. The DRM backend's DRM fd lives
     * in srv->epoll_fd with the backend tag; isz_get_fds still returns
     * it for Architects who prefer to poll it in their own epoll set. */
#ifdef ISHIZUE_HAVE_DRM
    if (srv->backend && srv->backend_type == ISZ_BACKEND_DRM) {
        int bcount = isz_drm_get_fds(srv->backend, NULL, 0);
        if (bcount > 0) {
            int bfds[8];
            int got = isz_drm_get_fds(srv->backend, bfds,
                                      (size_t)bcount < 8u ? (size_t)bcount : 8u);
            for (int i = 0; i < got; i++) {
                if (n < max)
                    fds[n] = bfds[i];
                n++;
            }
        }
    }
#endif

    /* libinput fd (§9). */
    if (srv->input_state && srv->input_state->fd >= 0) {
        if (n < max)
            fds[n] = srv->input_state->fd;
        n++;
    }

    /* PSI fd (§8). */
    int psi_fd = isz_psi_get_fd(&srv->psi);
    if (psi_fd >= 0) {
        if (n < max)
            fds[n] = psi_fd;
        n++;
    }

    /* Client socket fds. The client wave adds these to srv->epoll_fd
     * directly; isz_get_fds still returns them for Architects who
     * prefer to poll them in their own epoll set. */
    isz_list_node *pos;
    isz_list_for_each(pos, &srv->clients) {
        struct isz_client *c = container_of(pos, struct isz_client, node);
        if (c->conn && c->conn->fd >= 0) {
            if (n < max)
                fds[n] = c->conn->fd;
            n++;
        }
    }

    return (int)n;
}

/* ------------------------------------------------------------------ */
/* isz_destroy (SPEC §7.6)                                            */
/* ------------------------------------------------------------------ */
ISZ_API void isz_destroy(isz_server *srv)
{
    if (!srv)
        return;

    /* Mark DESTROYING first so the headless output hook and any
     * in-flight listener callbacks can see we're tearing down. */
    srv->state = ISZ_SERVER_DESTROYING;

    /* Close the listen fd (§6.1). epoll_ctl DEL is implicit on close
     * in Linux; we don't bother calling it explicitly. */
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }

    /* Disconnect clients (SPEC §6.12). The full cleanup (surfaces,
     * buffers, plane slots) is the surface wave's job; W3-A closes the
     * conn and emits CLIENT_DISCONNECT. */
    isz_list_node *pos;
    while ((pos = isz_list_pop_front(&srv->clients)) != NULL) {
        struct isz_client *c = container_of(pos, struct isz_client, node);
        if (c->conn) {
            isz_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = ISZ_EVENT_CLIENT_DISCONNECT;
            isz_server_emit_event(srv, &ev);
            isz_conn_close(c->conn);
            c->conn = NULL;
        }
        isz_buffer_cache_destroy(&c->buffer_cache);
        free(c);
    }

    /* Destroy outputs. SPEC §10 says the wrapper stays valid until
     * the Architect calls isz_output_destroy, but isz_destroy tears
     * everything down regardless. */
    while ((pos = isz_list_pop_front(&srv->outputs)) != NULL) {
        struct isz_output *out = container_of(pos, struct isz_output, node);
        isz_output_destroy_internal(out);
        free(out);
    }
    free(srv->output_list_cache);
    srv->output_list_cache = NULL;
    srv->output_list_count = 0;

    /* Tear down the input subsystem (default seat + libinput/libseat). */
    if (srv->input_state) {
        /* Destroy all seats first so they release their devices. */
        struct isz_seat *seat = srv->input_state->seats_head;
        while (seat) {
            struct isz_seat *next = seat->next;
            isz_seat_destroy(seat);
            seat = next;
        }
        isz_input_destroy(srv->input_state);
        srv->input_state = NULL;
    }

    /* Thread pool (W2-C). NULL until W2-C wires it; destroy is a no-op
     * on NULL. */
    isz_thread_pool_destroy(srv->thread_pool);
    srv->thread_pool = NULL;

    /* Backend. The headless backend's destroy fires the output hook
     * with added=false for each output; since we already destroyed
     * the wrappers above, the hook finds nothing to unwrap. */
    if (srv->backend) {
        isz_backend_destroy(srv->backend);
        srv->backend = NULL;
    }

    /* Listener registry. Walk each bucket and free every node. */
    for (size_t i = 0; i < ISZ_LISTENER_BUCKETS; i++) {
        isz_list_node *ln;
        while ((ln = isz_list_pop_front(&srv->listeners[i])) != NULL) {
            struct isz_listener *l =
                container_of(ln, struct isz_listener, node);
            free(l);
        }
    }

    /* Allowlist. */
    while ((pos = isz_list_pop_front(&srv->allowlist_binaries)) != NULL) {
        struct isz_allowlist_binary *b =
            container_of(pos, struct isz_allowlist_binary, node);
        free(b);
    }
    while ((pos = isz_list_pop_front(&srv->allowlist_cgroups)) != NULL) {
        struct isz_allowlist_cgroup *c =
            container_of(pos, struct isz_allowlist_cgroup, node);
        free(c->path);
        free(c);
    }

    isz_psi_destroy(&srv->psi);
    isz_buffer_cache_destroy(&srv->buffer_cache);
    isz_buffer_release_destroy(srv);

    if (srv->epoll_fd >= 0) {
        close(srv->epoll_fd);
        srv->epoll_fd = -1;
    }

    free(srv);
}

/* isz_enable_crash_recovery (SPEC section 12) is implemented in
 * src/isz_crash_recovery.c (W2-D). That file installs the SIGSEGV /
 * SIGABRT / SIGBUS handler that restores the VT, blanks all CRTCs, and
 * re-raises the signal so an Architect-installed crash reporter still
 * runs. The stub that used to live here has been removed to avoid a
 * duplicate symbol.
 *
 * The test hooks (SPEC section 4) are implemented in
 * src/isz_test_hooks.c (W2-D), guarded by ISHIZUE_ENABLE_TEST_HOOKS.
 * The isz_test_simulate_output_hotplug stub that used to live here has
 * been removed for the same reason. */
