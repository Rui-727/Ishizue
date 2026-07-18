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

#endif /* ISZ_BUFFER_H */
