/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_surface_internal.h -- concrete surface struct and render-layer
 * internal entry points. SPEC 7 (rendering pipeline), 6.6/6.7 (surface
 * types), 8 (buffer lifecycle).
 *
 * The concrete server/output/mode structs come from isz_server_internal.h
 * (W2-A). This header adds the surface struct, plane-slot reservation
 * management, capture consent, and color helpers.
 *
 * W2-A owns slot creation (isz_output_build_headless_slots populates
 * out->slots). This layer owns slot reservation tracking (which surface
 * holds which slot) since the output's slot table is read-only
 * capability data. */

#ifndef ISZ_SURFACE_INTERNAL_H
#define ISZ_SURFACE_INTERNAL_H

#include "../isz_server_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../util/isz_compiler.h"
#include "../util/isz_list.h"
#include "../buffer/isz_buffer.h"

/* Forward: surfaces point back at the owning connection (the dispatcher
 * sets this when a client creates a surface). isz_conn.h is not included
 * here to keep the include graph shallow; the concrete layout lives in
 * src/protocol/isz_conn.h. */
struct isz_conn;

/* ------------------------------------------------------------------ */
/* Surface kinds (6.6, 6.7).                                          */
/* ------------------------------------------------------------------ */
enum isz_surface_kind {
    ISZ_SURFACE_NORMAL     = 0,
    ISZ_SURFACE_SUBSURFACE = 1,
    ISZ_SURFACE_POPUP      = 2,
    ISZ_SURFACE_LAYER      = 3,
};

/* SPEC 8: up to 2 in-flight buffers per surface. */
#define ISZ_SURFACE_MAX_BUFFERS 2u

/* Per-frame damage cap. 256 is well above any realistic region count. */
#define ISZ_SURFACE_MAX_DAMAGE 256u

struct isz_surface {
    isz_server *srv;
    uint32_t    client_id;   /* 0 for Architect-created surfaces */

    /* Connection ownership (SPEC §6.4). Set by the dispatcher when a
     * client creates the surface; NULL for Architect-created surfaces
     * (isz_surface_create called directly). object_id is the per-
     * connection id assigned by isz_conn_register_object, or 0 if the
     * surface is not registered on any connection. Used by the commit
     * path to send presented events back to the owning client. */
    struct isz_conn *owning_conn;
    uint32_t         object_id;

    enum isz_surface_kind kind;

    /* Buffer slots. current scans out on the next commit; pending_release
     * is the previous buffer whose fence has not signalled yet. SPEC 8. */
    struct isz_buffer *current;
    struct isz_buffer *pending_release;

    /* Damage rects, surface-local, half-open [x1,y1,x2,y2). SPEC 7.9. */
    isz_rect *damage;
    size_t    damage_count;
    size_t    damage_cap;

    /* Geometry. width/height == 0 means derive from the attached buffer. */
    int32_t x, y;
    int32_t width, height;

    /* Plane assignment (7.7). plane_type is mandatory; plane_slot is
     * optional at the API but load-bearing at commit. */
    enum isz_plane_type plane_type;
    bool plane_type_set;
    int  plane_slot;        /* -1 = unset */
    bool plane_slot_set;

    int               zpos;
    enum isz_transform transform;

    /* Output assignment. */
    isz_output *output;

    /* Subsurface / popup / layer state. */
    isz_surface   *parent;
    int32_t        popup_x, popup_y;
    uint32_t       subsurface_flags;  /* ISZ_SUBSURFACE_DESYNC */
    enum isz_layer layer;

    /* List memberships. server_node is in the global surface list.
     * parent_node is in the parent's children list when kind ==
     * SUBSURFACE; self-linked otherwise. */
    isz_list_node server_node;
    isz_list_node parent_node;

    /* Subsurfaces of this surface. */
    isz_list children;
};

/* ------------------------------------------------------------------ */
/* Surface list.                                                      */
/*                                                                    */
/* W2-A's struct isz_server does not yet carry a surfaces field. The  */
/* render layer tracks all surfaces in a module-static list and       */
/* exposes it via isz_render_surface_list() for isz_commit. When the  */
/* lifecycle wave adds a surfaces field to struct isz_server, this    */
/* migrates onto the server struct. Single-threaded (SPEC 5).         */
/* ------------------------------------------------------------------ */
isz_list *isz_render_surface_list(void) ISZ_INTERNAL;

/* ------------------------------------------------------------------ */
/* Plane slot reservations (7.7).                                     */
/*                                                                    */
/* W2-A's out->slots is read-only capability data. The reservation    */
/* registry below tracks which surface holds which slot at commit     */
/* time.                                                              */
/* ------------------------------------------------------------------ */

/* Reserve surf's chosen slot. Verifies the slot exists on out, matches
 * surf->plane_type, supports surf->current's format, and is free (or
 * already held by surf). Returns the slot id or ISZ_ERR_PLANE_UNAVAIL. */
int  isz_plane_slot_assign(isz_output *out, isz_surface *surf) ISZ_INTERNAL;

/* Release a slot held by surf. No-op if surf doesn't hold it. */
void isz_plane_slot_release(isz_output *out, int slot_id,
                            isz_surface *surf) ISZ_INTERNAL;

/* True if surf currently holds slot_id on out. */
bool isz_plane_slot_held_by(isz_output *out, int slot_id,
                            isz_surface *surf) ISZ_INTERNAL;

/* Look up a slot by id on out. Returns NULL if not found. */
const struct isz_output_plane_slot *
isz_plane_slot_get(isz_output *out, int slot_id) ISZ_INTERNAL;

/* ------------------------------------------------------------------ */
/* Capture consent (SPEC 6.11, 7.11).                                 */
/*                                                                    */
/* Per-output, time-limited grant. The Architect calls grant() when   */
/* the user approves; isz_output_capture_start calls check_consent()  */
/* and fails with ISZ_ERR_ACCESS_DENIED if no valid grant exists.     */
/* Implemented in src/isz_capture_consent.c (W2-D).                   */
/* ------------------------------------------------------------------ */
void isz_capture_grant(isz_server *srv, isz_output *output) ISZ_API;
bool isz_capture_check_consent(isz_server *srv, isz_output *output) ISZ_INTERNAL;

/* ------------------------------------------------------------------ */
/* Color management helpers (7.2). Used by the commit path to create  */
/* KMS blobs from the output's stored LUT/CTM. Returns 0 on failure   */
/* or headless (no KMS blobs).                                        */
/* ------------------------------------------------------------------ */
uint32_t isz_color_gamma_blob(isz_output *out, const uint16_t *r,
                              const uint16_t *g, const uint16_t *b,
                              size_t size) ISZ_INTERNAL;
uint32_t isz_color_ctm_blob(isz_output *out, const float matrix[9]) ISZ_INTERNAL;
void     isz_color_destroy_blob(isz_output *out, uint32_t blob_id) ISZ_INTERNAL;

/* ------------------------------------------------------------------ */
/* Client-wave externs.                                               */
/*                                                                    */
/* presented and capture_done are S2C wire messages (ISZ_MSG_PRESENTED, */
/* ISZ_MSG_CAPTURE_DONE), not isz_event. The client dispatch wave      */
/* provides the send path; until then the symbols stay unresolved and  */
/* the build links (shared lib tolerance). isz_commit and isz_capture  */
/* call these after a successful commit / on capture_stop.             */
/* ------------------------------------------------------------------ */
int isz_render_send_presented(isz_server *srv, isz_surface *surf,
                              uint64_t vblank_ns);
int isz_render_send_capture_done(isz_server *srv, isz_output *out,
                                 int dmabuf_fd, isz_buffer_desc *desc);

/* DRM backend wave provides this. Returns -1 for headless. */
int isz_output_get_drm_fd(isz_output *out);

#endif /* ISZ_SURFACE_INTERNAL_H */
