/* SPDX-License-Identifier: MIT */
/*
 * isz_handshake.c - server side of the SPEC §6.2 connection handshake.
 *
 * State machine, run synchronously at connection accept:
 *
 *   1. (Caller) accept + SO_PEERCRED + cgroup allowlist (§6.3). If the
 *      allowlist check fails, the caller closes the connection before
 *      calling us; we never see it.
 *   2. Server sends 8-byte magic header "ISZH" (BE) + 32-bit LE version.
 *   3. Client replies with a 32-bit LE version (must be ≤ server max).
 *      A version greater than the server's max, or zero, closes the
 *      connection.
 *   4. Server broadcasts `global` events for every output and the
 *      active seat (§6.5). Wave 1 stub: the enumeration list is empty
 *      until the output/seat subsystems exist; the call sites are
 *      marked so a later wave can wire them.
 *   5. Server sends a `capabilities` event (§6.2 step 6): a 32-bit
 *      bitmask plus 32-bit max cursor width and height.
 *   6. Server sends `handshake_done`. After this, the client may send
 *      normal requests; any pre-handshake_done message is a fatal
 *      protocol violation (§6.2 final paragraph).
 *
 * The socket is assumed to be in blocking mode for the duration of the
 * handshake. After handshake_done, the caller switches the socket to
 * O_NONBLOCK and feeds it into the dispatch loop's epoll set.
 */
#define _DEFAULT_SOURCE 1
#include "isz_conn.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <ishizue/isz.h>

/* Wave-1 stubs: when the output/seat subsystems exist, these return
 * the server's list of outputs and the active seat. For now they
 * return NULL/0 so the broadcast loop in isz_handshake_server_side is
 * a no-op. The signature is fixed so a later wave can replace the
 * bodies without touching the handshake state machine. */
struct isz_output;  /* forward; matches public opaque type */
struct isz_seat;

static struct isz_output **isz_handshake_list_outputs(size_t *count) {
    *count = 0;
    return NULL;
}

static struct isz_seat *isz_handshake_active_seat(void) {
    return NULL;
}

/* Global event payload (§6.5). Wave-1 layout:
 *   u32 kind       (0 = output, 1 = seat)
 *   u32 object_id  (server-assigned, per-connection)
 *   ...kind-specific fields appended in a later wave (EDID, modes,
 *      seat capabilities).
 *
 * For Wave 1 we only send kind + object_id; clients bind to the id and
 * query further state via per-object requests. */
struct isz_global_payload {
    uint32_t kind;
    uint32_t object_id;
};

enum isz_global_kind {
    ISZ_GLOBAL_OUTPUT = 0,
    ISZ_GLOBAL_SEAT   = 1,
};

/* Capabilities payload (§6.2 step 6). */
struct isz_capabilities_payload {
    uint32_t caps;          /* bitmask of enum isz_capability */
    uint32_t max_cursor_w;  /* max cursor surface width, pixels */
    uint32_t max_cursor_h;  /* max cursor surface height, pixels */
};

/* Encode a u32 LE into a stub payload buffer. The handshake is small
 * and synchronous; we don't pull in the full isz_proto_encode here
 * because the conn's send path already does framing. */
static void isz_put_u32_le_local(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* Send a single handshake-phase message. Wraps isz_conn_send and
 * translates the ISZ_ERR_* returns into a single -1 for the state
 * machine. */
static int isz_handshake_send(struct isz_conn *conn, uint32_t msg_id,
                              const void *payload, size_t payload_len) {
    int r = isz_conn_send(conn, msg_id, payload, payload_len, NULL, 0);
    if (r != ISZ_OK) {
        return -1;
    }
    /* The handshake runs on a blocking socket, so the outbound queue
     * should drain immediately. Pull it through to be sure. */
    r = isz_conn_drain(conn);
    if (r != ISZ_OK && r != ISZ_ERR_COMMIT_PENDING) {
        return -1;
    }
    return 0;
}

/* Broadcast a global event for one output. Wave 1: object_id is
 * assigned by the server; here we use a placeholder id of 0 since the
 * object-id allocator doesn't exist yet. A later wave will pass the
 * real id and append kind-specific fields. */
static int isz_handshake_send_global_output(struct isz_conn *conn,
                                            struct isz_output *out) {
    (void)out;  /* Wave 1: no output fields to read yet. */
    uint8_t payload[sizeof(struct isz_global_payload)];
    isz_put_u32_le_local(payload,     ISZ_GLOBAL_OUTPUT);
    isz_put_u32_le_local(payload + 4, 0u);  /* placeholder object_id */
    return isz_handshake_send(conn, ISZ_MSG_GLOBAL, payload, sizeof(payload));
}

static int isz_handshake_send_global_seat(struct isz_conn *conn,
                                          struct isz_seat *seat) {
    (void)seat;
    uint8_t payload[sizeof(struct isz_global_payload)];
    isz_put_u32_le_local(payload,     ISZ_GLOBAL_SEAT);
    isz_put_u32_le_local(payload + 4, 0u);  /* placeholder object_id */
    return isz_handshake_send(conn, ISZ_MSG_GLOBAL, payload, sizeof(payload));
}

int isz_handshake_server_side(struct isz_conn *conn) {
    if (conn->handshake_done) {
        return ISZ_ERR_INVALID_ARG;
    }

    /* Step 2 (server side): send magic + server max version. */
    ssize_t s = isz_proto_send_magic(conn->fd, ISZ_PROTOCOL_VERSION);
    if (s < 0) {
        conn->dead = true;
        return ISZ_ERR_CLIENT_DISCONNECTED;
    }

    /* Step 3: receive client's chosen version, validate. */
    uint32_t client_version = 0;
    s = isz_proto_recv_version_reply(conn->fd, &client_version);
    if (s < 0) {
        conn->dead = true;
        return ISZ_ERR_CLIENT_DISCONNECTED;
    }
    if (client_version == 0 ||
        client_version > ISZ_PROTOCOL_VERSION) {
        /* SPEC §6.2 step 4: a version greater than the server's max
         * closes the connection. Treat zero as invalid too. */
        conn->dead = true;
        return ISZ_ERR_CLIENT_DISCONNECTED;
    }
    conn->version = client_version;

    /* Step 4: broadcast global events for outputs + active seat.
     * Wave 1: the enumeration returns empty, so this loop is a no-op.
     * A later wave fills in isz_handshake_list_outputs and the
     * per-output send helper. */
    {
        size_t n_outputs = 0;
        struct isz_output **outputs = isz_handshake_list_outputs(&n_outputs);
        for (size_t i = 0; i < n_outputs; i++) {
            if (isz_handshake_send_global_output(conn, outputs[i]) < 0) {
                conn->dead = true;
                return ISZ_ERR_CLIENT_DISCONNECTED;
            }
        }
        /* outputs is NULL in Wave 1; no free needed. A later wave that
         * returns a borrowed array won't free here either. */
    }
    {
        struct isz_seat *seat = isz_handshake_active_seat();
        if (seat != NULL) {
            if (isz_handshake_send_global_seat(conn, seat) < 0) {
                conn->dead = true;
                return ISZ_ERR_CLIENT_DISCONNECTED;
            }
        }
    }

    /* Step 5: send capabilities. Wave 1: empty bitmask, zero max cursor
     * size. A later wave reads these from the backend. */
    {
        uint8_t payload[sizeof(struct isz_capabilities_payload)];
        isz_put_u32_le_local(payload,      0u);  /* caps bitmask */
        isz_put_u32_le_local(payload + 4,  0u);  /* max cursor w */
        isz_put_u32_le_local(payload + 8,  0u);  /* max cursor h */
        if (isz_handshake_send(conn, ISZ_MSG_CAPABILITIES,
                               payload, sizeof(payload)) < 0) {
            conn->dead = true;
            return ISZ_ERR_CLIENT_DISCONNECTED;
        }
    }

    /* Step 6: send handshake_done. After this, the client may send
     * normal requests. */
    if (isz_handshake_send(conn, ISZ_MSG_HANDSHAKE_DONE, NULL, 0) < 0) {
        conn->dead = true;
        return ISZ_ERR_CLIENT_DISCONNECTED;
    }

    conn->handshake_done = true;
    return ISZ_OK;
}
