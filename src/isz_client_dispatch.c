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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* isz_client_dispatch.c - per-message dispatcher (SPEC §6, §7.3, §7.11, §8).
 *
 * The server's side of the wire protocol. isz_listen.c drains framed
 * messages off a client socket and hands each one here. This file parses
 * the payload, looks up the target object via the per-connection object
 * ID table (SPEC §6.4), calls the matching isz_* API, and queues the
 * reply (or an ISZ_MSG_ERROR) back on the connection's outbound queue.
 *
 * Fault tolerance (SPEC §6.12): lenient. Malformed payloads, bad object
 * IDs, and out-of-range arguments log a warning, queue an ISZ_MSG_ERROR
 * carrying the originating msg_id and an ISZ_ERR_* code, and continue.
 * Only a message arriving before handshake_done is fatal; the
 * dispatcher returns non-zero and isz_listen.c runs the §6.12 cleanup.
 *
 * Wire format (SPEC §6.1): every multi-byte field is little-endian.
 * Replies reuse the request opcode (single shared id space per §6.1).
 * Replies carry only the result fields; the client matches a reply to
 * its request by opcode (v1 protocol is synchronous per object). The
 * ISZ_MSG_ERROR reply carries u32 orig_msg_id + i32 err_code.
 *
 * Not every v1 message is fully wired in this wave. Drag-and-drop
 * (§6.9) needs pointer tracking that does not exist yet, so those
 * messages parse their payloads and reply with ISZ_ERR_FEATURE_UNAVAIL
 * rather than crash. Clipboard (§6.8) gets a minimal store-and-forward
 * path: one server-wide clipboard slot, fd + mime type, handed to the
 * requesting client via SCM_RIGHTS. */

#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include "isz_server_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/isz_log.h"
#include "util/isz_list.h"
#include "backend/isz_backend.h"
#include "buffer/isz_buffer.h"
#include "input/isz_seat_internal.h"
#include "protocol/isz_conn.h"
#include "protocol/isz_protocol.h"
#include "protocol/isz_wire_senders.h"
#include "render/isz_surface_internal.h"

/* ------------------------------------------------------------------ */
/* Reply payload sizes                                                 */
/* ------------------------------------------------------------------ */
#define ISZ_REPLY_SURFACE_CREATE_LEN     4u   /* u32 surface_id */
#define ISZ_REPLY_ATTACH_BUFFER_LEN      8u   /* u32 surface_id + u32 buffer_id */
#define ISZ_REPLY_SUBSURFACE_LEN         8u   /* u32 parent_id + u32 child_id */
#define ISZ_REPLY_POPUP_LEN              8u   /* u32 parent_id + u32 popup_id */
#define ISZ_REPLY_LAYER_LEN              8u   /* u32 output_id + u32 layer_id */
#define ISZ_REPLY_ERROR_LEN              8u   /* u32 orig_msg_id + i32 err_code */
#define ISZ_PRESENTED_LEN                12u  /* u32 surface_id + u64 vblank_ns */
#define ISZ_RELEASE_LEN                  4u   /* u32 buffer_id */
#define ISZ_CAPTURE_DONE_LEN             4u   /* u32 output_id (fd via cmsg) */
#define ISZ_PREFERRED_SCALE_LEN          12u  /* u32 surface_id + u32 num + u32 den */
#define ISZ_TEXT_INPUT_PREEDIT_HDR_LEN   12u  /* u32 id + i32 begin + i32 end */
#define ISZ_TEXT_INPUT_COMMIT_HDR_LEN    4u   /* u32 id */
#define ISZ_IDLE_INHIBIT_LEN             4u   /* u32 output_id */
#define ISZ_SET_SELECTION_OWNER_LEN      20u  /* u32 slot + u64 ts + u32 surf + u32 fd_idx */

/* ------------------------------------------------------------------ */
/* Capture-state accessors (isz_capture.c)                            */
/* ------------------------------------------------------------------ */
/* Forward declarations for the capture path. isz_capture.c owns the
 * capture-state table; this file owns the wire send. */
void isz_capture_set_owning_conn(isz_output *out, struct isz_conn *conn)
    __attribute__((visibility("hidden")));
struct isz_conn *isz_capture_get_owning_conn(isz_output *out)
    __attribute__((visibility("hidden")));

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* Build and queue an ISZ_MSG_ERROR reply. err_code is the public
 * isz_error value (negative). The reply's payload is
 *   u32 orig_msg_id   (the msg_id that triggered the error)
 *   i32 err_code      (ISZ_ERR_*)
 * Logs the error at debug so an Architect running the library at
 * ISZ_LOG_LEVEL=debug can correlate client behavior with replies. */
static void isz_reply_error(struct isz_conn *conn, uint32_t orig_msg_id,
                            int err_code)
{
    uint8_t payload[ISZ_REPLY_ERROR_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, orig_msg_id);
    off = isz_proto_write_i32(payload, off, (int32_t)err_code);
    int rc = isz_conn_send(conn, ISZ_MSG_ERROR, payload, off, NULL, 0);
    if (rc != ISZ_OK) {
        isz_log_internal(ISZ_LOG_DEBUG,
                         "dispatch: failed to queue ERROR reply rc=%d",
                         rc);
    }
}

/* Mark an fd as consumed so isz_listen.c does not close it after the
 * dispatcher returns. The dispatcher hands the fd to APIs that take
 * ownership (isz_surface_attach_buffer, isz_output_capture_start);
 * isz_listen.c closes any leftover fds, so mark consumed ones -1. */
static void isz_consume_fd(const int *fds, size_t n_fds, size_t idx)
{
    if (idx < n_fds) {
        int *mutable_fds = (int *)fds;
        mutable_fds[idx] = -1;
    }
}

/* Resolve an output object id from the conn's table. Falls back to
 * isz_output_list[0] when the id is 0 (placeholder used by clients
 * that have not seen the global event yet). Returns NULL if neither
 * path resolves. */
static isz_output *isz_resolve_output(isz_server *srv, struct isz_conn *conn,
                                      uint32_t output_id)
{
    if (output_id != 0) {
        isz_output *out =
            isz_conn_lookup_object(conn, output_id, ISZ_OBJECT_OUTPUT);
        if (out)
            return out;
    }
    size_t n = 0;
    isz_output **list = isz_output_list(srv, &n);
    if (list && n > 0)
        return list[0];
    return NULL;
}

/* Resolve a seat object id from the conn's table. Falls back to the
 * server's default seat. */
static isz_seat *isz_resolve_seat(isz_server *srv, struct isz_conn *conn,
                                  uint32_t seat_id)
{
    if (seat_id != 0) {
        isz_seat *seat =
            isz_conn_lookup_object(conn, seat_id, ISZ_OBJECT_SEAT);
        if (seat)
            return seat;
    }
    return isz_seat_default(srv);
}

/* Pull the output_id from the head of the payload and resolve it. */
static isz_output *isz_resolve_output_payload(isz_server *srv,
                                              struct isz_conn *conn,
                                              const uint8_t *payload,
                                              size_t payload_len,
                                              size_t *off_out)
{
    uint32_t output_id = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &output_id))
        return NULL;
    *off_out = off;
    return isz_resolve_output(srv, conn, output_id);
}

/* ------------------------------------------------------------------ */
/* Reply builders                                                      */
/* ------------------------------------------------------------------ */

static void isz_reply_surface_create(struct isz_conn *conn,
                                     uint32_t surface_id)
{
    uint8_t payload[ISZ_REPLY_SURFACE_CREATE_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, surface_id);
    (void)isz_conn_send(conn, ISZ_MSG_SURFACE_CREATE, payload, off, NULL, 0);
}

static void isz_reply_attach_buffer(struct isz_conn *conn,
                                    uint32_t surface_id, uint32_t buffer_id)
{
    uint8_t payload[ISZ_REPLY_ATTACH_BUFFER_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, surface_id);
    off = isz_proto_write_u32(payload, off, buffer_id);
    (void)isz_conn_send(conn, ISZ_MSG_SURFACE_ATTACH_BUFFER,
                        payload, off, NULL, 0);
}

static void isz_reply_subsurface(struct isz_conn *conn,
                                 uint32_t parent_id, uint32_t child_id)
{
    uint8_t payload[ISZ_REPLY_SUBSURFACE_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, parent_id);
    off = isz_proto_write_u32(payload, off, child_id);
    (void)isz_conn_send(conn, ISZ_MSG_SURFACE_CREATE_SUBSURFACE,
                        payload, off, NULL, 0);
}

static void isz_reply_popup(struct isz_conn *conn,
                            uint32_t parent_id, uint32_t popup_id)
{
    uint8_t payload[ISZ_REPLY_POPUP_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, parent_id);
    off = isz_proto_write_u32(payload, off, popup_id);
    (void)isz_conn_send(conn, ISZ_MSG_SURFACE_CREATE_POPUP,
                        payload, off, NULL, 0);
}

static void isz_reply_layer(struct isz_conn *conn,
                            uint32_t output_id, uint32_t layer_id)
{
    uint8_t payload[ISZ_REPLY_LAYER_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, output_id);
    off = isz_proto_write_u32(payload, off, layer_id);
    (void)isz_conn_send(conn, ISZ_MSG_SURFACE_CREATE_LAYER,
                        payload, off, NULL, 0);
}

/* SPEC §7.3 presented event: u32 surface_id + u64 vblank_ns. Sent to
 * the surface's owning conn after a successful scanout. Surfaces
 * created directly by the Architect (no owning_conn) get no event. */
void isz_send_presented(struct isz_conn *conn, uint32_t surface_id,
                        uint64_t vblank_ns)
{
    if (!conn)
        return;
    uint8_t payload[ISZ_PRESENTED_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, surface_id);
    off = isz_proto_write_u64(payload, off, vblank_ns);
    (void)isz_conn_send(conn, ISZ_MSG_PRESENTED, payload, off, NULL, 0);
}

/* SPEC §8 release event: u32 buffer_id. Server tells the client a
 * previously-attached buffer is no longer in use. */
void isz_send_release(struct isz_conn *conn, uint32_t buffer_id)
{
    if (!conn)
        return;
    uint8_t payload[ISZ_RELEASE_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, buffer_id);
    (void)isz_conn_send(conn, ISZ_MSG_RELEASE, payload, off, NULL, 0);
}

/* SPEC §7.11 capture_done event: u32 output_id plus a dmabuf_fd passed
 * via SCM_RIGHTS. The server hands the buffer back to the client after
 * capture_stop completes. */
void isz_send_capture_done(struct isz_conn *conn, uint32_t output_id,
                           int dmabuf_fd)
{
    if (!conn)
        return;
    uint8_t payload[ISZ_CAPTURE_DONE_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, output_id);
    int fds[1];
    size_t n_fds = 0;
    if (dmabuf_fd >= 0) {
        fds[0] = dmabuf_fd;
        n_fds = 1;
    }
    (void)isz_conn_send(conn, ISZ_MSG_CAPTURE_DONE,
                        payload, off, n_fds ? fds : NULL, n_fds);
}

/* ------------------------------------------------------------------ */
/* W9-A: S2C senders for the five messages added by W8-B                */
/* (§6.8, §6.15, §6.16, §7.2). Each is a thin encode + isz_conn_send.   */
/* NULL-tolerant on conn so callers can pass surf->owning_conn without */
/* a separate NULL check.                                               */
/* ------------------------------------------------------------------ */

/* SPEC §7.2: preferred fractional scale. The library forwards the
 * (numerator, denominator) the Architect set on the surface to the
 * client that owns it, so the client renders at the right resolution.
 * The library does not composite or rescale. */
void isz_send_surface_preferred_scale(struct isz_conn *conn,
                                      uint32_t surface_id,
                                      uint32_t numerator,
                                      uint32_t denominator)
{
    if (!conn)
        return;
    uint8_t payload[ISZ_PREFERRED_SCALE_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, surface_id);
    off = isz_proto_write_u32(payload, off, numerator);
    off = isz_proto_write_u32(payload, off, denominator);
    (void)isz_conn_send(conn, ISZ_MSG_SURFACE_PREFERRED_SCALE,
                        payload, off, NULL, 0);
}

/* SPEC §6.16: preedit text. The payload carries the text-input id,
 * cursor range, and UTF-8 bytes. text_len is derived from the byte
 * count, not a NUL terminator. NULL text is treated as an empty
 * string so the IME can clear preedit by passing NULL. */
void isz_send_text_input_preedit(struct isz_conn *conn,
                                 uint32_t text_input_id,
                                 const char *text,
                                 int32_t cursor_begin,
                                 int32_t cursor_end)
{
    if (!conn)
        return;
    size_t text_len = text ? strlen(text) : 0;
    if (text_len > ISZ_PROTO_MAX_MESSAGE - ISZ_MSG_HEADER_SIZE -
                   ISZ_TEXT_INPUT_PREEDIT_HDR_LEN) {
        text_len = ISZ_PROTO_MAX_MESSAGE - ISZ_MSG_HEADER_SIZE -
                   ISZ_TEXT_INPUT_PREEDIT_HDR_LEN;
    }
    uint8_t payload[ISZ_PROTO_MAX_MESSAGE - ISZ_MSG_HEADER_SIZE];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, text_input_id);
    off = isz_proto_write_i32(payload, off, cursor_begin);
    off = isz_proto_write_i32(payload, off, cursor_end);
    if (text_len > 0)
        memcpy(payload + off, text, text_len);
    off += text_len;
    (void)isz_conn_send(conn, ISZ_MSG_TEXT_INPUT_PREEDIT,
                        payload, off, NULL, 0);
}

/* SPEC §6.16: committed text. The payload carries the text-input id
 * and UTF-8 bytes. NULL text is treated as an empty commit. */
void isz_send_text_input_commit(struct isz_conn *conn,
                                uint32_t text_input_id,
                                const char *text)
{
    if (!conn)
        return;
    size_t text_len = text ? strlen(text) : 0;
    if (text_len > ISZ_PROTO_MAX_MESSAGE - ISZ_MSG_HEADER_SIZE -
                   ISZ_TEXT_INPUT_COMMIT_HDR_LEN) {
        text_len = ISZ_PROTO_MAX_MESSAGE - ISZ_MSG_HEADER_SIZE -
                   ISZ_TEXT_INPUT_COMMIT_HDR_LEN;
    }
    uint8_t payload[ISZ_PROTO_MAX_MESSAGE - ISZ_MSG_HEADER_SIZE];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, text_input_id);
    if (text_len > 0)
        memcpy(payload + off, text, text_len);
    off += text_len;
    (void)isz_conn_send(conn, ISZ_MSG_TEXT_INPUT_COMMIT,
                        payload, off, NULL, 0);
}

/* SPEC §6.15: idle-inhibit-active. Sent when the count of inhibited
 * surfaces on an output transitions from zero to non-zero. The
 * payload is the output_id. */
void isz_send_idle_inhibit_active(struct isz_conn *conn,
                                  uint32_t output_id)
{
    if (!conn)
        return;
    uint8_t payload[ISZ_IDLE_INHIBIT_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, output_id);
    (void)isz_conn_send(conn, ISZ_MSG_IDLE_INHIBIT_ACTIVE,
                        payload, off, NULL, 0);
}

/* SPEC §6.15: idle-inhibit-inactive. Sent when the count of
 * inhibited surfaces on an output returns to zero. */
void isz_send_idle_inhibit_inactive(struct isz_conn *conn,
                                    uint32_t output_id)
{
    if (!conn)
        return;
    uint8_t payload[ISZ_IDLE_INHIBIT_LEN];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, output_id);
    (void)isz_conn_send(conn, ISZ_MSG_IDLE_INHIBIT_INACTIVE,
                        payload, off, NULL, 0);
}

/* Walk the global surface list and send idle_inhibit_active or
 * _inactive to each unique owning conn that has a surface on the
 * given output. Called from isz_surface_inhibit_adjust (in
 * isz_surface.c) on a 0<->non-zero transition of out's
 * idle_inhibit_count.
 *
 * The dedup cap is small: a typical desktop has 1 to 3 clients with
 * surfaces on a given output. A fixed 32-entry table covers that
 * without an allocation; larger counts silently skip duplicates. */
#define ISZ_INHIBIT_NOTIFY_MAX_CONNS 32
void isz_render_send_idle_inhibit(isz_output *out, bool active)
{
    if (!out)
        return;
    isz_list *surfaces = isz_render_surface_list();
    isz_list_node *pos;
    struct isz_conn *seen[ISZ_INHIBIT_NOTIFY_MAX_CONNS];
    size_t n_seen = 0;
    isz_list_for_each(pos, surfaces) {
        isz_surface *s =
            container_of(pos, isz_surface, server_node);
        if (s->output != out || !s->owning_conn)
            continue;
        bool dup = false;
        for (size_t i = 0; i < n_seen; i++) {
            if (seen[i] == s->owning_conn) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        if (n_seen < ISZ_INHIBIT_NOTIFY_MAX_CONNS)
            seen[n_seen++] = s->owning_conn;
        if (active)
            isz_send_idle_inhibit_active(s->owning_conn, out->id);
        else
            isz_send_idle_inhibit_inactive(s->owning_conn, out->id);
    }
}

/* ------------------------------------------------------------------ */
/* Cross-wave externs (declared in isz_surface_internal.h)             */
/* ------------------------------------------------------------------ */
/* isz_commit.c calls isz_render_send_presented after a successful
 * scanout. The dispatcher routes it to the surface's owning conn. */
int isz_render_send_presented(isz_server *srv, isz_surface *surf,
                              uint64_t vblank_ns)
{
    (void)srv;
    if (!surf || !surf->owning_conn || surf->object_id == 0)
        return ISZ_OK;
    isz_send_presented(surf->owning_conn, surf->object_id, vblank_ns);
    return ISZ_OK;
}

/* isz_capture.c calls isz_render_send_capture_done when capture_stop
 * hands the dmabuf back. The dispatcher looks up the conn that started
 * the capture (tracked in isz_capture.c's per-output state). */
int isz_render_send_capture_done(isz_server *srv, isz_output *out,
                                 int dmabuf_fd, isz_buffer_desc *desc)
{
    (void)srv;
    (void)desc;
    if (!out)
        return ISZ_OK;
    struct isz_conn *conn = isz_capture_get_owning_conn(out);
    if (!conn)
        return ISZ_OK;
    isz_send_capture_done(conn, out->id, dmabuf_fd);
    return ISZ_OK;
}

/* ------------------------------------------------------------------ */
/* Per-message handlers                                                */
/* ------------------------------------------------------------------ */

static void isz_handle_surface_create(isz_server *srv, struct isz_conn *conn)
{
    isz_surface *surf = isz_surface_create(srv);
    if (!surf) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE, ISZ_ERR_NO_MEMORY);
        return;
    }
    uint32_t id = isz_conn_register_object(conn, surf, ISZ_OBJECT_SURFACE);
    if (id == 0) {
        isz_surface_destroy(surf);
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE, ISZ_ERR_NO_MEMORY);
        return;
    }
    surf->owning_conn = conn;
    surf->object_id = id;
    isz_reply_surface_create(conn, id);
}

static void isz_handle_surface_destroy(struct isz_conn *conn,
                                       const uint8_t *payload,
                                       size_t payload_len)
{
    uint32_t surface_id = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &surface_id)) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_DESTROY, ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *surf =
        isz_conn_lookup_object(conn, surface_id, ISZ_OBJECT_SURFACE);
    if (!surf) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_DESTROY, ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_conn_unregister_object(conn, surface_id);
    isz_surface_destroy(surf);
}

static void isz_handle_attach_buffer(struct isz_conn *conn,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     const int *fds, size_t n_fds)
{
    uint32_t surface_id = 0;
    uint32_t fd_index = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &surface_id) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &fd_index)) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_ATTACH_BUFFER,
                        ISZ_ERR_INVALID_ARG);
        return;
    }

    isz_buffer_desc desc;
    uint32_t w, h, stride, offset, format;
    uint64_t modifier;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &w) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &h) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &stride) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &offset) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &format) ||
        !isz_proto_read_u64_checked(payload, &off, payload_len, &modifier)) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_ATTACH_BUFFER,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    if (off >= payload_len) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_ATTACH_BUFFER,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    uint8_t alpha_mode = isz_proto_read_u8(payload, off);
    desc.width = w;
    desc.height = h;
    desc.stride = stride;
    desc.offset = offset;
    desc.format = format;
    desc.modifier = modifier;
    desc.alpha_mode = alpha_mode;

    isz_surface *surf =
        isz_conn_lookup_object(conn, surface_id, ISZ_OBJECT_SURFACE);
    if (!surf) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_ATTACH_BUFFER,
                        ISZ_ERR_INVALID_ARG);
        return;
    }

    if (fd_index >= n_fds || fds[fd_index] < 0) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_ATTACH_BUFFER,
                        ISZ_ERR_INVALID_DMABUF);
        return;
    }

    /* isz_surface_attach_buffer takes ownership of the fd on every path
     * (closes on failure). Mark it consumed so isz_listen.c does not
     * close it after we return. */
    int dmabuf_fd = fds[fd_index];
    isz_consume_fd(fds, n_fds, fd_index);

    int rc = isz_surface_attach_buffer(surf, dmabuf_fd, &desc);
    if (rc != ISZ_OK) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_ATTACH_BUFFER, rc);
        return;
    }

    /* Register the attached buffer on the conn so the client can refer
     * to it by id in BUFFER_DESTROY. The isz_buffer is now stored in
     * surf->current; we register that pointer. */
    uint32_t buffer_id = 0;
    if (surf->current) {
        buffer_id =
            isz_conn_register_object(conn, surf->current,
                                     ISZ_OBJECT_BUFFER);
    }
    if (buffer_id == 0) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_ATTACH_BUFFER,
                        ISZ_ERR_NO_MEMORY);
        return;
    }
    isz_reply_attach_buffer(conn, surface_id, buffer_id);
}

static void isz_handle_damage(struct isz_conn *conn,
                              const uint8_t *payload, size_t payload_len)
{
    uint32_t surface_id = 0;
    uint32_t n_rects = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &surface_id) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &n_rects)) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_DAMAGE, ISZ_ERR_INVALID_ARG);
        return;
    }

    /* Each rect is 4 × i32 = 16 bytes. Reject payloads whose rect
     * section is too short; we don't reject a too-long payload (extra
     * trailing bytes are ignored for forward compatibility). */
    size_t rects_bytes = (size_t)n_rects * 16u;
    if (payload_len - off < rects_bytes) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_DAMAGE, ISZ_ERR_INVALID_ARG);
        return;
    }

    isz_surface *surf =
        isz_conn_lookup_object(conn, surface_id, ISZ_OBJECT_SURFACE);
    if (!surf) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_DAMAGE, ISZ_ERR_INVALID_ARG);
        return;
    }

    if (n_rects == 0) {
        (void)isz_surface_damage(surf, NULL, 0);
        return;
    }

    isz_rect *rects = calloc(n_rects, sizeof(*rects));
    if (!rects) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_DAMAGE, ISZ_ERR_NO_MEMORY);
        return;
    }
    for (uint32_t i = 0; i < n_rects; i++) {
        rects[i].x1 = isz_proto_read_i32(payload, off); off += 4;
        rects[i].y1 = isz_proto_read_i32(payload, off); off += 4;
        rects[i].x2 = isz_proto_read_i32(payload, off); off += 4;
        rects[i].y2 = isz_proto_read_i32(payload, off); off += 4;
    }

    int rc = isz_surface_damage(surf, rects, n_rects);
    free(rects);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_SURFACE_DAMAGE, rc);
}

/* Generic setter trampoline for setters that take (isz_surface *, int). */
typedef int (*isz_surface_setter_fn)(isz_surface *surf, int value);

static void isz_handle_surface_setter(struct isz_conn *conn,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t reply_msg_id,
                                      isz_surface_setter_fn setter)
{
    uint32_t surface_id = 0;
    int32_t value = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &surface_id) ||
        !isz_proto_read_i32_checked(payload, &off, payload_len, &value)) {
        isz_reply_error(conn, reply_msg_id, ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *surf =
        isz_conn_lookup_object(conn, surface_id, ISZ_OBJECT_SURFACE);
    if (!surf) {
        isz_reply_error(conn, reply_msg_id, ISZ_ERR_INVALID_ARG);
        return;
    }
    int rc = setter(surf, value);
    if (rc != ISZ_OK)
        isz_reply_error(conn, reply_msg_id, rc);
}

/* SURFACE_SET_POSITION: payload = u32 surface_id, i32 x, i32 y. */
static void isz_handle_set_position(struct isz_conn *conn,
                                    const uint8_t *payload,
                                    size_t payload_len)
{
    uint32_t surface_id = 0;
    int32_t x = 0, y = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &surface_id) ||
        !isz_proto_read_i32_checked(payload, &off, payload_len, &x) ||
        !isz_proto_read_i32_checked(payload, &off, payload_len, &y)) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_SET_POSITION,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *surf =
        isz_conn_lookup_object(conn, surface_id, ISZ_OBJECT_SURFACE);
    if (!surf) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_SET_POSITION,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    int rc = isz_surface_set_position(surf, x, y);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_SURFACE_SET_POSITION, rc);
}

/* SURFACE_SET_SIZE: payload = u32 surface_id, i32 width, i32 height. */
static void isz_handle_set_size(struct isz_conn *conn,
                                const uint8_t *payload, size_t payload_len)
{
    uint32_t surface_id = 0;
    int32_t w = 0, h = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &surface_id) ||
        !isz_proto_read_i32_checked(payload, &off, payload_len, &w) ||
        !isz_proto_read_i32_checked(payload, &off, payload_len, &h)) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_SET_SIZE, ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *surf =
        isz_conn_lookup_object(conn, surface_id, ISZ_OBJECT_SURFACE);
    if (!surf) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_SET_SIZE, ISZ_ERR_INVALID_ARG);
        return;
    }
    int rc = isz_surface_set_size(surf, w, h);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_SURFACE_SET_SIZE, rc);
}

static void isz_handle_create_subsurface(struct isz_conn *conn,
                                         const uint8_t *payload,
                                         size_t payload_len)
{
    uint32_t parent_id = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &parent_id)) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_SUBSURFACE,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *parent =
        isz_conn_lookup_object(conn, parent_id, ISZ_OBJECT_SURFACE);
    if (!parent) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_SUBSURFACE,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *sub = isz_surface_create_subsurface(parent);
    if (!sub) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_SUBSURFACE,
                        ISZ_ERR_NO_MEMORY);
        return;
    }
    uint32_t id = isz_conn_register_object(conn, sub, ISZ_OBJECT_SURFACE);
    if (id == 0) {
        isz_surface_destroy(sub);
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_SUBSURFACE,
                        ISZ_ERR_NO_MEMORY);
        return;
    }
    sub->owning_conn = conn;
    sub->object_id = id;
    isz_reply_subsurface(conn, parent_id, id);
}

static void isz_handle_create_popup(struct isz_conn *conn,
                                    const uint8_t *payload, size_t payload_len)
{
    uint32_t parent_id = 0;
    int32_t x = 0, y = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &parent_id) ||
        !isz_proto_read_i32_checked(payload, &off, payload_len, &x) ||
        !isz_proto_read_i32_checked(payload, &off, payload_len, &y)) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_POPUP,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *parent =
        isz_conn_lookup_object(conn, parent_id, ISZ_OBJECT_SURFACE);
    if (!parent) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_POPUP,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *pop = isz_surface_create_popup(parent, x, y);
    if (!pop) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_POPUP, ISZ_ERR_NO_MEMORY);
        return;
    }
    uint32_t id = isz_conn_register_object(conn, pop, ISZ_OBJECT_SURFACE);
    if (id == 0) {
        isz_surface_destroy(pop);
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_POPUP, ISZ_ERR_NO_MEMORY);
        return;
    }
    pop->owning_conn = conn;
    pop->object_id = id;
    isz_reply_popup(conn, parent_id, id);
}

static void isz_handle_create_layer(isz_server *srv, struct isz_conn *conn,
                                    const uint8_t *payload,
                                    size_t payload_len)
{
    uint32_t output_id = 0;
    int32_t layer = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &output_id) ||
        !isz_proto_read_i32_checked(payload, &off, payload_len, &layer)) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_LAYER,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_output *out = isz_resolve_output(srv, conn, output_id);
    if (!out) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_LAYER,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    if (layer < (int32_t)ISZ_LAYER_OVERLAY ||
        layer > (int32_t)ISZ_LAYER_LOCK) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_LAYER,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *s = isz_surface_create_layer(out, (enum isz_layer)layer);
    if (!s) {
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_LAYER, ISZ_ERR_NO_MEMORY);
        return;
    }
    uint32_t id = isz_conn_register_object(conn, s, ISZ_OBJECT_SURFACE);
    if (id == 0) {
        isz_surface_destroy(s);
        isz_reply_error(conn, ISZ_MSG_SURFACE_CREATE_LAYER, ISZ_ERR_NO_MEMORY);
        return;
    }
    s->owning_conn = conn;
    s->object_id = id;
    isz_reply_layer(conn, out->id, id);
}

static void isz_handle_buffer_destroy(struct isz_conn *conn,
                                      const uint8_t *payload,
                                      size_t payload_len)
{
    uint32_t buffer_id = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &buffer_id)) {
        isz_reply_error(conn, ISZ_MSG_BUFFER_DESTROY, ISZ_ERR_INVALID_ARG);
        return;
    }
    struct isz_buffer *buf =
        isz_conn_lookup_object(conn, buffer_id, ISZ_OBJECT_BUFFER);
    if (!buf) {
        /* SPEC §8: a release for an already-destroyed buffer is a
         * silent no-op; treat destroy the same way (idempotent). */
        return;
    }
    isz_conn_unregister_object(conn, buffer_id);
    isz_buffer_release(buf);
    isz_buffer_unref(buf);
}

static void isz_handle_commit(isz_server *srv, struct isz_conn *conn,
                              const uint8_t *payload, size_t payload_len)
{
    uint32_t output_id = 0;
    uint32_t flags = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &output_id) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &flags)) {
        isz_reply_error(conn, ISZ_MSG_COMMIT, ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_output *out = isz_resolve_output(srv, conn, output_id);
    if (!out) {
        isz_reply_error(conn, ISZ_MSG_COMMIT, ISZ_ERR_INVALID_ARG);
        return;
    }
    int rc = isz_commit(out, flags);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_COMMIT, rc);
}

static void isz_handle_output_enable(isz_server *srv, struct isz_conn *conn,
                                     const uint8_t *payload,
                                     size_t payload_len)
{
    size_t off = 0;
    isz_output *out = isz_resolve_output_payload(srv, conn,
                                                 payload, payload_len, &off);
    if (!out) {
        isz_reply_error(conn, ISZ_MSG_OUTPUT_ENABLE, ISZ_ERR_INVALID_ARG);
        return;
    }
    uint32_t mode_index = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &mode_index)) {
        isz_reply_error(conn, ISZ_MSG_OUTPUT_ENABLE, ISZ_ERR_INVALID_ARG);
        return;
    }
    size_t n = 0;
    isz_mode **modes = isz_output_get_modes(out, &n);
    if (!modes || mode_index >= n) {
        isz_reply_error(conn, ISZ_MSG_OUTPUT_ENABLE, ISZ_ERR_INVALID_ARG);
        return;
    }
    int rc = isz_output_enable(out, modes[mode_index]);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_OUTPUT_ENABLE, rc);
}

static void isz_handle_output_disable(isz_server *srv, struct isz_conn *conn,
                                      const uint8_t *payload,
                                      size_t payload_len)
{
    size_t off = 0;
    isz_output *out = isz_resolve_output_payload(srv, conn,
                                                 payload, payload_len, &off);
    if (!out) {
        isz_reply_error(conn, ISZ_MSG_OUTPUT_DISABLE, ISZ_ERR_INVALID_ARG);
        return;
    }
    int rc = isz_output_disable(out);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_OUTPUT_DISABLE, rc);
}

static void isz_handle_output_set_dpms(isz_server *srv, struct isz_conn *conn,
                                       const uint8_t *payload,
                                       size_t payload_len)
{
    size_t off = 0;
    isz_output *out = isz_resolve_output_payload(srv, conn,
                                                 payload, payload_len, &off);
    if (!out) {
        isz_reply_error(conn, ISZ_MSG_OUTPUT_SET_DPMS, ISZ_ERR_INVALID_ARG);
        return;
    }
    int32_t state = 0;
    if (!isz_proto_read_i32_checked(payload, &off, payload_len, &state)) {
        isz_reply_error(conn, ISZ_MSG_OUTPUT_SET_DPMS, ISZ_ERR_INVALID_ARG);
        return;
    }
    if (state < (int32_t)ISZ_DPMS_ON || state > (int32_t)ISZ_DPMS_OFF) {
        isz_reply_error(conn, ISZ_MSG_OUTPUT_SET_DPMS, ISZ_ERR_INVALID_ARG);
        return;
    }
    int rc = isz_output_set_dpms(out, (enum isz_dpms_state)state);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_OUTPUT_SET_DPMS, rc);
}

/* Color LUT payloads: u32 size, then size × 3 × u16 (r, g, b channels
 * interleaved per LUT entry). Unpacks into the three separate channel
 * arrays the public API expects. */
static int isz_parse_lut_payload(const uint8_t *payload, size_t payload_len,
                                 size_t *off_in, uint32_t *size_out,
                                 const uint16_t **r_out,
                                 const uint16_t **g_out,
                                 const uint16_t **b_out)
{
    size_t off = *off_in;
    uint32_t size = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &size))
        return ISZ_ERR_INVALID_ARG;
    size_t need = (size_t)size * 3u * 2u;
    if (size == 0 || payload_len - off < need)
        return ISZ_ERR_INVALID_ARG;

    static __thread uint16_t r_buf[4096], g_buf[4096], b_buf[4096];
    if (size > 4096)
        return ISZ_ERR_RESOURCE_LIMIT;
    for (uint32_t i = 0; i < size; i++) {
        const uint8_t *p = payload + off + (size_t)i * 6u;
        r_buf[i] = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
        g_buf[i] = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
        b_buf[i] = (uint16_t)(p[4] | ((uint16_t)p[5] << 8));
    }
    *off_in = off + need;
    *size_out = size;
    *r_out = r_buf;
    *g_out = g_buf;
    *b_out = b_buf;
    return ISZ_OK;
}

static void isz_handle_output_set_lut(isz_server *srv, struct isz_conn *conn,
                                      const uint8_t *payload,
                                      size_t payload_len, uint32_t reply_id,
                                      int (*api)(isz_output *,
                                                 const uint16_t *,
                                                 const uint16_t *,
                                                 const uint16_t *, size_t))
{
    size_t off = 0;
    isz_output *out = isz_resolve_output_payload(srv, conn,
                                                 payload, payload_len, &off);
    if (!out) {
        isz_reply_error(conn, reply_id, ISZ_ERR_INVALID_ARG);
        return;
    }
    uint32_t size = 0;
    const uint16_t *r = NULL, *g = NULL, *b = NULL;
    int rc = isz_parse_lut_payload(payload, payload_len, &off, &size,
                                   &r, &g, &b);
    if (rc != ISZ_OK) {
        isz_reply_error(conn, reply_id, rc);
        return;
    }
    rc = api(out, r, g, b, size);
    if (rc != ISZ_OK)
        isz_reply_error(conn, reply_id, rc);
}

static void isz_handle_output_set_ctm(isz_server *srv, struct isz_conn *conn,
                                      const uint8_t *payload,
                                      size_t payload_len)
{
    size_t off = 0;
    isz_output *out = isz_resolve_output_payload(srv, conn,
                                                 payload, payload_len, &off);
    if (!out) {
        isz_reply_error(conn, ISZ_MSG_OUTPUT_SET_CTM, ISZ_ERR_INVALID_ARG);
        return;
    }
    /* CTM is 9 floats = 36 bytes. */
    if (payload_len - off < 36u) {
        isz_reply_error(conn, ISZ_MSG_OUTPUT_SET_CTM, ISZ_ERR_INVALID_ARG);
        return;
    }
    float matrix[9];
    for (int i = 0; i < 9; i++) {
        uint32_t bits = isz_proto_read_u32(payload, off);
        off += 4;
        float f;
        memcpy(&f, &bits, sizeof(f));
        matrix[i] = f;
    }
    int rc = isz_output_set_ctm(out, matrix);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_OUTPUT_SET_CTM, rc);
}

static void isz_handle_output_set_hdr(isz_server *srv, struct isz_conn *conn,
                                      const uint8_t *payload,
                                      size_t payload_len)
{
    size_t off = 0;
    isz_output *out = isz_resolve_output_payload(srv, conn,
                                                 payload, payload_len, &off);
    if (!out) {
        isz_reply_error(conn, ISZ_MSG_OUTPUT_SET_HDR_METADATA,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    if (payload_len < off) {
        isz_reply_error(conn, ISZ_MSG_OUTPUT_SET_HDR_METADATA,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    size_t n = payload_len - off;
    if (n > 64)
        n = 64;
    isz_hdr_metadata meta;
    meta.size = n;
    memcpy(meta.bytes, payload + off, n);
    int rc = isz_output_set_hdr_metadata(out, &meta);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_OUTPUT_SET_HDR_METADATA, rc);
}

static void isz_handle_seat_set_keyboard_focus(isz_server *srv,
                                               struct isz_conn *conn,
                                               const uint8_t *payload,
                                               size_t payload_len)
{
    uint32_t seat_id = 0, surface_id = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &seat_id) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &surface_id)) {
        isz_reply_error(conn, ISZ_MSG_SEAT_SET_KEYBOARD_FOCUS,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_seat *seat = isz_resolve_seat(srv, conn, seat_id);
    if (!seat) {
        isz_reply_error(conn, ISZ_MSG_SEAT_SET_KEYBOARD_FOCUS,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *surf = NULL;
    if (surface_id != 0) {
        surf = isz_conn_lookup_object(conn, surface_id, ISZ_OBJECT_SURFACE);
        if (!surf) {
            isz_reply_error(conn, ISZ_MSG_SEAT_SET_KEYBOARD_FOCUS,
                            ISZ_ERR_INVALID_ARG);
            return;
        }
    }
    int rc = isz_seat_set_keyboard_focus(seat, surf);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_SEAT_SET_KEYBOARD_FOCUS, rc);
}

static void isz_handle_seat_set_cursor_surface(isz_server *srv,
                                               struct isz_conn *conn,
                                               const uint8_t *payload,
                                               size_t payload_len)
{
    uint32_t seat_id = 0, surface_id = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &seat_id) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &surface_id)) {
        isz_reply_error(conn, ISZ_MSG_SEAT_SET_CURSOR_SURFACE,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_seat *seat = isz_resolve_seat(srv, conn, seat_id);
    if (!seat) {
        isz_reply_error(conn, ISZ_MSG_SEAT_SET_CURSOR_SURFACE,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *surf = NULL;
    if (surface_id != 0) {
        surf = isz_conn_lookup_object(conn, surface_id, ISZ_OBJECT_SURFACE);
        if (!surf) {
            isz_reply_error(conn, ISZ_MSG_SEAT_SET_CURSOR_SURFACE,
                            ISZ_ERR_INVALID_ARG);
            return;
        }
    }
    int rc = isz_seat_set_cursor_surface(seat, surf);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_SEAT_SET_CURSOR_SURFACE, rc);
}

static void isz_handle_capture_start(isz_server *srv, struct isz_conn *conn,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     const int *fds, size_t n_fds)
{
    uint32_t output_id = 0;
    uint32_t fd_index = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &output_id) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &fd_index)) {
        isz_reply_error(conn, ISZ_MSG_CAPTURE_START, ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_buffer_desc desc;
    uint32_t w, h, stride, offset, format;
    uint64_t modifier;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &w) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &h) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &stride) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &offset) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &format) ||
        !isz_proto_read_u64_checked(payload, &off, payload_len, &modifier)) {
        isz_reply_error(conn, ISZ_MSG_CAPTURE_START, ISZ_ERR_INVALID_ARG);
        return;
    }
    if (off >= payload_len) {
        isz_reply_error(conn, ISZ_MSG_CAPTURE_START, ISZ_ERR_INVALID_ARG);
        return;
    }
    uint8_t alpha_mode = isz_proto_read_u8(payload, off);
    desc.width = w; desc.height = h; desc.stride = stride;
    desc.offset = offset; desc.format = format;
    desc.modifier = modifier; desc.alpha_mode = alpha_mode;

    isz_output *out = isz_resolve_output(srv, conn, output_id);
    if (!out) {
        isz_reply_error(conn, ISZ_MSG_CAPTURE_START, ISZ_ERR_INVALID_ARG);
        return;
    }
    if (fd_index >= n_fds || fds[fd_index] < 0) {
        isz_reply_error(conn, ISZ_MSG_CAPTURE_START, ISZ_ERR_INVALID_DMABUF);
        return;
    }
    int dmabuf_fd = fds[fd_index];
    isz_consume_fd(fds, n_fds, fd_index);

    int rc = isz_output_capture_start(out, dmabuf_fd, &desc);
    if (rc != ISZ_OK) {
        isz_reply_error(conn, ISZ_MSG_CAPTURE_START, rc);
        return;
    }

    isz_capture_set_owning_conn(out, conn);
}

static void isz_handle_capture_stop(isz_server *srv, struct isz_conn *conn,
                                    const uint8_t *payload,
                                    size_t payload_len)
{
    uint32_t output_id = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &output_id)) {
        isz_reply_error(conn, ISZ_MSG_CAPTURE_STOP, ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_output *out = isz_resolve_output(srv, conn, output_id);
    if (!out) {
        isz_reply_error(conn, ISZ_MSG_CAPTURE_STOP, ISZ_ERR_INVALID_ARG);
        return;
    }
    int rc = isz_output_capture_stop(out);
    if (rc != ISZ_OK)
        isz_reply_error(conn, ISZ_MSG_CAPTURE_STOP, rc);
    isz_capture_set_owning_conn(out, NULL);
}

static void isz_handle_popup_dismiss(struct isz_conn *conn,
                                     const uint8_t *payload,
                                     size_t payload_len)
{
    uint32_t surface_id = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &surface_id)) {
        isz_reply_error(conn, ISZ_MSG_POPUP_DISMISS, ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_surface *surf =
        isz_conn_lookup_object(conn, surface_id, ISZ_OBJECT_SURFACE);
    if (!surf) {
        isz_reply_error(conn, ISZ_MSG_POPUP_DISMISS, ISZ_ERR_INVALID_ARG);
        return;
    }
    isz_log_internal(ISZ_LOG_DEBUG,
                     "popup_dismiss: surf_id=%u (grab release stub)",
                     (unsigned)surface_id);
}

/* ------------------------------------------------------------------ */
/* Clipboard (SPEC §6.8): minimal store-and-forward                  */
/* ------------------------------------------------------------------ */
struct isz_clipboard_slot {
    int    fd;          /* memfd or dmabuf; -1 when empty */
    char   mime[64];
    size_t mime_len;
};

static struct isz_clipboard_slot s_clipboard = { .fd = -1 };

static void isz_clipboard_slot_clear(struct isz_clipboard_slot *s)
{
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
    s->mime[0] = '\0';
    s->mime_len = 0;
}

static void isz_handle_clipboard_set(struct isz_conn *conn,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     const int *fds, size_t n_fds)
{
    uint32_t fd_index = 0, mime_len = 0;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &fd_index) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len, &mime_len)) {
        isz_reply_error(conn, ISZ_MSG_CLIPBOARD_SET, ISZ_ERR_INVALID_ARG);
        return;
    }
    if (mime_len > sizeof(s_clipboard.mime) - 1 ||
        payload_len - off < mime_len) {
        isz_reply_error(conn, ISZ_MSG_CLIPBOARD_SET, ISZ_ERR_INVALID_ARG);
        return;
    }
    if (fd_index >= n_fds || fds[fd_index] < 0) {
        isz_reply_error(conn, ISZ_MSG_CLIPBOARD_SET, ISZ_ERR_INVALID_ARG);
        return;
    }

    /* dup the fd so the caller's close in isz_listen.c doesn't kill
     * our stored copy. */
    int stored = dup(fds[fd_index]);
    if (stored < 0) {
        isz_reply_error(conn, ISZ_MSG_CLIPBOARD_SET, ISZ_ERR_NO_MEMORY);
        return;
    }
    isz_consume_fd(fds, n_fds, fd_index);

    isz_clipboard_slot_clear(&s_clipboard);
    s_clipboard.fd = stored;
    memcpy(s_clipboard.mime, payload + off, mime_len);
    s_clipboard.mime[mime_len] = '\0';
    s_clipboard.mime_len = mime_len;
}

static void isz_handle_clipboard_request(struct isz_conn *conn)
{
    uint8_t payload[4 + sizeof(s_clipboard.mime)];
    size_t off = 0;
    off = isz_proto_write_u32(payload, off, (uint32_t)s_clipboard.mime_len);
    if (s_clipboard.mime_len > 0) {
        memcpy(payload + off, s_clipboard.mime, s_clipboard.mime_len);
        off += s_clipboard.mime_len;
    }
    int fds[1];
    size_t n_fds = 0;
    if (s_clipboard.fd >= 0) {
        int dup_fd = dup(s_clipboard.fd);
        if (dup_fd < 0) {
            isz_reply_error(conn, ISZ_MSG_CLIPBOARD_REQUEST,
                            ISZ_ERR_NO_MEMORY);
            return;
        }
        fds[0] = dup_fd;
        n_fds = 1;
    }
    int rc = isz_conn_send(conn, ISZ_MSG_CLIPBOARD_REQUEST,
                           payload, off, n_fds ? fds : NULL, n_fds);
    if (rc != ISZ_OK && n_fds > 0)
        close(fds[0]);
}

/* ------------------------------------------------------------------ */
/* §6.8 selections (W9-A): SET_SELECTION_OWNER                         */
/* ------------------------------------------------------------------ */
/* The W8-B CLIPBOARD_SET path above is the older single-clipboard
 * store-and-forward. SET_SELECTION_OWNER is the W8-B replacement that
 * names a (slot, surface) pair, carries a CLOCK_MONOTONIC_RAW
 * timestamp for stale-claim rejection, and optionally passes an fd
 * with the selection contents.
 *
 * The library does not parse the fd per §6.8; it holds it for the
 * next selection request. v1 stores one fd per slot in a process-
 * global; a later wave that adds a selection-request wire message
 * will hand the fd back via SCM_RIGHTS. The slot table is small
 * (PRIMARY + CLIPBOARD), so a fixed array beats a hash. */
#define ISZ_SELECTION_FD_NONE  0xFFFFFFFFu
static int s_selection_fd[2] = { -1, -1 };

static void isz_handle_set_selection_owner(isz_server *srv,
                                           struct isz_conn *conn,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           const int *fds, size_t n_fds)
{
    uint32_t slot = 0;
    uint64_t timestamp_ns = 0;
    uint32_t surface_id = 0;
    uint32_t fd_index = ISZ_SELECTION_FD_NONE;
    size_t off = 0;
    if (!isz_proto_read_u32_checked(payload, &off, payload_len, &slot) ||
        !isz_proto_read_u64_checked(payload, &off, payload_len,
                                    &timestamp_ns) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len,
                                    &surface_id) ||
        !isz_proto_read_u32_checked(payload, &off, payload_len,
                                    &fd_index)) {
        isz_reply_error(conn, ISZ_MSG_SET_SELECTION_OWNER,
                        ISZ_ERR_INVALID_ARG);
        return;
    }
    if (slot > (uint32_t)ISZ_SELECTION_CLIPBOARD) {
        isz_reply_error(conn, ISZ_MSG_SET_SELECTION_OWNER,
                        ISZ_ERR_INVALID_ARG);
        return;
    }

    /* owner_surface_id == 0 is the release form: clear ownership and
     * drop any held fd for the slot. The library treats it the same
     * as isz_seat_set_selection_owner(seat, slot, NULL, 0). */
    isz_surface *owner = NULL;
    if (surface_id != 0) {
        owner = isz_conn_lookup_object(conn, surface_id,
                                       ISZ_OBJECT_SURFACE);
        if (!owner) {
            isz_reply_error(conn, ISZ_MSG_SET_SELECTION_OWNER,
                            ISZ_ERR_INVALID_ARG);
            return;
        }
    }

    isz_seat *seat = isz_seat_default(srv);
    if (!seat) {
        isz_reply_error(conn, ISZ_MSG_SET_SELECTION_OWNER,
                        ISZ_ERR_NO_MEMORY);
        return;
    }
    int rc = isz_seat_set_selection_owner(seat,
                                          (enum isz_selection_slot)slot,
                                          owner, timestamp_ns);
    if (rc != ISZ_OK) {
        isz_reply_error(conn, ISZ_MSG_SET_SELECTION_OWNER, rc);
        return;
    }

    /* Hold the fd (if any) for the next selection request. The
     * library dups the fd so isz_listen.c's close of the original
     * does not invalidate the stored copy. A new fd for the same
     * slot replaces the old one (closed). */
    if (fd_index != ISZ_SELECTION_FD_NONE && fd_index < n_fds &&
        fds[fd_index] >= 0) {
        int stored = dup(fds[fd_index]);
        isz_consume_fd(fds, n_fds, fd_index);
        if (stored < 0) {
            isz_reply_error(conn, ISZ_MSG_SET_SELECTION_OWNER,
                            ISZ_ERR_NO_MEMORY);
            return;
        }
        if (s_selection_fd[slot] >= 0)
            close(s_selection_fd[slot]);
        s_selection_fd[slot] = stored;
    } else if (owner == NULL) {
        /* Release form: drop any held fd. */
        if (s_selection_fd[slot] >= 0) {
            close(s_selection_fd[slot]);
            s_selection_fd[slot] = -1;
        }
    }
    /* Success is silent (no reply) per the W5-A pattern for setters. */
}

/* ------------------------------------------------------------------ */
/* Drag-and-drop (SPEC §6.9): stub                                    */
/* ------------------------------------------------------------------ */
/* Pointer tracking that the drag state machine needs (motion → surface
 * under pointer) does not exist yet. Reply with FEATURE_UNAVAIL so the
 * client framing stays intact. Real wiring lands with the input-
 * routing wave. */
static void isz_handle_drag_stub(struct isz_conn *conn, uint32_t msg_id)
{
    isz_log_internal(ISZ_LOG_DEBUG,
                     "drag msg_id=%u: not yet implemented", (unsigned)msg_id);
    isz_reply_error(conn, msg_id, ISZ_ERR_FEATURE_UNAVAIL);
}

/* ------------------------------------------------------------------ */
/* Dispatcher entry point                                              */
/* ------------------------------------------------------------------ */
int isz_handle_client_message(isz_server *srv, struct isz_conn *conn,
                              uint32_t msg_id,
                              const uint8_t *payload, size_t payload_len,
                              const int *fds, size_t n_fds)
{
    if (!srv || !conn) {
        isz_log_internal(ISZ_LOG_WARN,
                         "dispatch: null srv or conn (msg_id=%u)",
                         (unsigned)msg_id);
        return ISZ_ERR_INVALID_ARG;
    }

    /* SPEC §6.2 final paragraph: any client message before
     * handshake_done (other than the version reply, which the
     * handshake handles itself) is a fatal protocol violation. */
    if (!conn->handshake_done) {
        isz_log_internal(ISZ_LOG_INFO,
                         "dispatch: msg_id=%u before handshake_done, "
                         "disconnecting", (unsigned)msg_id);
        return ISZ_ERR_CLIENT_DISCONNECTED;
    }

    switch (msg_id) {
    case ISZ_MSG_SURFACE_CREATE:
        isz_handle_surface_create(srv, conn);
        break;

    case ISZ_MSG_SURFACE_DESTROY:
        isz_handle_surface_destroy(conn, payload, payload_len);
        break;

    case ISZ_MSG_SURFACE_ATTACH_BUFFER:
        isz_handle_attach_buffer(conn, payload, payload_len, fds, n_fds);
        break;

    case ISZ_MSG_SURFACE_DAMAGE:
        isz_handle_damage(conn, payload, payload_len);
        break;

    case ISZ_MSG_SURFACE_SET_POSITION:
        isz_handle_set_position(conn, payload, payload_len);
        break;

    case ISZ_MSG_SURFACE_SET_SIZE:
        isz_handle_set_size(conn, payload, payload_len);
        break;

    case ISZ_MSG_SURFACE_SET_PLANE_TYPE:
        isz_handle_surface_setter(conn, payload, payload_len,
                                  ISZ_MSG_SURFACE_SET_PLANE_TYPE,
                                  (isz_surface_setter_fn)isz_surface_set_plane_type);
        break;

    case ISZ_MSG_SURFACE_SET_PLANE_SLOT:
        isz_handle_surface_setter(conn, payload, payload_len,
                                  ISZ_MSG_SURFACE_SET_PLANE_SLOT,
                                  (isz_surface_setter_fn)isz_surface_set_plane_slot);
        break;

    case ISZ_MSG_SURFACE_SET_ZPOS:
        isz_handle_surface_setter(conn, payload, payload_len,
                                  ISZ_MSG_SURFACE_SET_ZPOS,
                                  (isz_surface_setter_fn)isz_surface_set_zpos);
        break;

    case ISZ_MSG_SURFACE_SET_TRANSFORM:
        isz_handle_surface_setter(conn, payload, payload_len,
                                  ISZ_MSG_SURFACE_SET_TRANSFORM,
                                  (isz_surface_setter_fn)isz_surface_set_transform);
        break;

    case ISZ_MSG_SURFACE_CREATE_SUBSURFACE:
        isz_handle_create_subsurface(conn, payload, payload_len);
        break;

    case ISZ_MSG_SURFACE_CREATE_POPUP:
        isz_handle_create_popup(conn, payload, payload_len);
        break;

    case ISZ_MSG_SURFACE_CREATE_LAYER:
        isz_handle_create_layer(srv, conn, payload, payload_len);
        break;

    case ISZ_MSG_BUFFER_DESTROY:
        isz_handle_buffer_destroy(conn, payload, payload_len);
        break;

    case ISZ_MSG_COMMIT:
        isz_handle_commit(srv, conn, payload, payload_len);
        break;

    case ISZ_MSG_OUTPUT_ENABLE:
        isz_handle_output_enable(srv, conn, payload, payload_len);
        break;

    case ISZ_MSG_OUTPUT_DISABLE:
        isz_handle_output_disable(srv, conn, payload, payload_len);
        break;

    case ISZ_MSG_OUTPUT_SET_DPMS:
        isz_handle_output_set_dpms(srv, conn, payload, payload_len);
        break;

    case ISZ_MSG_OUTPUT_SET_GAMMA:
        isz_handle_output_set_lut(srv, conn, payload, payload_len,
                                  ISZ_MSG_OUTPUT_SET_GAMMA,
                                  isz_output_set_gamma);
        break;

    case ISZ_MSG_OUTPUT_SET_DEGAMMA:
        isz_handle_output_set_lut(srv, conn, payload, payload_len,
                                  ISZ_MSG_OUTPUT_SET_DEGAMMA,
                                  isz_output_set_degamma);
        break;

    case ISZ_MSG_OUTPUT_SET_CTM:
        isz_handle_output_set_ctm(srv, conn, payload, payload_len);
        break;

    case ISZ_MSG_OUTPUT_SET_HDR_METADATA:
        isz_handle_output_set_hdr(srv, conn, payload, payload_len);
        break;

    case ISZ_MSG_SEAT_SET_KEYBOARD_FOCUS:
        isz_handle_seat_set_keyboard_focus(srv, conn, payload, payload_len);
        break;

    case ISZ_MSG_SEAT_SET_CURSOR_SURFACE:
        isz_handle_seat_set_cursor_surface(srv, conn, payload, payload_len);
        break;

    case ISZ_MSG_CAPTURE_START:
        isz_handle_capture_start(srv, conn, payload, payload_len, fds, n_fds);
        break;

    case ISZ_MSG_CAPTURE_STOP:
        isz_handle_capture_stop(srv, conn, payload, payload_len);
        break;

    case ISZ_MSG_POPUP_DISMISS:
        isz_handle_popup_dismiss(conn, payload, payload_len);
        break;

    case ISZ_MSG_CLIPBOARD_SET:
        isz_handle_clipboard_set(conn, payload, payload_len, fds, n_fds);
        break;

    case ISZ_MSG_CLIPBOARD_REQUEST:
        isz_handle_clipboard_request(conn);
        break;

    case ISZ_MSG_SET_SELECTION_OWNER:
        isz_handle_set_selection_owner(srv, conn, payload, payload_len,
                                       fds, n_fds);
        break;

    case ISZ_MSG_DRAG_START:
    case ISZ_MSG_DRAG_MOTION:
    case ISZ_MSG_DRAG_ACCEPT:
    case ISZ_MSG_DRAG_REJECT:
    case ISZ_MSG_DRAG_DROP:
        isz_handle_drag_stub(conn, msg_id);
        break;

    default:
        /* Unknown or server-only message id (S2C events a client
         * should not be sending back). Lenient per §6.12: log +
         * error reply + continue. */
        isz_log_internal(ISZ_LOG_WARN,
                         "dispatch: unknown msg_id=%u from client "
                         "(payload_len=%zu)",
                         (unsigned)msg_id, payload_len);
        isz_reply_error(conn, msg_id, ISZ_ERR_INVALID_ARG);
        break;
    }

    return ISZ_OK;
}
