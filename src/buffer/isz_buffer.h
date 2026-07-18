/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_buffer.h - tracked DMA-BUF buffer objects, per-client import cache.
 * SPEC §8 (Buffer & Memory Management). Internal; not installed. */

#ifndef ISZ_BUFFER_H
#define ISZ_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ishizue/isz.h>

#include "../util/isz_list.h"
#include "../util/isz_compiler.h"

/* A tracked buffer. Created by isz_buffer_import() (DMA-BUF) or
 * isz_shm_import() (SHM fallback, SPEC §11). The library owns the
 * underlying fd and/or mmap mapping for the buffer's lifetime.
 *
 * The fields below match the W1-D task spec. The trailing extension
 * fields (priv, priv_size, is_shm) carry the SHM-fallback mmap state
 * and are NULL/false for pure DMA-BUF imports. The W1-A parent task may
 * rewire `next` into an isz_list_node without touching the rest. */
struct isz_buffer {
    int       dmabuf_fd;   /* library-owned; -1 if SHM-only or destroyed */
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;
    uint32_t  offset;
    uint32_t  format;      /* DRM_FORMAT_* fourcc */
    uint64_t  modifier;    /* DRM_FORMAT_MOD_* or DRM_FORMAT_MOD_INVALID */
    uint8_t   alpha_mode;  /* isz_alpha_mode */
    uint32_t  client_id;
    bool      released;    /* release event sent (or queued) to client */
    uint32_t  refcount;    /* outstanding KMS-commit references */
    struct isz_buffer *next;  /* intrusive list link; W1-A rewires as needed */

    /* SHM-fallback extension. priv holds the mmap'd CPU pointer when
     * is_shm is true. NULL/0 for DMA-BUF imports. */
    void     *priv;
    size_t    priv_size;
    bool      is_shm;

    /* W5-B explicit-sync state (SPEC 7.5). Both syncobj handles are
     * server-side drm_syncobj handles created on the scanout GPU's
     * drm_fd, valid only while ISHIZUE_HAVE_DRM is defined and the
     * kernel advertises DRM_CAP_SYNCOBJ. They are 0 in every other
     * configuration, including the headless test backend. */
    uint32_t      in_syncobj;   /* render-GPU completion fence the
                                 * display GPU waits on before scanout;
                                 * 0 means "no explicit acquire fence" */
    uint32_t      out_syncobj;  /* fence the library populates from the
                                 * CRTC's OUT_FENCE_PTR after commit; the
                                 * buffer is releasable to the client
                                 * once it signals. 0 means implicit
                                 * sync, release on page-flip event. */

    /* Intrusive list node used by isz_buffer_on_page_flip /
     * isz_buffer_poll_out_fences to track buffers whose out_syncobj
     * has not signalled yet. Self-linked (not in any list) when the
     * buffer is not pending release. */
    isz_list_node release_node;

    /* True once the buffer has been queued on the pending-release
     * list so isz_buffer_unref can take it off before freeing. */
    bool          release_pending;
};

/* Per-client import cache. SPEC §8: "the library caches recently-imported
 * dmabuf handles per client. Repeated fds reuse the existing
 * drm_prime_fd_to_handle() result rather than re-importing."
 *
 * v1 keys on the fd directly. A follow-up can switch to (st_dev, st_ino)
 * to coalesce dup()ed fds from the same client; the API stays the same.
 *
 * Fixed-size open-addressed hash table. Small N (32) keeps collision
 * chains short; ISZ_MAX_DMABUF_IMPORTS_TOTAL (§4) bounds the live set
 * at a higher layer, so the cache is a hot-path accelerator, not the
 * authoritative tracker. */
#define ISZ_BUFFER_CACHE_SLOTS 32

struct isz_buffer_cache_entry {
    int                fd;    /* key; -1 marks an empty slot */
    struct isz_buffer *buf;   /* referenced; cache holds one ref per entry */
};

struct isz_buffer_cache {
    struct isz_buffer_cache_entry slots[ISZ_BUFFER_CACHE_SLOTS];
    uint32_t count;
};

/* Import takes ownership of dmabuf_fd on every path, success or failure:
 * the caller (isz_surface_attach_buffer) must not close it after this
 * call returns. SPEC §8: "the dmabuf_fd passed to
 * isz_surface_attach_buffer() is library-owned after the call."
 *
 * On success, *out holds a buffer with refcount 1. On failure, dmabuf_fd
 * is closed and *out is untouched. */
int  isz_buffer_import(uint32_t client_id, int dmabuf_fd,
                       isz_buffer_desc *desc,
                       struct isz_buffer **out) ISZ_INTERNAL;

/* Mark a buffer as releasable. The commit path later sends ISZ_MSG_RELEASE
 * over the wire (SPEC §8). Idempotent: a second call on an already-
 * released buffer is a no-op, matching the spec's "If the client has
 * already destroyed the buffer before the event arrives, the release is
 * a silent no-op server-side." */
void isz_buffer_release(struct isz_buffer *buf) ISZ_INTERNAL;

/* Refcount. The cache and the in-flight KMS-commit list each hold their
 * own ref. unref at zero closes the fd (DMA-BUF) or munmaps (SHM) and
 * frees the struct. NULL is tolerated. */
void isz_buffer_ref(struct isz_buffer *buf) ISZ_INTERNAL;
void isz_buffer_unref(struct isz_buffer *buf) ISZ_INTERNAL;

/* Cache lifecycle. init zeroes the table. lookup returns the cached
 * buffer (with its existing ref; caller does not gain a new ref) or
 * NULL on miss. insert gains a ref on buf and stores it; if the cache
 * is full, returns ISZ_ERR_RESOURCE_LIMIT and the caller falls through
 * to the uncached path (import still succeeds at the higher layer).
 * evict drops a specific fd's entry (called when a buffer is freed).
 * destroy drops all refs. */
void             isz_buffer_cache_init(struct isz_buffer_cache *cache) ISZ_INTERNAL;
struct isz_buffer *isz_buffer_cache_lookup(struct isz_buffer_cache *cache,
                                           int fd) ISZ_INTERNAL;
int              isz_buffer_cache_insert(struct isz_buffer_cache *cache,
                                         struct isz_buffer *buf) ISZ_INTERNAL;
void             isz_buffer_cache_evict(struct isz_buffer_cache *cache,
                                        int fd) ISZ_INTERNAL;
void             isz_buffer_cache_destroy(struct isz_buffer_cache *cache) ISZ_INTERNAL;

/* ------------------------------------------------------------------ */
/* W5-B: explicit-sync entry points (SPEC 7.5)                        */
/* ------------------------------------------------------------------ */
/* The functions below are the buffer-layer surface the DRM atomic
 * commit and page-flip paths call into. Declared unconditionally so
 * the no-libdrm build still sees the symbols; the implementations
 * are no-ops or feature-unavail returns without ISHIZUE_HAVE_DRM. */

struct isz_server;

/* DRM backend registers its fd at init so the buffer layer can reach
 * it for drmSyncobj* calls without threading it through every call
 * site. No-op without ISHIZUE_HAVE_DRM. */
void isz_buffer_set_drm_fd(int drm_fd) ISZ_INTERNAL;

/* Import a client-supplied sync_file fd as the buffer's acquire fence.
 * The server creates a fresh drm_syncobj, attaches the fence to it
 * via drmSyncobjImportSyncFile, and stores the handle on
 * buf->in_syncobj. The atomic commit later exports a sync_file from
 * this syncobj and sets it on the plane's IN_FENCE_FD property.
 *
 * Choice of convention: SPEC §8 doesn't fully specify the wire shape
 * for in-fences. We use the linux-drm-syncobj-v1 model: the client
 * passes a sync_file fd alongside the dmabuf (via SCM_RIGHTS, the
 * same cmsg channel as the dmabuf itself), the server imports it
 * into a server-side syncobj rather than juggling raw fds across
 * commits. This matches what wlroots / Mutter do today, keeps the
 * syncobj alive across multiple commits of the same buffer, and
 * lets the server close the client fd immediately.
 *
 * drm_fd < 0 (no DRM backend) returns ISZ_ERR_FEATURE_UNAVAIL: the
 * buffer carries no explicit in-fence and the atomic commit falls
 * back to implicit sync. */
int isz_buffer_set_in_fence_fd(struct isz_buffer *buf, int drm_fd,
                               int sync_file_fd) ISZ_INTERNAL;

/* Hook called by the DRM backend's page-flip event handler when a
 * commit's flip completes. For each buffer the just-completed commit
 * was scanning out, the backend adds the buffer to the server's
 * pending-release list with a ref, then calls this entry point.
 *
 * If buf->out_syncobj == 0 (implicit sync), the buffer is sent to
 * the owning client as ISZ_MSG_RELEASE immediately. If out_syncobj
 * != 0 (explicit sync), the buffer stays on the pending-release
 * list until isz_buffer_poll_out_fences observes the syncobj
 * signalled.
 *
 * srv may be NULL in the headless build; in that case the function
 * is a no-op (no wire to send on, no syncobj to wait for). */
void isz_buffer_on_page_flip(struct isz_server *srv,
                             struct isz_buffer *buf) ISZ_INTERNAL;

/* Walk the server's pending-release list once. For each buffer whose
 * out_syncobj has signalled, send ISZ_MSG_RELEASE and drop the buffer
 * from the list. Called from isz_dispatch every iteration so
 * explicit-sync releases make progress even without further
 * page-flip events.
 *
 * Without ISHIZUE_HAVE_DRM this is a no-op: no syncobj is ever set
 * on a buffer, so the pending-release list is always empty. */
void isz_buffer_poll_out_fences(struct isz_server *srv) ISZ_INTERNAL;

/* Drop any pending-release entries the server still holds. Called
 * from isz_destroy so a teardown mid-flip doesn't leak buffer refs. */
void isz_buffer_release_destroy(struct isz_server *srv) ISZ_INTERNAL;

/* Wire send for ISZ_MSG_RELEASE (SPEC §8). Looks up the buffer's
 * owning client and queues the message on its outbound connection.
 * Returns ISZ_OK on success or a tolerant failure (client gone,
 * buffer already destroyed); the caller always removes the buffer
 * from the pending-release list regardless. The real wire encoding
 * lands with the client dispatch wave; until then this is a
 * debug-log stub so the link stays clean. */
int isz_buffer_send_release(struct isz_server *srv,
                            struct isz_buffer *buf) ISZ_INTERNAL;

#endif /* ISZ_BUFFER_H */
