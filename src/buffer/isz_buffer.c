/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_buffer.c - DMA-BUF import, release tracking, per-client cache.
 * SPEC §8. Wave 1 scope: tracking and bookkeeping only; the actual
 * drmPrimeFDToHandle() call is made by the DRM backend wave, guarded
 * here by ISHIZUE_HAVE_DRM. */

/* Enable POSIX.1-2008 symbols under -std=c11 (close, munmap, mmap). */
#define _POSIX_C_SOURCE 200809L

#include "isz_buffer.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ishizue/isz.h>

/* When libdrm is available, the DRM backend wave will define
 * isz_buffer_drm_import() to call drmPrimeFDToHandle() and stash the
 * GEM handle on the buffer. Until then, import just records the fd. */
#ifdef ISHIZUE_HAVE_DRM
extern int isz_buffer_drm_import(struct isz_buffer *buf);
extern void isz_buffer_drm_release(struct isz_buffer *buf);
#endif

/* ------------------------------------------------------------------ */
/* Import / release / refcount                                        */
/* ------------------------------------------------------------------ */

int isz_buffer_import(uint32_t client_id, int dmabuf_fd,
                      isz_buffer_desc *desc,
                      struct isz_buffer **out)
{
    if (out == NULL) {
        if (dmabuf_fd >= 0) close(dmabuf_fd);
        return ISZ_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (dmabuf_fd < 0 || desc == NULL) {
        return ISZ_ERR_INVALID_ARG;
    }
    /* SPEC §8 descriptor: width/height/stride are non-zero for any
     * usable buffer. Reject early so the caller gets INVALID_DMABUF
     * rather than NO_MEMORY later. */
    if (desc->width == 0 || desc->height == 0 || desc->stride == 0) {
        close(dmabuf_fd);
        return ISZ_ERR_INVALID_DMABUF;
    }

    struct isz_buffer *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        close(dmabuf_fd);
        return ISZ_ERR_NO_MEMORY;
    }

    buf->dmabuf_fd  = dmabuf_fd;
    buf->width      = desc->width;
    buf->height     = desc->height;
    buf->stride     = desc->stride;
    buf->offset     = desc->offset;
    buf->format     = desc->format;
    buf->modifier   = desc->modifier;
    buf->alpha_mode = desc->alpha_mode;
    buf->client_id  = client_id;
    buf->released   = false;
    buf->refcount   = 1;
    buf->next       = NULL;
    buf->priv       = NULL;
    buf->priv_size  = 0;
    buf->is_shm     = false;

#ifdef ISHIZUE_HAVE_DRM
    int ret = isz_buffer_drm_import(buf);
    if (ret != ISZ_OK) {
        /* isz_buffer_drm_import must not close dmabuf_fd on failure;
         * the destroy path below owns it. */
        isz_buffer_unref(buf);
        return ret;
    }
#endif

    *out = buf;
    return ISZ_OK;
}

void isz_buffer_release(struct isz_buffer *buf)
{
    if (buf == NULL) return;
    buf->released = true;
}

void isz_buffer_ref(struct isz_buffer *buf)
{
    if (buf == NULL) return;
    /* refcount is uint32_t; the cache and the in-flight commit list are
     * the only holders, so wraparound is not a practical concern. */
    buf->refcount++;
}

void isz_buffer_unref(struct isz_buffer *buf)
{
    if (buf == NULL) return;
    if (buf->refcount > 1u) {
        buf->refcount--;
        return;
    }

    /* Last reference. Tear down backing storage. */
#ifdef ISHIZUE_HAVE_DRM
    isz_buffer_drm_release(buf);
#endif

    if (buf->is_shm && buf->priv != NULL) {
        /* SHM fallback: munmap the CPU mapping. SPEC §11. The SHM
         * module sets priv to the mmap base and priv_size to the
         * length; we own the unmap here so teardown stays in one
         * place and the SHM module owns only import setup. */
        (void)munmap(buf->priv, buf->priv_size);
        buf->priv      = NULL;
        buf->priv_size = 0;
    }

    if (buf->dmabuf_fd >= 0) {
        close(buf->dmabuf_fd);
        buf->dmabuf_fd = -1;
    }

    free(buf);
}

/* ------------------------------------------------------------------ */
/* Per-client import cache                                            */
/* ------------------------------------------------------------------ */

static unsigned cache_hash(int fd)
{
    /* FNV-ish on the fd. Slots are 32; any decent spread works. */
    unsigned h = (unsigned)fd * 2654435761u;
    return h % ISZ_BUFFER_CACHE_SLOTS;
}

void isz_buffer_cache_init(struct isz_buffer_cache *cache)
{
    if (cache == NULL) return;
    for (size_t i = 0; i < ISZ_BUFFER_CACHE_SLOTS; i++) {
        cache->slots[i].fd  = -1;
        cache->slots[i].buf = NULL;
    }
    cache->count = 0;
}

struct isz_buffer *isz_buffer_cache_lookup(struct isz_buffer_cache *cache,
                                           int fd)
{
    if (cache == NULL || fd < 0) return NULL;

    unsigned start = cache_hash(fd);
    for (size_t i = 0; i < ISZ_BUFFER_CACHE_SLOTS; i++) {
        unsigned idx = (unsigned)((start + i) % ISZ_BUFFER_CACHE_SLOTS);
        struct isz_buffer_cache_entry *e = &cache->slots[idx];
        if (e->fd == -1) {
            /* Empty slot: with linear probing and no deletion, the
             * probe chain stops here. evict() preserves this invariant
             * by reinserting the chain after removal. */
            return NULL;
        }
        if (e->fd == fd) {
            return e->buf;
        }
    }
    return NULL;
}

int isz_buffer_cache_insert(struct isz_buffer_cache *cache,
                            struct isz_buffer *buf)
{
    if (cache == NULL || buf == NULL) return ISZ_ERR_INVALID_ARG;
    if (buf->dmabuf_fd < 0) return ISZ_ERR_INVALID_ARG;

    unsigned start = cache_hash(buf->dmabuf_fd);
    for (size_t i = 0; i < ISZ_BUFFER_CACHE_SLOTS; i++) {
        unsigned idx = (unsigned)((start + i) % ISZ_BUFFER_CACHE_SLOTS);
        struct isz_buffer_cache_entry *e = &cache->slots[idx];
        if (e->fd == buf->dmabuf_fd) {
            /* Already cached. Refresh the pointer and ref. */
            if (e->buf != NULL) isz_buffer_unref(e->buf);
            e->buf = buf;
            isz_buffer_ref(buf);
            return ISZ_OK;
        }
        if (e->fd == -1) {
            e->fd  = buf->dmabuf_fd;
            e->buf = buf;
            isz_buffer_ref(buf);
            cache->count++;
            return ISZ_OK;
        }
    }
    /* Cache full. The caller can still keep the buffer; it just won't
     * be cached. A v2 LRU eviction could go here; SPEC §8 doesn't
     * specify one. */
    return ISZ_ERR_RESOURCE_LIMIT;
}

void isz_buffer_cache_evict(struct isz_buffer_cache *cache, int fd)
{
    if (cache == NULL || fd < 0) return;

    unsigned start = cache_hash(fd);
    ssize_t victim = -1;
    for (size_t i = 0; i < ISZ_BUFFER_CACHE_SLOTS; i++) {
        unsigned idx = (unsigned)((start + i) % ISZ_BUFFER_CACHE_SLOTS);
        struct isz_buffer_cache_entry *e = &cache->slots[idx];
        if (e->fd == -1) {
            /* Probe chain ended before finding fd; nothing to evict. */
            return;
        }
        if (e->fd == fd) {
            victim = (ssize_t)idx;
            break;
        }
    }
    if (victim < 0) return;

    /* Drop the ref held by the cache, then reinsert everything that
     * hashed past the victim slot to preserve the lookup invariant. */
    if (cache->slots[victim].buf != NULL) {
        isz_buffer_unref(cache->slots[victim].buf);
    }
    cache->slots[victim].fd  = -1;
    cache->slots[victim].buf = NULL;
    if (cache->count > 0) cache->count--;

    /* Backshift the probe chain: for each subsequent occupied slot
     * whose canonical hash is at or before the victim, move it back. */
    unsigned hole = (unsigned)victim;
    for (size_t i = 1; i < ISZ_BUFFER_CACHE_SLOTS; i++) {
        unsigned cur = (unsigned)((hole + i) % ISZ_BUFFER_CACHE_SLOTS);
        struct isz_buffer_cache_entry *e = &cache->slots[cur];
        if (e->fd == -1) break;
        unsigned home = cache_hash(e->fd);
        /* Distance from hole to cur, going forward. */
        unsigned dist_cur = (unsigned)((cur - hole + ISZ_BUFFER_CACHE_SLOTS)
                                       % ISZ_BUFFER_CACHE_SLOTS);
        /* Distance from home to cur, going forward. */
        unsigned dist_home = (unsigned)((cur - home + ISZ_BUFFER_CACHE_SLOTS)
                                        % ISZ_BUFFER_CACHE_SLOTS);
        if (dist_home >= dist_cur) {
            /* e sits at or beyond its home; moving it to the hole would
             * place it before its home, breaking lookups. Leave it. */
            continue;
        }
        cache->slots[hole].fd  = e->fd;
        cache->slots[hole].buf = e->buf;
        e->fd  = -1;
        e->buf = NULL;
        hole = cur;
    }
}

void isz_buffer_cache_destroy(struct isz_buffer_cache *cache)
{
    if (cache == NULL) return;
    for (size_t i = 0; i < ISZ_BUFFER_CACHE_SLOTS; i++) {
        struct isz_buffer_cache_entry *e = &cache->slots[i];
        if (e->buf != NULL) {
            isz_buffer_unref(e->buf);
        }
        e->fd  = -1;
        e->buf = NULL;
    }
    cache->count = 0;
}
