/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_composition_target.c -- DMA-BUF the Architect renders into for
 * software composition. SPEC 7.7.
 *
 * Wave 2 allocates a memfd as a stand-in. A real DMA-BUF needs the GPU
 * driver (drm_prime_alloc, gbm_bo, or dma-heap), which is a later wave.
 * The Architect gets an fd they can mmap and write pixels to; attaching
 * it to a surface via isz_surface_attach_buffer works the same as any
 * client dmabuf. */

#define _GNU_SOURCE 1

#include "isz_surface_internal.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../util/isz_log.h"

/* Bytes per pixel for the placeholder. The composition target's real
 * format is whatever the Architect passes in; for the memfd stand-in we
 * assume 4 bytes/pixel (XRGB8888 / ARGB8888) which covers the common
 * case. A later wave with real DMA-BUF allocation will honour `format`
 * exactly. */
#define ISZ_COMP_TARGET_BPP 4u

ISZ_API int isz_composition_target_create(isz_server *srv, uint32_t width,
                                          uint32_t height, uint32_t format,
                                          int *dmabuf_fd_out)
{
    if (!srv || !dmabuf_fd_out) return ISZ_ERR_INVALID_ARG;
    if (width == 0 || height == 0) return ISZ_ERR_INVALID_ARG;
    *dmabuf_fd_out = -1;

    (void)format;  /* honoured by the real DMA-BUF path */

    size_t size = (size_t)width * (size_t)height * ISZ_COMP_TARGET_BPP;

    int fd = memfd_create("ishizue-comp-target", MFD_CLOEXEC);
    if (fd < 0) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "composition_target: memfd_create failed");
        return ISZ_ERR_NO_MEMORY;
    }

    if (ftruncate(fd, (off_t)size) < 0) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "composition_target: ftruncate failed");
        close(fd);
        return ISZ_ERR_NO_MEMORY;
    }

    *dmabuf_fd_out = fd;
    return ISZ_OK;
}

ISZ_API int isz_composition_target_get_egl_image(int dmabuf_fd,
                                                 isz_buffer_desc *desc,
                                                 void **egl_image_out)
{
    (void)dmabuf_fd;
    (void)desc;
    if (!egl_image_out) return ISZ_ERR_INVALID_ARG;
    *egl_image_out = NULL;
    /* Real EGL needs Mesa headers and a current EGLDisplay. Stubbed
     * until the GLES wave lands. */
    return ISZ_ERR_FEATURE_UNAVAIL;
}
