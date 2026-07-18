/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_shm_fallback.c - SHM fallback import. SPEC §11.
 *
 * Wave 1 scope: just the shape. mmap the shm_fd for PROT_READ|PROT_WRITE,
 * wrap it in an isz_buffer marked is_shm = true. The actual CPU-access
 * compositing path (read pixels, convert format, upload to a GPU
 * composition target) is a later wave's problem; this file only
 * establishes the mapping and lifetime. */

/* Enable POSIX.1-2008 symbols under -std=c11 (close, mmap, munmap). */
#define _POSIX_C_SOURCE 200809L

#include "isz_shm_fallback.h"

#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ishizue/isz.h>

int isz_shm_import(int shm_fd, isz_buffer_desc *desc,
                   struct isz_buffer **out)
{
    if (out == NULL) {
        if (shm_fd >= 0) close(shm_fd);
        return ISZ_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (shm_fd < 0 || desc == NULL) {
        return ISZ_ERR_INVALID_ARG;
    }
    if (desc->width == 0 || desc->height == 0 || desc->stride == 0) {
        close(shm_fd);
        return ISZ_ERR_INVALID_DMABUF;
    }

    /* Map enough to cover offset + stride * height. SPEC §8 mapping
     * discipline: mmap is used only to read metadata and, in the SHM
     * fallback case, to give the CPU access to pixel data. */
    size_t map_size = (size_t)desc->offset
                    + (size_t)desc->stride * (size_t)desc->height;

    void *ptr = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     shm_fd, 0);
    if (ptr == MAP_FAILED) {
        close(shm_fd);
        return ISZ_ERR_INVALID_DMABUF;
    }

    /* The mapping holds a reference to the underlying file; the fd is
     * no longer needed. Closing it here matches the DMA-BUF ownership
     * contract (caller hands us the fd, we own its lifecycle) without
     * forcing isz_buffer to carry a separate shm_fd field. */
    close(shm_fd);

    struct isz_buffer *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        munmap(ptr, map_size);
        return ISZ_ERR_NO_MEMORY;
    }

    buf->dmabuf_fd  = -1;
    buf->width      = desc->width;
    buf->height     = desc->height;
    buf->stride     = desc->stride;
    buf->offset     = desc->offset;
    buf->format     = desc->format;
    buf->modifier   = desc->modifier;
    buf->alpha_mode = desc->alpha_mode;
    buf->client_id  = 0;   /* set by the caller once routed to a client */
    buf->released   = false;
    buf->refcount   = 1;
    buf->next       = NULL;
    buf->priv       = ptr;
    buf->priv_size  = map_size;
    buf->is_shm     = true;

    *out = buf;
    return ISZ_OK;
}
