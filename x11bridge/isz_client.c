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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* isz_client.c: client side of the Ishizue wire protocol.
 *
 * Connects to the server UDS, runs the §6.2 handshake from the client
 * side, and exposes high-level send helpers that emit the surface and
 * commit messages the bridge needs. */

#define _GNU_SOURCE 1  /* need F_GETFL / F_SETFL on Linux */

#include "isz_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <ishizue/isz.h>

static void isz_log(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void isz_log(const char *level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "x11bridge/isz: %s: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

struct isz_client *isz_client_connect(const char *path) {
    if (path == NULL || path[0] == '\0') {
        isz_log("error", "isz_client_connect: empty path");
        return NULL;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        isz_log("error", "socket: %s", strerror(errno));
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        isz_log("error", "socket path too long: %s", path);
        close(fd);
        return NULL;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        isz_log("error", "connect(%s): %s", path, strerror(errno));
        close(fd);
        return NULL;
    }

    struct isz_conn *conn = isz_conn_create(fd);
    if (conn == NULL) {
        /* isz_conn_create closes fd on failure. */
        isz_log("error", "isz_conn_create failed");
        return NULL;
    }

    struct isz_client *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        isz_conn_close(conn);
        isz_log("error", "out of memory");
        return NULL;
    }
    c->conn            = conn;
    c->fd              = fd;
    c->handshake_done  = false;
    c->next_surface_id = 1u;  /* 0 reserved as null */
    c->output_id       = 0u;  /* updated by §6.5 globals if any */
    c->seat_id         = 0u;
    return c;
}

int isz_client_handshake(struct isz_client *c) {
    if (c == NULL || c->conn == NULL) return -1;

    /* The handshake helpers in isz_protocol.c assume blocking I/O.
     * The UDS we just connected to is blocking by default; assert
     * that here in case a future caller flips it before us. */
    int flags = fcntl(c->fd, F_GETFL, 0);
    if (flags < 0 || (flags & O_NONBLOCK) != 0) {
        isz_log("error", "handshake: socket must be blocking");
        return -1;
    }

    /* Step 2 (client side): recv magic + server max version. */
    uint32_t server_version = 0;
    ssize_t s = isz_proto_recv_magic(c->fd, &server_version);
    if (s == -2) {
        isz_log("error", "handshake: bad magic from server");
        return -1;
    }
    if (s < 0) {
        isz_log("error", "handshake: recv magic: %s",
                strerror(errno));
        return -1;
    }

    /* If the server closed before sending magic, it almost certainly
     * means we got denied by the §6.3 allowlist. SPEC §6.3: deny
     * closes the connection before any data is sent. recv_magic
     * returns -1 with errno from recv; the most helpful thing we can
     * do is hint at the cause. */
    if (server_version == 0) {
        isz_log("error", "handshake: server sent version 0 (closed?)");
        return -1;
    }
    isz_log("info", "handshake: server max version=%u",
            (unsigned)server_version);

    /* Step 3 (client side): reply with our chosen version. We
     * negotiate down to the lower of our max and the server's max.
     * Currently we support version 1. */
    uint32_t our_version = ISZ_PROTOCOL_VERSION;
    if (our_version > server_version) {
        our_version = server_version;
    }
    if (isz_proto_send_version_reply(c->fd, our_version) < 0) {
        isz_log("error", "handshake: send version reply: %s",
                strerror(errno));
        return -1;
    }
    c->conn->version = our_version;

    /* Step 4-6: drain GLOBAL and CAPABILITIES messages until
     * HANDSHAKE_DONE arrives. §6.2 says any other message before
     * handshake_done is a fatal protocol violation; we treat it as
     * such here. */
    for (;;) {
        uint8_t buf[ISZ_PROTO_MAX_MESSAGE];
        uint32_t msg_id = 0;
        size_t payload_len = 0;
        int fds[ISZ_PROTO_MAX_FDS];
        size_t n_fds = 0;

        ssize_t n = isz_conn_recv(c->conn, buf, sizeof(buf),
                                  &msg_id, &payload_len, fds, &n_fds);
        if (n < 0) {
            isz_log("error", "handshake: recv: %s", strerror(errno));
            return -1;
        }
        for (size_t i = 0; i < n_fds; i++) {
            close(fds[i]);
        }

        switch (msg_id) {
        case ISZ_MSG_GLOBAL: {
            /* Provisional global layout (isz_handshake.c):
             *   u32 kind   (0=output, 1=seat)
             *   u32 object_id */
            if (payload_len >= 8) {
                const uint8_t *p = (const uint8_t *)buf +
                                   ISZ_MSG_HEADER_SIZE;
                uint32_t kind = (uint32_t)p[0] |
                               ((uint32_t)p[1] << 8)  |
                               ((uint32_t)p[2] << 16) |
                               ((uint32_t)p[3] << 24);
                uint32_t oid  = (uint32_t)p[4] |
                               ((uint32_t)p[5] << 8)  |
                               ((uint32_t)p[6] << 16) |
                               ((uint32_t)p[7] << 24);
                if (kind == 0u) {
                    c->output_id = oid;
                    isz_log("info", "handshake: output id=%u",
                            (unsigned)oid);
                } else if (kind == 1u) {
                    c->seat_id = oid;
                    isz_log("info", "handshake: seat id=%u",
                            (unsigned)oid);
                }
            }
            break;
        }
        case ISZ_MSG_CAPABILITIES:
            isz_log("info", "handshake: capabilities payload=%zu",
                    payload_len);
            break;
        case ISZ_MSG_HANDSHAKE_DONE:
            isz_log("info", "handshake: done (version=%u)",
                    (unsigned)c->conn->version);
            c->handshake_done = true;
            /* Switch to non-blocking for epoll-driven I/O. */
            if (fcntl(c->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                isz_log("warn", "handshake: fcntl NONBLOCK: %s",
                        strerror(errno));
                return -1;
            }
            return 0;
        default:
            isz_log("error",
                    "handshake: unexpected msg_id=%u before done",
                    (unsigned)msg_id);
            return -1;
        }
    }
}

int isz_client_fd(const struct isz_client *c) {
    return c ? c->fd : -1;
}

ssize_t isz_client_recv(struct isz_client *c,
                        void *out_buf, size_t buf_len,
                        uint32_t *out_msg_id,
                        size_t *out_payload_len) {
    if (c == NULL || c->conn == NULL) return -1;
    int fds[ISZ_PROTO_MAX_FDS];
    size_t n_fds = 0;
    ssize_t n = isz_conn_recv(c->conn, out_buf, buf_len,
                              out_msg_id, out_payload_len,
                              fds, &n_fds);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* no complete message yet */
        }
        return -1;  /* EOF or hard error */
    }
    for (size_t i = 0; i < n_fds; i++) {
        close(fds[i]);
    }
    return n;
}

/* Build and send a single message with a little-endian payload. */
static int isz_client_send(struct isz_client *c, uint32_t msg_id,
                           const void *payload, size_t payload_len) {
    int r = isz_conn_send(c->conn, msg_id, payload, payload_len, NULL, 0);
    if (r != ISZ_OK) {
        isz_log("error", "send msg_id=%u failed: %d",
                (unsigned)msg_id, r);
        return -1;
    }
    /* Drain any queued frames. isz_conn_drain returns ISZ_OK or
     * ISZ_ERR_COMMIT_PENDING (queue still has data) or a hard error.
     * Either of the first two is acceptable here. */
    r = isz_conn_drain(c->conn);
    if (r != ISZ_OK && r != ISZ_ERR_COMMIT_PENDING) {
        isz_log("error", "drain msg_id=%u failed: %d",
                (unsigned)msg_id, r);
        return -1;
    }
    return 0;
}

int isz_client_send_surface_create(struct isz_client *c, uint32_t *id_out) {
    if (c == NULL) return -1;
    uint32_t id = c->next_surface_id++;
    uint8_t payload[4];
    payload[0] = (uint8_t)(id & 0xffu);
    payload[1] = (uint8_t)((id >> 8) & 0xffu);
    payload[2] = (uint8_t)((id >> 16) & 0xffu);
    payload[3] = (uint8_t)((id >> 24) & 0xffu);
    if (isz_client_send(c, ISZ_MSG_SURFACE_CREATE, payload,
                        sizeof(payload)) < 0) {
        return -1;
    }
    if (id_out) *id_out = id;
    isz_log("debug", "surface_create id=%u", (unsigned)id);
    return 0;
}

static void isz_put_u32_le_local(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void isz_put_i32_le_local(uint8_t *p, int32_t v) {
    isz_put_u32_le_local(p, (uint32_t)v);
}

int isz_client_send_surface_set_position(struct isz_client *c,
                                         uint32_t surface_id,
                                         int32_t x, int32_t y) {
    if (c == NULL) return -1;
    uint8_t payload[12];
    isz_put_u32_le_local(payload,     surface_id);
    isz_put_i32_le_local(payload + 4, x);
    isz_put_i32_le_local(payload + 8, y);
    isz_log("debug", "surface_set_position id=%u x=%d y=%d",
            (unsigned)surface_id, (int)x, (int)y);
    return isz_client_send(c, ISZ_MSG_SURFACE_SET_POSITION,
                           payload, sizeof(payload));
}

int isz_client_send_surface_set_size(struct isz_client *c,
                                     uint32_t surface_id,
                                     int32_t width, int32_t height) {
    if (c == NULL) return -1;
    uint8_t payload[12];
    isz_put_u32_le_local(payload,     surface_id);
    isz_put_i32_le_local(payload + 4, width);
    isz_put_i32_le_local(payload + 8, height);
    isz_log("debug", "surface_set_size id=%u w=%d h=%d",
            (unsigned)surface_id, (int)width, (int)height);
    return isz_client_send(c, ISZ_MSG_SURFACE_SET_SIZE,
                           payload, sizeof(payload));
}

int isz_client_send_surface_set_output(struct isz_client *c,
                                       uint32_t surface_id,
                                       uint32_t output_id) {
    if (c == NULL) return -1;
    uint8_t payload[8];
    isz_put_u32_le_local(payload,     surface_id);
    isz_put_u32_le_local(payload + 4, output_id);
    isz_log("debug", "surface_set_output id=%u output=%u",
            (unsigned)surface_id, (unsigned)output_id);
    return isz_client_send(c, ISZ_MSG_SURFACE_SET_OUTPUT,
                           payload, sizeof(payload));
}

/* Clear the surface's output by sending SET_OUTPUT with output_id 0.
 * The server interprets output 0 as "no output" and tears down the
 * surface->output link. The placeholder SET_OUTPUT id stays the same. */
int isz_client_send_surface_clear_output(struct isz_client *c,
                                         uint32_t surface_id) {
    return isz_client_send_surface_set_output(c, surface_id, 0u);
}

int isz_client_send_surface_set_plane_type(struct isz_client *c,
                                            uint32_t surface_id,
                                            int plane_type) {
    if (c == NULL) return -1;
    uint8_t payload[8];
    isz_put_u32_le_local(payload,     surface_id);
    isz_put_u32_le_local(payload + 4, (uint32_t)plane_type);
    isz_log("debug", "surface_set_plane_type id=%u type=%d",
            (unsigned)surface_id, plane_type);
    return isz_client_send(c, ISZ_MSG_SURFACE_SET_PLANE_TYPE,
                           payload, sizeof(payload));
}

int isz_client_send_surface_set_plane_slot(struct isz_client *c,
                                            uint32_t surface_id,
                                            int plane_slot) {
    if (c == NULL) return -1;
    uint8_t payload[8];
    isz_put_u32_le_local(payload,     surface_id);
    isz_put_u32_le_local(payload + 4, (uint32_t)plane_slot);
    isz_log("debug", "surface_set_plane_slot id=%u slot=%d",
            (unsigned)surface_id, plane_slot);
    return isz_client_send(c, ISZ_MSG_SURFACE_SET_PLANE_SLOT,
                           payload, sizeof(payload));
}

int isz_client_send_surface_set_zpos(struct isz_client *c,
                                      uint32_t surface_id,
                                      int32_t zpos) {
    if (c == NULL) return -1;
    uint8_t payload[8];
    isz_put_u32_le_local(payload,     surface_id);
    isz_put_i32_le_local(payload + 4, zpos);
    isz_log("debug", "surface_set_zpos id=%u zpos=%d",
            (unsigned)surface_id, (int)zpos);
    return isz_client_send(c, ISZ_MSG_SURFACE_SET_ZPOS,
                           payload, sizeof(payload));
}

int isz_client_send_surface_destroy(struct isz_client *c, uint32_t surface_id) {
    if (c == NULL) return -1;
    uint8_t payload[4];
    isz_put_u32_le_local(payload, surface_id);
    isz_log("debug", "surface_destroy id=%u", (unsigned)surface_id);
    return isz_client_send(c, ISZ_MSG_SURFACE_DESTROY,
                           payload, sizeof(payload));
}

/* Seat-focus wire message: u32 seat_id, u32 surface_id (0 to clear).
 * The server's seat object translates this into an
 * ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED event per SPEC §9. */
int isz_client_send_seat_set_keyboard_focus(struct isz_client *c,
                                             uint32_t seat_id,
                                             uint32_t surface_id) {
    if (c == NULL) return -1;
    uint8_t payload[8];
    isz_put_u32_le_local(payload,     seat_id);
    isz_put_u32_le_local(payload + 4, surface_id);
    isz_log("debug", "seat_set_keyboard_focus seat=%u surface=%u",
            (unsigned)seat_id, (unsigned)surface_id);
    return isz_client_send(c, ISZ_MSG_SEAT_SET_KEYBOARD_FOCUS,
                           payload, sizeof(payload));
}

int isz_client_send_commit(struct isz_client *c, uint32_t output_id) {
    return isz_client_send_commit_flags(c, output_id, 0u);
}

int isz_client_send_commit_flags(struct isz_client *c, uint32_t output_id,
                                  uint32_t flags) {
    if (c == NULL) return -1;
    /* Provisional layout: u32 output_id, u32 flags. The original
     * ISZ_MSG_COMMIT payload was just u32 output_id; W8-A grows it
     * to 8 bytes so the bridge can request ISZ_COMMIT_NORMAL /
     * ISZ_COMMIT_ASYNC / ISZ_COMMIT_TEST_ONLY per SPEC §7.3. The
     * server's dispatch stub reads the first 4 bytes only, so older
     * code keeps working. */
    uint8_t payload[8];
    isz_put_u32_le_local(payload,     output_id);
    isz_put_u32_le_local(payload + 4, flags);
    isz_log("debug", "commit output=%u flags=0x%x",
            (unsigned)output_id, (unsigned)flags);
    return isz_client_send(c, ISZ_MSG_COMMIT, payload, sizeof(payload));
}

void isz_client_destroy(struct isz_client *c) {
    if (c == NULL) return;
    if (c->conn != NULL) {
        isz_conn_close(c->conn);
    }
    free(c);
}
