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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* isz_listen.c - client socket lifecycle (SPEC §6.1, §6.2, §6.3, §6.12).
 *
 * Wires the Architect-supplied listen fd into the server's epoll set and
 * owns accept + handshake + allowlist + the per-client recv path. Real
 * per-message dispatch lives in isz_client_dispatch.c; this file routes
 * every framed message there.
 *
 * Lifecycle:
 *
 *   isz_listen(srv, listen_fd)
 *     Stores the fd on the server, switches it to non-blocking so
 *     isz_accept_connection can drain pending connections without
 *     blocking, and adds it to srv->epoll_fd with EPOLLIN and the
 *     listen tag.
 *
 *   isz_accept_connection(srv)             [called from isz_dispatch]
 *     Loops accept4(SOCK_NONBLOCK | SOCK_CLOEXEC) until EAGAIN. For each
 *     accepted fd: SO_PEERCRED -> isz_allowlist_check (close on deny
 *     before any handshake byte is sent, §6.3), switch to blocking for
 *     the §6.2 handshake (W1-C's helpers assume blocking), switch back
 *     to non-blocking, wrap in an isz_conn + isz_client, add to epoll
 *     with the client tag, emit CLIENT_CONNECT.
 *
 *   isz_recv_client_messages(srv, client)  [called from isz_dispatch]
 *     Loops isz_conn_recv. Routes complete messages to
 *     isz_handle_client_message. On EOF or hard error, runs the §6.12
 *     cleanup: epoll_ctl DEL, isz_conn_close, unlink from the client
 *     list, emit CLIENT_DISCONNECT, free the client.
 *
 * Cross-wave extern resolved here:
 *
 *   isz_backend_blank_all_crtcs
 *     Wave 2-D's crash handler calls this from signal context to black
 *     out every CRTC before re-raising. Dispatches through the backend
 *     ops table; backends with no CRTCs (headless, nested stub) provide
 *     a no-op implementation. */
#define _GNU_SOURCE 1

#include "isz_server_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util/isz_log.h"
#include "backend/isz_backend.h"
#include "buffer/isz_buffer.h"
#include "input/isz_seat_internal.h"
#include "protocol/isz_conn.h"

/* ------------------------------------------------------------------ */
/* isz_listen (SPEC §6.1)                                              */
/* ------------------------------------------------------------------ */
ISZ_API int isz_listen(isz_server *srv, int listen_fd)
{
    if (!srv || listen_fd < 0)
        return ISZ_ERR_INVALID_ARG;
    if (srv->state != ISZ_SERVER_RUNNING)
        return ISZ_ERR_INVALID_ARG;
    if (srv->listen_fd >= 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "isz_listen: listen fd already set (%d)",
                         srv->listen_fd);
        return ISZ_ERR_INVALID_ARG;
    }

    /* Switch the listen fd to non-blocking so isz_accept_connection
     * can drain pending connections in a tight loop without blocking
     * when the queue empties. The Architect may have set this already;
     * fcntl is idempotent. */
    int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags < 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "isz_listen: fcntl F_GETFL failed: %s",
                         strerror(errno));
        return ISZ_ERR_INVALID_ARG;
    }
    if (fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "isz_listen: fcntl F_SETFL O_NONBLOCK failed: %s",
                         strerror(errno));
        return ISZ_ERR_INVALID_ARG;
    }

    srv->listen_fd         = listen_fd;
    srv->listen_tag.kind   = ISZ_FD_LISTEN;
    srv->listen_tag.opaque = NULL;

    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.ptr = &srv->listen_tag;
    if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "isz_listen: epoll_ctl ADD failed: %s",
                         strerror(errno));
        srv->listen_fd = -1;
        return ISZ_ERR_INVALID_ARG;
    }

    isz_log_internal(ISZ_LOG_INFO,
                     "isz_listen: listening on fd=%d", listen_fd);
    return ISZ_OK;
}

/* ------------------------------------------------------------------ */
/* §6.12 cleanup for a single client                                  */
/* ------------------------------------------------------------------ */
/* Removes the client's fd from epoll, closes the conn (which closes
 * the fd and frees the conn struct), unlinks the client from the
 * server's client list, destroys the per-client buffer cache, emits
 * CLIENT_DISCONNECT, and frees the isz_client struct.
 *
 * Wave 3 scope: protocol-layer cleanup only. The surface/buffer/
 * plane-slot release paths are wired by the per-message dispatch wave
 * that follows; for now we just tear down the socket and free the
 * client wrapper. */
static void isz_client_cleanup(isz_server *srv, struct isz_client *c)
{
    if (!srv || !c)
        return;

    if (c->conn && c->conn->fd >= 0)
        epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, c->conn->fd, NULL);

    isz_list_remove(&c->node);

    if (c->conn) {
        isz_conn_close(c->conn);
        c->conn = NULL;
    }

    isz_buffer_cache_destroy(&c->buffer_cache);

    isz_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ISZ_EVENT_CLIENT_DISCONNECT;
    isz_server_emit_event(srv, &ev);

    free(c);
}

/* ------------------------------------------------------------------ */
/* isz_accept_connection (called from isz_dispatch)                   */
/* ------------------------------------------------------------------ */
void isz_accept_connection(isz_server *srv)
{
    if (!srv || srv->listen_fd < 0)
        return;

    for (;;) {
        int fd = accept4(srv->listen_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;  /* drained */
            if (errno == EINTR)
                continue;
            isz_log_internal(ISZ_LOG_WARN,
                             "accept: accept4 failed: %s",
                             strerror(errno));
            break;
        }

        /* §6.3: SO_PEERCRED + allowlist before any handshake byte. */
        struct ucred cred;
        socklen_t cl = sizeof(cred);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) < 0) {
            isz_log_internal(ISZ_LOG_WARN,
                             "accept: SO_PEERCRED failed: %s",
                             strerror(errno));
            close(fd);
            continue;
        }

        if (!isz_allowlist_check(srv, cred.pid)) {
            isz_log_internal(ISZ_LOG_INFO,
                             "accept: peer pid=%d denied by allowlist",
                             (int)cred.pid);
            close(fd);  /* §6.3: close before any data is sent */
            continue;
        }

        /* The §6.2 handshake helpers (W1-C) assume a blocking socket.
         * accept4 gave us a non-blocking fd; switch to blocking for
         * the handshake, then switch back. */
        int blk_flags = fcntl(fd, F_GETFL, 0);
        if (blk_flags < 0 ||
            fcntl(fd, F_SETFL, blk_flags & ~O_NONBLOCK) < 0) {
            isz_log_internal(ISZ_LOG_WARN,
                             "accept: fcntl blocking failed: %s",
                             strerror(errno));
            close(fd);
            continue;
        }

        struct isz_conn *conn = isz_conn_create(fd);
        if (!conn) {
            /* isz_conn_create closes fd on failure. */
            isz_log_internal(ISZ_LOG_WARN,
                             "accept: isz_conn_create failed");
            continue;
        }
        conn->allowlisted = true;

        int rc = isz_handshake_server_side(conn);
        if (rc != ISZ_OK) {
            isz_log_internal(ISZ_LOG_INFO,
                             "accept: handshake failed rc=%d", rc);
            isz_conn_close(conn);
            continue;
        }

        /* Switch back to non-blocking for epoll-driven I/O. */
        if (fcntl(fd, F_SETFL, blk_flags | O_NONBLOCK) < 0) {
            isz_log_internal(ISZ_LOG_WARN,
                             "accept: fcntl non-blocking failed: %s",
                             strerror(errno));
            isz_conn_close(conn);
            continue;
        }

        struct isz_client *c = calloc(1, sizeof(*c));
        if (!c) {
            isz_log_internal(ISZ_LOG_ERROR,
                             "accept: out of memory");
            isz_conn_close(conn);
            continue;
        }
        c->srv        = srv;
        c->conn       = conn;
        c->peer_pid   = cred.pid;
        c->tag.kind   = ISZ_FD_CLIENT;
        c->tag.opaque = c;
        isz_buffer_cache_init(&c->buffer_cache);
        isz_list_push_back(&srv->clients, &c->node);

        struct epoll_event ev;
        ev.events   = EPOLLIN;
        ev.data.ptr = &c->tag;
        if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            isz_log_internal(ISZ_LOG_WARN,
                             "accept: epoll_ctl ADD client failed: %s",
                             strerror(errno));
            isz_list_remove(&c->node);
            isz_buffer_cache_destroy(&c->buffer_cache);
            isz_conn_close(conn);
            free(c);
            continue;
        }

        isz_event ce;
        memset(&ce, 0, sizeof(ce));
        ce.type = ISZ_EVENT_CLIENT_CONNECT;
        isz_server_emit_event(srv, &ce);

        isz_log_internal(ISZ_LOG_DEBUG,
                         "accept: client pid=%d fd=%d handshake done",
                         (int)cred.pid, fd);
    }
}

/* ------------------------------------------------------------------ */
/* isz_recv_client_messages (called from isz_dispatch)                */
/* ------------------------------------------------------------------ */
void isz_recv_client_messages(isz_server *srv, struct isz_client *c)
{
    if (!srv || !c || !c->conn)
        return;

    for (;;) {
        uint8_t buf[ISZ_PROTO_MAX_MESSAGE];
        uint32_t msg_id = 0;
        size_t payload_len = 0;
        int fds[ISZ_PROTO_MAX_FDS];
        size_t n_fds = 0;

        ssize_t n = isz_conn_recv(c->conn, buf, sizeof(buf),
                                  &msg_id, &payload_len, fds, &n_fds);
        if (n == 0) {
            /* No complete message yet; wait for the next EPOLLIN. */
            break;
        }
        if (n < 0) {
            /* EOF or hard error. Run §6.12 cleanup. */
            isz_log_internal(ISZ_LOG_DEBUG,
                             "client pid=%d recv EOF/error, disconnecting",
                             (int)c->peer_pid);
            isz_client_cleanup(srv, c);
            return;
        }

        const uint8_t *payload = (const uint8_t *)buf + ISZ_MSG_HEADER_SIZE;
        int rc = isz_handle_client_message(srv, c->conn, msg_id,
                                           payload, payload_len,
                                           fds, n_fds);
        /* isz_handle_client_message consumed any fds it needed; any
         * leftovers are closed here so we never leak. */
        for (size_t i = 0; i < n_fds; i++) {
            if (fds[i] >= 0)
                close(fds[i]);
        }

        if (rc != ISZ_OK && c->conn != NULL) {
            /* Fatal protocol violation; tear down the connection. */
            isz_log_internal(ISZ_LOG_INFO,
                             "client pid=%d fatal protocol violation, "
                             "disconnecting (rc=%d)",
                             (int)c->peer_pid, rc);
            isz_client_cleanup(srv, c);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Cross-wave extern: §12 crash-handler CRTC blanker                  */
/* ------------------------------------------------------------------ */
/* Dispatches to the backend's blank_all_crtcs op. Backends with no
 * real CRTCs (headless, nested stub) provide a no-op. Async-signal-
 * safe: only the function-pointer call, no logging, no allocation. */
void isz_backend_blank_all_crtcs(struct isz_backend *b)
{
    if (!b || !b->ops || !b->ops->blank_all_crtcs)
        return;
    b->ops->blank_all_crtcs(b);
}
