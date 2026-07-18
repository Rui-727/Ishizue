/* SPDX-License-Identifier: MIT */
/*
 * isz_conn.h - per-connection state for Ishizue's client protocol.
 *
 * Internal header: a connection wraps a Unix domain socket fd together
 * with the partial-read state (isz_protocol.h), the negotiated wire
 * protocol version, allowlist status, handshake flag, and an outbound
 * event queue used when the kernel send buffer is full (SPEC §6.13).
 *
 * Lifecycle:
 *   - isz_conn_create(fd)  take ownership of fd, set SO_SNDBUF, init state.
 *   - isz_conn_recv()      stateful read of one framed message.
 *   - isz_conn_send()      send one framed message, queueing if needed.
 *   - isz_conn_drain()     flush queued messages when the socket is
 *                           writable (called from the dispatch loop).
 *   - isz_conn_close()     cleanup per SPEC §6.12; surfaces/buffers
 *                           released by the caller; this function frees
 *                           the conn and closes the fd.
 */
#ifndef ISHIZUE_CONN_H
#define ISHIZUE_CONN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "../util/isz_compiler.h"

#include "isz_protocol.h"

/* SPEC §6.13: default SO_SNDBUF for each client socket. */
#define ISZ_DEFAULT_SNDBUF 262144

#ifndef ISZ_MAX_EVENTS_PER_CLIENT
# define ISZ_MAX_EVENTS_PER_CLIENT 1024
#endif

/* SPEC §6.4 object model: object IDs are per-connection, server-assigned,
 * 32-bit. ID 0 is reserved for "no object" (sentinel for failed lookups
 * and unset registrations). The first registered object gets id 1 and
 * IDs increase monotonically for the lifetime of the connection. */
enum isz_conn_object_kind {
    ISZ_OBJECT_NONE    = 0,
    ISZ_OBJECT_SURFACE = 1,
    ISZ_OBJECT_BUFFER  = 2,
    ISZ_OBJECT_OUTPUT  = 3,
    ISZ_OBJECT_SEAT    = 4,
};

struct isz_conn_object_entry {
    uint32_t id;
    void    *handle;
    int      kind;
};

struct isz_msg_node {
    struct isz_msg_node *next;
    size_t               total;     /* bytes in buf (header + payload) */
    size_t               sent;      /* bytes already written (partial) */
    size_t               n_fds;     /* fds carried with this message */
    int                  fds[ISZ_PROTO_MAX_FDS];
    uint8_t              buf[];     /* flexible array: encoded message */
};

struct isz_conn {
    int                              fd;
    struct isz_proto_recv_state      recv_state;
    uint32_t                         version;        /* negotiated */
    bool                             allowlisted;    /* passed §6.3 check */
    bool                             handshake_done; /* §6.2 step 7 */
    bool                             dead;           /* marked for close */

    /* Outbound queue (SPEC §6.13). When the socket's SO_SNDBUF is full,
     * we queue here up to ISZ_MAX_EVENTS_PER_CLIENT events; beyond that,
     * isz_conn_send returns ISZ_ERR_CLIENT_TOO_SLOW and the caller
     * disconnects. */
    struct isz_msg_node             *send_head;
    struct isz_msg_node             *send_tail;
    size_t                           queued_events;

    /* Per-connection object ID table (SPEC §6.4). Grows geometrically.
     * The dispatcher's register/lookup helpers walk this list linearly;
     * v1 client object counts are small enough that a hash table is not
     * worth the complexity. */
    struct isz_conn_object_entry    *objects;
    size_t                           objects_count;
    size_t                           objects_cap;
    uint32_t                         next_object_id;
};

/* Take ownership of fd. Sets SO_SNDBUF to ISZ_DEFAULT_SNDBUF. Returns
 * NULL on failure (fd closed on failure, so caller need not close). */
struct isz_conn *isz_conn_create(int fd)
    ISZ_INTERNAL
    __attribute__((malloc));

/* SPEC §6.12 cleanup: close fd, free queued messages (closing their
 * carried fds), discard any partial-read fds, free the conn. The
 * caller is responsible for releasing surfaces/buffers/plane slots
 * held by the client before calling this; that wiring arrives in a
 * later wave. */
void isz_conn_close(struct isz_conn *conn)
    ISZ_INTERNAL;

/* Stateful read of one framed message. Wraps isz_proto_recv_state_read.
 * On success returns total bytes in out_buf, with *out_msg_id and
 * *out_payload_len set. out_fds (capacity ISZ_PROTO_MAX_FDS) receives
 * any fds. Returns 0 if no complete message yet (caller waits for
 * EPOLLIN). Returns (ssize_t)-1 on error or peer-closed (caller marks
 * conn->dead and calls isz_conn_close). */
ssize_t isz_conn_recv(struct isz_conn *conn,
                      void *out_buf, size_t buf_len,
                      uint32_t *out_msg_id,
                      size_t *out_payload_len,
                      int *out_fds, size_t *out_n_fds)
    ISZ_INTERNAL
    __attribute__((nonnull(1, 2, 4, 5, 6, 7)));

/* Send one framed message. Tries a direct sendmsg first; on EAGAIN,
 * enqueues into the outbound queue. If the queue is full, returns
 * ISZ_ERR_CLIENT_TOO_SLOW (caller marks conn->dead and disconnects).
 * Returns ISZ_OK on success (sent or queued). Returns
 * ISZ_ERR_CLIENT_DISCONNECTED on hard socket error. */
int isz_conn_send(struct isz_conn *conn,
                  uint32_t msg_id,
                  const void *payload, size_t payload_len,
                  const int *fds, size_t n_fds)
    ISZ_INTERNAL
    __attribute__((nonnull(1)));

/* Flush the outbound queue when the socket is writable. Returns ISZ_OK
 * if the queue was drained (or was empty), ISZ_ERR_CLIENT_DISCONNECTED
 * on hard error, ISZ_ERR_COMMIT_PENDING if some messages remain (would
 * block again). */
int isz_conn_drain(struct isz_conn *conn)
    ISZ_INTERNAL
    __attribute__((nonnull(1)));

/* Server-side handshake state machine (isz_handshake.c). */
int isz_handshake_server_side(struct isz_conn *conn)
    ISZ_INTERNAL
    __attribute__((nonnull(1)));

/* ------------------------------------------------------------------ */
/* Per-connection object ID table (SPEC §6.4)                          */
/* ------------------------------------------------------------------ */
/* Register a server-side object (surface, buffer, output, seat proxy)
 * on the connection and return its freshly-allocated 32-bit id. The id
 * is per-connection: the same server-side object may carry different
 * ids on different conns. Returns 0 on allocation failure (callers
 * treat 0 as "no id" since 0 is the sentinel). NULL-tolerant on conn. */
uint32_t isz_conn_register_object(struct isz_conn *conn, void *handle,
                                  int kind)
    ISZ_INTERNAL;

/* Look up an object by id. kind is the expected kind (ISZ_OBJECT_SURFACE,
 * ...); a mismatch returns NULL, as does a missing id. NULL-tolerant
 * on conn. */
void *isz_conn_lookup_object(struct isz_conn *conn, uint32_t id, int kind)
    ISZ_INTERNAL;

/* Drop the registration for id (if present). The server-side object is
 * not freed here; the caller owns its lifecycle. */
void isz_conn_unregister_object(struct isz_conn *conn, uint32_t id)
    ISZ_INTERNAL;

#endif /* ISHIZUE_CONN_H */
