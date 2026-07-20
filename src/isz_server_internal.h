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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* isz_server_internal.h - concrete struct isz_server and cross-wave
 * accessors. SPEC §5, §6.3, §6.5, §7.6, §9, §10, §12. Wave 2-A.
 *
 * The public opaque typedef is in include/ishizue/isz.h; this header
 * defines the real layout so the lifecycle, listener, output, and
 * allowlist .c files can reach into it. Other waves (surface, render,
 * client) include this header for the accessors and the isz_output /
 * isz_mode / isz_hdr_metadata struct layouts they need.
 *
 * Sibling-wave contract: these symbols are defined here:
 *
 *   isz_server_get_input_state / isz_server_set_input_state
 *      Used by the W1-E input layer (isz_seat_api.c, isz_input.c,
 *      isz_session.c) to store the input subsystem state on the server.
 *
 *   isz_server_emit_event / isz_emit_event
 *      Used by every event-producing subsystem (input, output, client)
 *      to dispatch an isz_event to registered listeners on the main
 *      thread. isz_emit_event is a thin alias; both names are kept so
 *      each calling wave can pick whichever reads cleaner at the site.
 *
 *   isz_server_get_thread_pool
 *      Used by the W2-C thread-pool API shim (isz_thread_pool_api.c).
 *      Returns the server's pool handle, which is NULL until W2-C wires
 *      the pool init into isz_init / a separate setup call. With the
 *      pool NULL, isz_thread_pool_submit returns -1 (in the
 *      ENABLE_THREAD_POOL=1 build); the brief defers real wiring.
 */
#ifndef ISZ_SERVER_INTERNAL_H
#define ISZ_SERVER_INTERNAL_H

#include <ishizue/isz.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "util/isz_compiler.h"
#include "util/isz_list.h"
#include "backend/isz_backend.h"
#include "backend/isz_headless.h"
#include "buffer/isz_buffer.h"
#include "buffer/isz_psi.h"
#include "input/isz_seat_internal.h"

/* ------------------------------------------------------------------ */
/* Server lifecycle states                                            */
/* ------------------------------------------------------------------ */
enum isz_server_state {
    ISZ_SERVER_UNINITIALIZED = 0,
    ISZ_SERVER_RUNNING       = 1,
    ISZ_SERVER_DESTROYING    = 2,
};

/* ------------------------------------------------------------------ */
/* Epoll fd tagging (SPEC §5)                                         */
/* ------------------------------------------------------------------ */
/* Every fd the library adds to its epoll set carries one of these
 * tags in epoll_data.ptr. isz_dispatch reads the tag to route the
 * ready event to the right handler without walking a list. */
enum isz_fd_kind {
    ISZ_FD_LISTEN  = 0,  /* the Architect-supplied UDS listen socket */
    ISZ_FD_CLIENT  = 1,  /* a connected client socket */
    ISZ_FD_PSI     = 2,  /* /sys/kernel/mm/pressure/memory trigger fd */
    ISZ_FD_BACKEND = 3,  /* backend-provided fd (DRM page-flip, etc.) */
};

struct isz_fd_tag {
    enum isz_fd_kind kind;
    void            *opaque;  /* struct isz_client * for ISZ_FD_CLIENT;
                               * NULL for the other kinds. */
};

/* ------------------------------------------------------------------ */
/* Listener registry (SPEC §5)                                        */
/* ------------------------------------------------------------------ */
/* One intrusive list per event_type. Listeners fire in registration
 * order. Public isz_event_type values are 1..24; the array is sized
 * up to 32 for headroom without recompilation when new events land. */
#define ISZ_LISTENER_BUCKETS 32

struct isz_listener {
    isz_event_listener_fn fn;
    void                 *userdata;
    isz_list_node         node;
};

/* ------------------------------------------------------------------ */
/* Allowlist (SPEC §6.3)                                              */
/* ------------------------------------------------------------------ */
/* Binary entries are resolved to (st_dev, st_ino) at the call site so
 * the check is immune to path renames. Cgroup entries store the path
 * string verbatim and re-resolve on each check (correctness first; the
 * cache optimization is a later pass). */
struct isz_allowlist_binary {
    dev_t       st_dev;
    ino_t       st_ino;
    isz_list_node node;
};

struct isz_allowlist_cgroup {
    char       *path;
    isz_list_node node;
};

/* ------------------------------------------------------------------ */
/* Client wrapper                                                     */
/* ------------------------------------------------------------------ */
/* Ties an isz_conn (W1-C protocol layer) to the server's client list
 * and carries the per-client buffer cache (W1-D). The detailed client
 * struct (object-id table, surface list, etc.) lands with the protocol
 * dispatch wave; the fields below are what Wave 2-A needs. */
struct isz_client {
    struct isz_server   *srv;
    struct isz_conn     *conn;
    pid_t                peer_pid;
    struct isz_buffer_cache buffer_cache;
    struct isz_fd_tag    tag;       /* epoll routing tag for this client */
    isz_list_node        node;
};

/* ------------------------------------------------------------------ */
/* Outputs (SPEC §7.2, §7.7, §10)                                     */
/* ------------------------------------------------------------------ */
/* isz_mode and isz_hdr_metadata are opaque in the public header; the
 * layouts here are the concrete definitions. */
struct isz_mode {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_mhz;
    bool     preferred;
};

struct isz_hdr_metadata {
    /* Opaque blob storage. Holds either the EDID-parsed
     * HDR_STATIC_METADATA or an Architect-supplied override; the
     * render wave serializes it into KMS's HDR_OUTPUT_METADATA. */
    size_t   size;
    uint8_t  bytes[64];
};

/* Plane slot table for an output (SPEC §7.7). The headless backend
 * synthesizes 3 slots (1 primary + 1 overlay + 1 cursor); DRM-backed
 * outputs read this from KMS plane caps in the render wave. */
struct isz_output_plane_slot {
    int                 id;
    enum isz_plane_type type;
    uint32_t            formats[16];
    size_t              format_count;
    bool                supports_scaling;
    bool                supports_transform;
    int                 zpos_min;
    int                 zpos_max;
};

/* Color-management LUT storage (SPEC §7.2). The render wave creates the
 * KMS blobs from these; Wave 2-A only validates sizes and stores. For
 * the headless backend the "hardware" LUT size is the sentinel below. */
#define ISZ_HEADLESS_LUT_SIZE 256u

struct isz_output_color_lut {
    uint16_t *r;
    uint16_t *g;
    uint16_t *b;
    size_t    size;
    bool      set;
};

struct isz_output {
    struct isz_server   *srv;
    struct isz_backend  *backend;

    uint32_t             id;          /* monotonic within the server */
    char                 name[32];

    /* Headless source identifier. Set when the output wraps a
     * headless_backend output record; 0 otherwise. Used by the
     * output hook to find the wrapper on hotplug removal. */
    uint32_t             headless_id;
    bool                 is_headless;

    /* DRM source identifiers. Set when the output wraps a DRM
     * connector; 0 / false otherwise. drm_crtc_mask is the bit index
     * the CRTC occupies in plane possible_crtcs masks (SPEC 10). */
    uint32_t             drm_connector_id;
    uint32_t             drm_crtc_id;
    uint32_t             drm_crtc_mask;
    bool                 is_drm;
    bool                 hdr_capable;
    bool                 vrr_capable;
    bool                 vrr_enabled;
    char                 gpu_node_path[64];  /* render node for scanout */

    /* Modes (EDID-derived for DRM; synthetic for headless). Stored as
     * an array of pointers so isz_output_get_modes can hand back the
     * same pointer array the caller cache expects. */
    struct isz_mode    **modes;
    size_t               mode_count;
    struct isz_mode     *current_mode;

    /* EDID blob (SPEC §7.2: raw opaque bytes; library validates
     * checksum and parses only HDR_STATIC_METADATA). NULL/0 for
     * headless. */
    uint8_t             *edid;
    size_t               edid_size;

    /* DPMS + enable state (SPEC §10). */
    enum isz_dpms_state  dpms;
    bool                 enabled;

    /* SPEC §10 / §7.10: set true when the backend fires the output-
     * remove hook. isz_commit returns ISZ_ERR_OUTPUT_DISCONNECTED
     * while this is set; the wrapper stays allocated until the
     * Architect calls isz_output_destroy. */
    bool                 disconnected;

    /* Plane slots (SPEC §7.7). */
    struct isz_output_plane_slot *slots;
    size_t                        slot_count;

    /* Color management (SPEC §7.2). */
    struct isz_output_color_lut gamma;
    struct isz_output_color_lut degamma;
    float                       ctm[9];
    bool                        ctm_set;
    struct isz_hdr_metadata     hdr;
    bool                        hdr_set;

    /* Cached pointer array for isz_output_get_modes (caller-must-not-
     * free, valid until the mode list changes). */
    isz_mode           **mode_ptr_cache;
    size_t               mode_ptr_count;

    /* §6.15 idle inhibit: count of surfaces on this output with the
     * idle-inhibit flag set. The library emits
     * ISZ_EVENT_IDLE_INHIBIT_ACTIVE on the 0->1 transition and
     * ISZ_EVENT_IDLE_INHIBIT_INACTIVE on the 1->0 transition. */
    int                  idle_inhibit_count;

    isz_list_node        node;  /* server's outputs list */
};

/* ------------------------------------------------------------------ */
/* Concrete server (public opaque typedef in isz.h)                   */
/* ------------------------------------------------------------------ */
struct isz_server {
    enum isz_server_state  state;

    /* Backend (SPEC §10). */
    struct isz_backend    *backend;
    enum isz_backend_type  backend_type;

    /* Outputs and clients (SPEC §6.5, §6.12). */
    isz_list               outputs;
    isz_list               clients;
    uint32_t               next_output_id;

    /* Borrowed array refreshed on each isz_output_list call. Caller
     * must not free; valid until the next dispatch (SPEC §7.6). */
    isz_output           **output_list_cache;
    size_t                 output_list_count;

    /* Input subsystem state (W1-E handoff). */
    struct isz_input_state *input_state;

    /* Listener registry (SPEC §5). One list per event_type. */
    isz_list               listeners[ISZ_LISTENER_BUCKETS];

    /* Allowlist (SPEC §6.3). */
    isz_list               allowlist_binaries;
    isz_list               allowlist_cgroups;

    /* Thread pool (SPEC §5). Forward-declared; wired by W2-C. NULL
     * until then, which makes isz_thread_pool_submit return -1. */
    struct isz_thread_pool *thread_pool;

    /* Buffer cache (SPEC §8). Per-client caches live on isz_client;
     * this is the server-level fallback used when no client context
     * is available (e.g. headless backend commits without a client). */
    struct isz_buffer_cache buffer_cache;

    /* W5-B: pending-release list (SPEC 7.5, 8). Buffers whose page-flip
     * has fired but whose out_syncobj has not yet signalled. Drained by
     * isz_buffer_poll_out_fences on each isz_dispatch iteration. Empty
     * without ISHIZUE_HAVE_DRM (no syncobj is ever attached to a
     * buffer, so the page-flip path releases buffers immediately). */
    isz_list               pending_releases;

    /* PSI monitor (SPEC §8). */
    struct isz_psi_monitor  psi;

    /* Epoll set (SPEC §5). Library-owned; holds backend fds and
     * client sockets. Wave 2-A creates the fd; the surface/client
     * waves add fds to it. */
    int                     epoll_fd;

    /* §6.1: the Architect hands a bound+listening UDS fd to the
     * library via isz_listen. -1 until that call. */
    int                     listen_fd;
    struct isz_fd_tag       listen_tag;
    struct isz_fd_tag       psi_tag;
    struct isz_fd_tag       backend_tag;

    /* Logging. The runtime filter lives in W1-A's isz_log.c (process-
     * wide statics, since isz_set_log_callback predates the server
     * handle). These fields mirror them per-server for inspection and
     * future per-server routing. */
    isz_log_fn              log_fn;
    void                   *log_userdata;
    enum isz_log_level      log_level;

    /* Crash recovery (SPEC §12, opt-in). */
    bool                    crash_recovery_enabled;

    /* §6.4 surface serial counter: 64-bit, monotonic, global to the
     * server lifetime, never reused. First surface gets serial 1; 0
     * is reserved for "no surface" on the read side. */
    uint64_t                next_surface_serial;
};

/* ------------------------------------------------------------------ */
/* Cross-wave accessors (internal).                                   */
/* ------------------------------------------------------------------ */

/* W1-E input layer stores its state on the server via these. */
struct isz_input_state *isz_server_get_input_state(isz_server *srv)
    ISZ_INTERNAL;
void                    isz_server_set_input_state(isz_server *srv,
                                                   struct isz_input_state *st)
    ISZ_INTERNAL;

/* Dispatch an event to all registered listeners for ev->type, in
 * registration order, on the calling (main) thread. NULL-tolerant. */
void isz_server_emit_event(isz_server *srv, const isz_event *ev)
    ISZ_INTERNAL;

/* Alias used by some subsystems; same function. */
void isz_emit_event(isz_server *srv, const isz_event *ev)
    ISZ_INTERNAL;

/* W2-C thread-pool API shim looks up the server's pool here. NULL
 * until W2-C wires pool creation. */
struct isz_thread_pool *isz_server_get_thread_pool(isz_server *srv)
    ISZ_INTERNAL;

/* §6.3: true if peer_pid's binary inode/dev or cgroup matches any
 * stored entry. Empty allowlist means deny-all. */
bool isz_allowlist_check(isz_server *srv, pid_t peer_pid)
    ISZ_INTERNAL;

/* Internal: tear down an isz_output's allocations without unlinking
 * it from the server list (caller handles list removal). Used by the
 * destroy path. */
void isz_output_destroy_internal(struct isz_output *out)
    ISZ_INTERNAL;

/* Internal: wrap a headless backend output into an isz_output, add it
 * to the server's outputs list, and emit ISZ_EVENT_OUTPUT_ADD. */
int  isz_server_wrap_headless_output(isz_server *srv,
                                     const struct isz_headless_output_info *info)
    ISZ_INTERNAL;

/* Internal: find a headless-wrapped output by headless_id and emit
 * ISZ_EVENT_OUTPUT_REMOVE. The wrapper stays valid until the
 * Architect calls isz_output_destroy (SPEC §10). */
void isz_server_unwrap_headless_output(isz_server *srv, uint32_t headless_id)
    ISZ_INTERNAL;

/* W4-A DRM backend internal accessors. These are NOT in isz.h; they
 * exist for the DRM backend wave to set / read DRM-specific output
 * state without touching the public ABI. All hidden (no ISZ_API). */

/* SPEC 10: pin a render node path for this output's scanout GPU. The
 * path must be one of the render nodes the DRM backend enumerated at
 * init. Returns ISZ_ERR_INVALID_ARG on a NULL path or unknown node,
 * ISZ_OK on success. */
int  isz_output_set_gpu_node(isz_output *out, const char *render_node_path)
    ISZ_INTERNAL;

/* SPEC 7.2: read back the HDR metadata blob the Architect set via
 * isz_output_set_hdr_metadata (or that the EDID parser populated).
 * Returns a pointer into the output's storage; valid until the next
 * set call or output destruction. NULL-tolerant. */
const isz_hdr_metadata *isz_output_get_hdr_metadata(isz_output *out)
    ISZ_INTERNAL;

/* SPEC 7.2: enable / disable VRR on the next commit. The atomic
 * builder reads out->vrr_enabled and sets VRR_ENABLED on the CRTC.
 * Setting VRR on a CRTC that doesn't advertise vrr_capable is a
 * no-op (the builder skips the prop). */
int  isz_output_set_vrr_enabled(isz_output *out, bool enabled)
    ISZ_INTERNAL;

/* ------------------------------------------------------------------ */
/* isz_listen + client accept / recv path (isz_listen.c)              */
/* ------------------------------------------------------------------ */
/* isz_accept_connection is called from isz_dispatch when the listen fd
 * reports EPOLLIN. It accepts one client, runs the §6.2 handshake on
 * a blocking socket, runs the §6.3 allowlist check via SO_PEERCRED,
 * switches the client fd to non-blocking, wraps it in an isz_conn, and
 * adds it to the server's epoll set with the client tag. */
void isz_accept_connection(isz_server *srv) ISZ_INTERNAL;

/* isz_recv_client_messages is called from isz_dispatch when a client
 * fd reports EPOLLIN. It drains at least one framed message and routes
 * each to isz_handle_client_message. On EOF or hard error it runs the
 * §6.12 cleanup and emits CLIENT_DISCONNECT. */
void isz_recv_client_messages(isz_server *srv, struct isz_client *c)
    ISZ_INTERNAL;

/* Per-message dispatcher (isz_client_dispatch.c). Parses the payload,
 * looks up the target object via isz_conn_lookup_object, calls the
 * corresponding isz_* API on the connection's behalf, and queues the
 * reply (or an ISZ_MSG_ERROR) on the connection's outbound queue.
 *
 * Per SPEC §6.12 fault tolerance: malformed payloads and bad object IDs
 * log a warning + queue an error reply + continue. Only truly fatal
 * violations (message before handshake_done) disconnect; the caller is
 * responsible for closing the conn in that case (return value != 0).
 *
 * Returns ISZ_OK on tolerated errors / successful handling, or a
 * negative ISZ_ERR_* if the dispatcher flagged this connection for
 * disconnect (the caller marks the conn dead and runs §6.12 cleanup). */
int isz_handle_client_message(isz_server *srv, struct isz_conn *conn,
                              uint32_t msg_id,
                              const uint8_t *payload, size_t payload_len,
                              const int *fds, size_t n_fds)
    ISZ_INTERNAL;

#endif /* ISZ_SERVER_INTERNAL_H */
