/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* x11_client.h: per-X11-client state.
 *
 * One of these is created when an X11 client connects to the bridge's
 * listening socket. It owns the per-client socket fd, the client's
 * chosen byte order, the per-client XID range (resource-id-base +
 * resource-id-mask), the sequence counter, a partial-read buffer for
 * incomplete requests, and a small table mapping X11 window ids to
 * bridge-tracked window state.
 *
 * W7-C: the bridge now completes the X11 connection setup handshake
 * for real, parses CreateWindow's value-list, and answers GetGeometry.
 * Other opcodes are accepted and silently dropped (with a debug log)
 * so a client that probes extensions does not stall the connection. */

#ifndef X11_CLIENT_H
#define X11_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "x11_proto.h"

#define X11_CLIENT_RECV_BUF   16384
#define X11_CLIENT_MAX_WIN    64   /* windows tracked per client */

struct x11_window {
    uint32_t  x11_id;          /* X11 window id (client-chosen) */
    uint32_t  isz_surface_id;  /* bridge-allocated Ishizue surface id */
    int32_t   x, y;
    int32_t   w, h;
    uint16_t  border_width;
    uint8_t   depth;
    uint8_t   window_class;    /* 0 CopyFromParent, 1 InputOutput, 2 InputOnly */
    uint32_t  visual_id;
    uint32_t  event_mask;      /* X11 SETofEVENT the client asked for */
    uint32_t  parent_xid;      /* root for top-level, real parent otherwise */
    bool      override_redirect;
    bool      mapped;
    bool      in_use;
};

struct x11_client {
    int      fd;
    bool     setup_done;          /* setup_request received, success sent */
    uint8_t  byte_order;          /* X11_BYTE_ORDER_LSB or _MSB */
    uint16_t sequence;            /* next reply/event sequence number */

    /* Per-client XID range, advertised in SetupSuccess. The client
     * allocates its own XIDs via (base | (counter++ & mask)). The
     * bridge tracks the same (base, mask) so it can sanity-check
     * client-chosen XIDs (currently advisory only). */
    uint32_t xid_base;
    uint32_t xid_mask;
    uint32_t next_xid;            /* bridge-side counter (currently unused) */

    /* Partial-read buffer. Requests come in 4-byte units; we
     * accumulate bytes here until we have at least one full request,
     * then dispatch and shift. */
    uint8_t  buf[X11_CLIENT_RECV_BUF];
    size_t   have;                /* valid bytes in buf */

    /* Window table: X11 window id -> bridge state. Indexed by slot;
     * linear scan on lookup. v1 client window counts are small. */
    struct x11_window windows[X11_CLIENT_MAX_WIN];
};

struct isz_client;  /* forward declaration */

/* Take ownership of fd. Returns NULL on failure (fd closed). */
struct x11_client *x11_client_create(int fd);
void               x11_client_destroy(struct x11_client *c);

int  x11_client_fd(const struct x11_client *c);

/* Drains pending bytes from the socket into c->buf, then parses as
 * many complete requests as possible. Each parsed request is routed
 * to translation.c, which uses `isz` to emit Ishizue wire messages.
 * Returns 0 on success (including EAGAIN), -1 on EOF or hard error
 * (caller should destroy the client). */
int  x11_client_drain(struct x11_client *c, struct isz_client *isz);

/* Send a 32-byte X11 event to this client. Used by the translation
 * layer to forward Ishizue input events. Returns 0 on success, -1 on
 * failure. */
int  x11_client_send_event(struct x11_client *c,
                           const struct x11_event_32 *ev);

/* Look up the first mapped window on this client, or NULL if there
 * is none. Used to route incoming Ishizue input events. */
struct x11_window *x11_client_first_mapped(struct x11_client *c);

/* Bump the sequence counter and return its previous value, in the
 * client's byte order, for use in event/reply headers. */
uint16_t x11_client_next_sequence(struct x11_client *c);

#endif /* X11_CLIENT_H */
