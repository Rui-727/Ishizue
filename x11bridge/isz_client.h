/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_client.h: client-side wrapper around the Ishizue wire protocol.
 *
 * The bridge is an ordinary Ishizue client (SPEC §13): a separate
 * process that connects to the Ishizue Unix domain socket, completes
 * the §6.2 handshake from the client side, and exchanges framed
 * messages. It does not call any isz_* server API; it speaks the wire
 * protocol directly.
 *
 * The framing primitives (isz_conn_*, isz_proto_*) are ISZ_INTERNAL
 * (hidden) in libishizue.so, so the bridge cannot link them from the
 * .so. The Makefile compiles ../src/protocol/isz_protocol.c and
 * isz_conn.c directly into the binary, keeping the framing code in
 * sync with the library without exporting internal symbols.
 *
 * Provisional payload layouts:
 *
 *   The wire protocol's per-message payload formats are not yet
 *   formalized in isz_protocol.h (Wave 3's dispatch is a stub). The
 *   layouts below are reasonable choices based on SPEC §6 and §7
 *   descriptions; the bridge will need updating when the per-message
 *   dispatch wave lands and pins the layouts down. All multi-byte
 *   fields are little-endian per SPEC §6.1.
 *
 *   ISZ_MSG_SURFACE_CREATE: u32 surface_id (client-chosen; the server
 *     does not yet reply with an assigned id, so the bridge uses
 *     locally-allocated ids starting at 1).
 *   ISZ_MSG_SURFACE_SET_POSITION: u32 surface_id, i32 x, i32 y.
 *   ISZ_MSG_SURFACE_SET_SIZE:      u32 surface_id, i32 width, i32 height.
 *   ISZ_MSG_SURFACE_SET_OUTPUT:    u32 surface_id, u32 output_id.
 *   ISZ_MSG_COMMIT:                u32 output_id.
 *
 *   The bridge never sends a u32 surface_id of 0; 0 is reserved as
 *   the null surface. The first client-chosen id is 1.
 *
 *   Output ids come from the §6.5 global broadcast during the
 *   handshake. As of Wave 3 the handshake stub does not broadcast any
 *   outputs, so the bridge falls back to output_id 0 (also reserved
 *   as null). SET_OUTPUT/COMMIT messages with output_id 0 are sent
 *   anyway so the wire path is exercised; the server will reject them
 *   until real outputs exist. */

#ifndef ISZ_CLIENT_H
#define ISZ_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "isz_conn.h"

/* ISZ_MSG_SURFACE_SET_OUTPUT is not yet in isz_protocol.h's enum
 * (isz_protocol.h covers SET_POSITION/SET_SIZE/SET_PLANE_TYPE/
 * SET_PLANE_SLOT/SET_ZPOS/SET_TRANSFORM, but the public API's
 * isz_surface_set_output / isz_surface_clear_output were not added
 * to the wire enum in W1-C). The bridge uses a placeholder value
 * picked just past the current enum range (ISZ_MSG_ERROR = 50) until
 * the per-message dispatch wave formalizes it. The server's dispatch
 * stub ignores unknown ids, so this is safe to send.
 *
 * ISZ_MSG_SURFACE_CLEAR_OUTPUT is the inverse: the bridge sends
 * SET_OUTPUT with output_id 0 (None). That matches how the public
 * API's isz_surface_clear_output maps onto the wire (the server
 * tears down the surface->output link). */
#define ISZ_MSG_SURFACE_SET_OUTPUT 51u

struct isz_client {
    struct isz_conn *conn;        /* owns the fd */
    int              fd;          /* alias of conn->fd, for epoll */
    bool             handshake_done;
    uint32_t         next_surface_id;  /* client-side allocation */
    uint32_t         output_id;        /* from §6.5 global, or 0 */
    uint32_t         seat_id;          /* from §6.5 global, or 0 */
};

/* Connect to the Ishizue UDS at `path`. Returns NULL on failure. */
struct isz_client *isz_client_connect(const char *path);

/* Run the client side of the §6.2 handshake. Drains GLOBAL and
 * CAPABILITIES messages, returns after HANDSHAKE_DONE. Returns 0 on
 * success, -1 on failure (caller should isz_client_destroy). */
int  isz_client_handshake(struct isz_client *c);

/* Returns the fd for epoll registration. */
int  isz_client_fd(const struct isz_client *c);

/* Stateful recv of one framed message. Returns total bytes in
 * out_buf on success (>0), 0 if no complete message yet (EAGAIN), or
 * -1 on EOF / hard error (caller should disconnect). out_buf must be
 * ISZ_PROTO_MAX_MESSAGE bytes. Any fds the server passes are closed
 * (the bridge does not yet use them). */
ssize_t isz_client_recv(struct isz_client *c,
                        void *out_buf, size_t buf_len,
                        uint32_t *out_msg_id,
                        size_t *out_payload_len);

/* High-level send helpers. Each encodes the message and sends it.
 * Returns 0 on success, -1 on failure (caller should disconnect). */
int  isz_client_send_surface_create(struct isz_client *c, uint32_t *id_out);
int  isz_client_send_surface_destroy(struct isz_client *c, uint32_t surface_id);
int  isz_client_send_surface_set_position(struct isz_client *c,
                                          uint32_t surface_id,
                                          int32_t x, int32_t y);
int  isz_client_send_surface_set_size(struct isz_client *c,
                                      uint32_t surface_id,
                                      int32_t width, int32_t height);
int  isz_client_send_surface_set_output(struct isz_client *c,
                                        uint32_t surface_id,
                                        uint32_t output_id);
int  isz_client_send_surface_clear_output(struct isz_client *c,
                                          uint32_t surface_id);
int  isz_client_send_surface_set_plane_type(struct isz_client *c,
                                            uint32_t surface_id,
                                            int plane_type);
int  isz_client_send_surface_set_plane_slot(struct isz_client *c,
                                            uint32_t surface_id,
                                            int plane_slot);
int  isz_client_send_surface_set_zpos(struct isz_client *c,
                                      uint32_t surface_id,
                                      int32_t zpos);
int  isz_client_send_seat_set_keyboard_focus(struct isz_client *c,
                                             uint32_t seat_id,
                                             uint32_t surface_id);
int  isz_client_send_commit(struct isz_client *c, uint32_t output_id);
int  isz_client_send_commit_flags(struct isz_client *c, uint32_t output_id,
                                  uint32_t flags);

/* Close the connection and free the wrapper. */
void isz_client_destroy(struct isz_client *c);

#endif /* ISZ_CLIENT_H */
