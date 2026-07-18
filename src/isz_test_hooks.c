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

/* isz_test_hooks.c -- SPEC section 4 test hooks.
 *
 * Built only with -DISHIZUE_ENABLE_TEST_HOOKS (the Makefile's test/check
 * targets add it). Without it this file is an empty translation unit so
 * no test-hook symbols leak into a release build.
 *
 * The hooks drive the headless backend for automated integration tests:
 * fake client connections, input injection, and output hotplug, all
 * without real hardware or a wire-protocol peer.
 *
 * isz_test_connect creates a socketpair and feeds one end through
 * isz_conn_create so the server-side connection machinery (W1-C) has a
 * real fd. The other end is kept but unused; test drivers inject events
 * directly via isz_test_send_key / isz_test_send_pointer_motion, which
 * synthesize isz_event structs and push them through the listener
 * registry (isz_server_emit_event, W2-A). No wire bytes flow.
 *
 * The allowlist (SPEC section 6.3) is checked by stating fake_binary_path
 * and comparing (st_dev, st_ino) against the server's binary entries,
 * mirroring isz_allowlist_check but taking a path directly since the
 * test hook has no real peer pid. */

#ifdef ISHIZUE_ENABLE_TEST_HOOKS

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <ishizue/isz.h>

#include "isz_server_internal.h"
#include "util/isz_compiler.h"
#include "input/isz_seat_internal.h"
#include "protocol/isz_conn.h"
#include "backend/isz_headless.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct isz_test_client {
    isz_server      *srv;
    struct isz_conn *conn;
    int              client_fd;         /* other end of the socketpair */
    char            *fake_binary_path;
};

static uint64_t isz_test_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Check fake_binary_path against the server's binary allowlist by
 * (st_dev, st_ino). Returns true if the path resolves to an allowlisted
 * inode. An empty allowlist means deny-all (SPEC section 6.3). */
static bool isz_test_allowlist_matches(isz_server *srv, const char *path)
{
    if (!srv || !path)
        return false;
    if (isz_list_empty(&srv->allowlist_binaries))
        return false;

    struct stat st;
    if (stat(path, &st) != 0)
        return false;

    isz_list_node *pos;
    isz_list_for_each(pos, &srv->allowlist_binaries) {
        struct isz_allowlist_binary *b =
            container_of(pos, struct isz_allowlist_binary, node);
        if (b->st_dev == st.st_dev && b->st_ino == st.st_ino)
            return true;
    }
    return false;
}

ISZ_API isz_test_client *isz_test_connect(isz_server *srv,
                                          const char *fake_binary_path)
{
    if (!srv || !fake_binary_path)
        return NULL;

    if (!isz_test_allowlist_matches(srv, fake_binary_path))
        return NULL;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
        return NULL;

    /* isz_conn_create takes ownership of sv[0] (closes it on failure). */
    struct isz_conn *conn = isz_conn_create(sv[0]);
    if (!conn) {
        close(sv[1]);
        return NULL;
    }

    /* Skip the on-the-wire handshake (SPEC section 6.2): test clients
     * inject events directly and never speak the wire protocol. Mark
     * the conn as allowlisted and handshake-done so any server-side
     * machinery that inspects those flags sees a usable connection. */
    conn->allowlisted    = true;
    conn->handshake_done = true;
    conn->version        = ISZ_PROTOCOL_VERSION;

    isz_test_client *c = calloc(1, sizeof(*c));
    if (!c) {
        isz_conn_close(conn);
        close(sv[1]);
        return NULL;
    }
    c->srv       = srv;
    c->conn      = conn;
    c->client_fd = sv[1];

    c->fake_binary_path = strdup(fake_binary_path);
    if (!c->fake_binary_path) {
        free(c);
        isz_conn_close(conn);
        close(sv[1]);
        return NULL;
    }

    /* SPEC section 6.5: emit CLIENT_CONNECT post-allowlist. */
    isz_event ev = {
        .type    = ISZ_EVENT_CLIENT_CONNECT,
        .time_ns = isz_test_now_ns(),
    };
    isz_server_emit_event(srv, &ev);

    return c;
}

ISZ_API void isz_test_send_key(isz_test_client *client, uint32_t keycode,
                               bool press)
{
    if (!client || !client->srv)
        return;
    isz_event ev = {
        .type    = ISZ_EVENT_INPUT_KEYBOARD_KEY,
        .time_ns = isz_test_now_ns(),
    };
    ev.u.keyboard_key.keycode = keycode;
    ev.u.keyboard_key.pressed = press;
    isz_server_emit_event(client->srv, &ev);
}

ISZ_API void isz_test_send_pointer_motion(isz_test_client *client,
                                          int x, int y)
{
    if (!client || !client->srv)
        return;
    isz_event ev = {
        .type    = ISZ_EVENT_INPUT_POINTER_MOTION,
        .time_ns = isz_test_now_ns(),
    };
    ev.u.pointer_motion.dx      = x;
    ev.u.pointer_motion.dy      = y;
    ev.u.pointer_motion.has_abs = false;
    isz_server_emit_event(client->srv, &ev);
}

ISZ_API void isz_test_simulate_output_hotplug(isz_server *srv,
                                              uint32_t width, uint32_t height)
{
    if (!srv || width == 0 || height == 0)
        return;
    struct isz_backend *b = srv->backend;
    if (!b || b->type != ISZ_BACKEND_HEADLESS)
        return;
    /* The headless backend's hotplug helper (W1-B) fires the output
     * hook; the server (W2-A) wraps that into an isz_output and emits
     * ISZ_EVENT_OUTPUT_ADD. */
    (void)isz_headless_simulate_output_hotplug(b, width, height);
}

#endif /* ISHIZUE_ENABLE_TEST_HOOKS */

/* ISO C forbids an empty translation unit. This dummy is only visible
 * when ISHIZUE_ENABLE_TEST_HOOKS is not defined, so release builds get
 * a valid (but symbol-free) object file. */
#ifndef ISHIZUE_ENABLE_TEST_HOOKS
typedef int isz_test_hooks_empty_translation_unit;
#endif
