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
 * W8-A: the bridge now handles eight core opcodes end-to-end
 * (CreateWindow, ChangeWindowAttributes, DestroyWindow, MapWindow,
 * UnmapWindow, ConfigureWindow, GetGeometry, InternAtom,
 * ChangeProperty, GetProperty). Real Ishizue wire messages are sent
 * for surface create/destroy, set_output, set_position, set_size,
 * set_plane_type, set_plane_slot, set_zpos, and commit. StructureNotify
 * events (MapNotify, UnmapNotify, ConfigureNotify, DestroyNotify) and
 * PropertyNotify events are generated when the client selected for
 * them. */

#ifndef X11_CLIENT_H
#define X11_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "x11_proto.h"

#define X11_CLIENT_RECV_BUF   16384
#define X11_CLIENT_MAX_WIN    64   /* windows tracked per client */
#define X11_CLIENT_MAX_PROPS  16   /* properties per window */
#define X11_PROP_MAX_BYTES    4096 /* per-property value cap */

/* Per-window property: (property atom, type atom, format, value bytes).
 * Format is 8/16/32. The value is stored as raw bytes; the caller
 * reads/writes format-sized units. */
struct x11_property {
    bool      in_use;
    uint32_t  property;   /* atom */
    uint32_t  type;       /* atom */
    uint8_t   format;     /* 8, 16, or 32 */
    size_t    value_len;  /* in bytes */
    uint8_t  *value;      /* malloc'd; freed when overwritten or window destroyed */
};

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
    uint32_t  cursor_xid;      /* 0 None means inherit from parent */
    int32_t   zpos;            /* stacking order; bridge-assigned */
    bool      override_redirect;
    bool      mapped;
    bool      has_surface;     /* true once an Ishizue surface has been created */
    bool      has_output;      /* true once set_output has been sent */
    bool      in_use;

    /* Property list. Linear scan; v1 client property counts are small. */
    struct x11_property props[X11_CLIENT_MAX_PROPS];
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
