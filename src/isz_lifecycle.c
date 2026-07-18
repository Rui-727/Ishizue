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
 * via isz_backend_create), create the default seat, set up the epoll
 * set, and register the headless output hook if the backend is
 * headless. Returns NULL on failure with an isz_log_internal error.
 *
 * isz_dispatch: one non-blocking iteration. Drains the epoll set (1
 * iteration, timeout 0), calls the backend's read_events, and drains
 * libinput + libseat session events. Subsystems emit events straight
 * to listeners via isz_server_emit_event; dispatch itself does not
 * walk the listener registry.
 *
 * isz_get_fds: returns the pollable fds the Architect should add to
 * their own epoll set: backend fds (currently none for headless), the
 * libinput fd, the PSI fd, and (once the client wave lands) client
 * sockets. Wave 2-A returns the PSI fd and libinput fd if either is
 * active; everything else is wired as the corresponding waves land.
 *
 * isz_destroy (SPEC §7.6): mark DESTROYING, disconnect clients
 * (isz_conn_close + emit CLIENT_DISCONNECT), destroy outputs, destroy
 * the default seat / input state, destroy the backend, close epoll,
 * free.
 *
 * The cross-wave accessors isz_server_get_input_state /
 * isz_server_set_input_state / isz_server_emit_event /
 * isz_server_get_thread_pool live here too; the W1-E input layer and
 * the W2-C thread-pool shim call them. */
#define _POSIX_C_SOURCE 200809L

#include "isz_server_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "util/isz_log.h"
#include "util/isz_thread.h"
#include "util/isz_thread_pool.h"
#include "backend/isz_backend.h"
#include "backend/isz_headless.h"
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
    srv->next_output_id = 1;
    srv->log_level     = ISZ_LOG_WARN;

    isz_list_init(&srv->outputs);
    isz_list_init(&srv->clients);
    isz_list_init(&srv->allowlist_binaries);
    isz_list_init(&srv->allowlist_cgroups);
    for (size_t i = 0; i < ISZ_LISTENER_BUCKETS; i++)
        isz_list_init(&srv->listeners[i]);

    isz_buffer_cache_init(&srv->buffer_cache);
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

    /* Create the default seat (W1-E). For the DRM backend, libseat +
     * libinput are wired by the DRM wave; for headless, the seat
     * exists but has no devices. */
    if (!isz_seat_default(srv)) {
        isz_log_internal(ISZ_LOG_ERROR, "isz_init: default seat creation failed");
        goto fail_backend;
    }

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

    /* Drain our epoll set, one non-blocking tick. Wave 2-A has nothing
     * in the set (no client sockets yet, headless has no fds); this
     * loop is here for the waves that follow. */
    if (srv->epoll_fd >= 0) {
        struct epoll_event evs[16];
        int n = epoll_wait(srv->epoll_fd, evs, 16, 0);
        if (n < 0) {
            /* EINTR is normal under debugger signals; log others. */
            isz_log_internal(ISZ_LOG_DEBUG,
                             "isz_dispatch: epoll_wait rc=%d", n);
        }
        /* Per-fd dispatch is implemented by the client and surface
         * waves; they register the fd → callback mapping. */
    }

    if (srv->backend) {
        int rc = isz_backend_read_events(srv->backend);
        if (rc < 0 && rc != ISZ_ERR_FEATURE_UNAVAIL)
            isz_log_internal(ISZ_LOG_WARN,
                             "isz_dispatch: backend read_events rc=%d", rc);
    }

    /* §9: libinput_next_event runs directly in the main dispatch loop.
     * §9 session: libseat_dispatch drains inline. */
    isz_session_dispatch(srv);
    isz_input_dispatch(srv);

    /* PSI: if the monitor is armed and readable, consume the event so
     * the trigger rearms. The Architect decides what to do with the
     * pressure signal (SPEC §8: eviction policy is theirs). */
    isz_psi_dispatch(&srv->psi);
}

/* ------------------------------------------------------------------ */
/* isz_get_fds (SPEC §5)                                              */
/* ------------------------------------------------------------------ */
ISZ_API int isz_get_fds(isz_server *srv, int *fds, size_t max)
{
    if (!srv || !fds)
        return 0;

    size_t n = 0;

    /* Backend fds. The DRM backend exposes a DRM fd + vblank fds in a
     * later wave; headless has none. */
    /* TODO: srv->backend->ops->get_fds when the DRM wave lands. */

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

    /* Disconnect clients (SPEC §6.12). The full cleanup (surfaces,
     * buffers, plane slots) is the surface wave's job; Wave 2-A
     * closes the conn and emits CLIENT_DISCONNECT. */
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

    if (srv->epoll_fd >= 0) {
        close(srv->epoll_fd);
        srv->epoll_fd = -1;
    }

    free(srv);
}

/* ------------------------------------------------------------------ */
/* isz_enable_crash_recovery (SPEC §12)                               */
/* ------------------------------------------------------------------ */
/* Opt-in. The full implementation installs a SIGSEGV/SIGABRT handler
 * that restores the VT and blanks all CRTCs before re-raising the
 * signal. That requires the DRM backend (for CRTC blanking) and
 * libseat (for VT restore); neither is built in Wave 2-A. We set the
 * flag and return ISZ_OK so the Architect's call doesn't fail; the
 * signal handler installation lands with the DRM wave. */
ISZ_API int isz_enable_crash_recovery(isz_server *srv)
{
    if (!srv)
        return ISZ_ERR_INVALID_ARG;
    srv->crash_recovery_enabled = true;
    isz_log_internal(ISZ_LOG_INFO,
                     "crash recovery requested (handler install deferred)");
    return ISZ_OK;
}

/* ------------------------------------------------------------------ */
/* Test hooks (SPEC §4, ISHIZUE_ENABLE_TEST_HOOKS only)               */
/* ------------------------------------------------------------------ */
/* isz_test_simulate_output_hotplug drives the headless backend's
 * hotplug path. The backend's helper fires the output hook we
 * registered in isz_init, which wraps the new output into an
 * isz_output and emits ISZ_EVENT_OUTPUT_ADD. */
#ifdef ISHIZUE_ENABLE_TEST_HOOKS
ISZ_API void isz_test_simulate_output_hotplug(isz_server *srv,
                                              uint32_t width, uint32_t height)
{
    if (!srv || !srv->backend)
        return;
    if (srv->backend_type != ISZ_BACKEND_HEADLESS) {
        isz_log_internal(ISZ_LOG_WARN,
                         "test_simulate_output_hotplug: not headless backend");
        return;
    }
    int rc = isz_headless_simulate_output_hotplug(srv->backend, width, height);
    if (rc < 0)
        isz_log_internal(ISZ_LOG_WARN,
                         "test_simulate_output_hotplug: backend rc=%d", rc);
}
#endif
