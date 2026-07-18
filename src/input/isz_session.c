/* SPDX-License-Identifier: MIT
 *
 * Ishizue (礎) - libseat session management. Wave 1-E.
 *
 * §9: the library owns the libseat session and registers its own seat
 * listener. On VT switch away it emits ISZ_EVENT_SESSION_INACTIVE; on
 * switch back, ISZ_EVENT_SESSION_ACTIVE. It does not block commits on
 * session loss; the Architect decides whether to keep rendering (for
 * writeback capture, etc.).
 *
 * libseat dispatches enable_seat / disable_seat inline from
 * libseat_dispatch(), so the events fire on the main thread directly.
 * Without ISHIZUE_HAVE_LIBSEAT the entry points are stubs.
 */
#include "isz_seat_internal.h"

#include <stdlib.h>
#include <string.h>

#ifndef ISHIZUE_HAVE_LIBSEAT

int isz_session_init(isz_server *srv) {
    (void)srv;
    isz_log_internal(ISZ_LOG_DEBUG, "session_init: libseat not built in");
    return ISZ_ERR_FEATURE_UNAVAIL;
}

void isz_session_dispatch(isz_server *srv) {
    (void)srv;
}

#else  /* ISHIZUE_HAVE_LIBSEAT */

static void session_enable_seat(struct libseat *l, void *userdata) {
    (void)l;
    struct isz_input_state *st = userdata;
    st->session_active = true;
    isz_event e = { .type = ISZ_EVENT_SESSION_ACTIVE };
    isz_server_emit_event(st->srv, &e);
}

static void session_disable_seat(struct libseat *l, void *userdata) {
    (void)l;
    struct isz_input_state *st = userdata;
    st->session_active = false;
    isz_event e = { .type = ISZ_EVENT_SESSION_INACTIVE };
    isz_server_emit_event(st->srv, &e);
}

static const struct libseat_seat_listener seat_listener = {
    .enable_seat  = session_enable_seat,
    .disable_seat = session_disable_seat,
};

int isz_session_init(isz_server *srv) {
    if (!srv)
        return ISZ_ERR_INVALID_ARG;

    struct isz_input_state *st = isz_server_get_input_state(srv);
    if (!st) {
        st = calloc(1, sizeof(*st));
        if (!st)
            return ISZ_ERR_NO_MEMORY;
        st->srv = srv;
        st->fd  = -1;
        isz_server_set_input_state(srv, st);
    }
    if (st->session)
        return ISZ_OK;  /* already initialised */

    st->session = libseat_open_seat(&seat_listener, st);
    if (!st->session) {
        isz_log_internal(ISZ_LOG_DEBUG, "libseat_open_seat failed");
        return ISZ_ERR_FEATURE_UNAVAIL;
    }
    st->session_active = true;
    return ISZ_OK;
}

void isz_session_dispatch(isz_server *srv) {
    struct isz_input_state *st = isz_server_get_input_state(srv);
    if (!st || !st->session)
        return;
    /* enable_seat / disable_seat fire inline here, emitting the
     * session events from the main thread. Drain with zero timeout. */
    while (libseat_dispatch(st->session, 0) > 0) {
        /* keep draining */
    }
}

#endif  /* ISHIZUE_HAVE_LIBSEAT */
