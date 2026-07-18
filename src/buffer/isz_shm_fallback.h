/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_shm_fallback.h - SHM fallback import path. SPEC §11.
 *
 * SHM appears in v1 only as a fallback for the case where a client
 * produces a buffer whose format/modifier the scanout GPU can't
 * import. The server first attempts modifier renegotiation; if that
 * also fails, the client re-submits the buffer over a memfd/shm_fd
 * and the server mmaps it for CPU access. This is never the primary
 * path. */

#ifndef ISZ_SHM_FALLBACK_H
#define ISZ_SHM_FALLBACK_H

#include <ishizue/isz.h>

#include "isz_buffer.h"
#include "../util/isz_compiler.h"

/* Import a SHM-backed buffer for CPU access. SPEC §11.
 *
 * Takes ownership of shm_fd on every path, mirroring the DMA-BUF
 * ownership rule (§8): the caller must not close it after this call.
 * The fd is closed inside this function once the mmap is established;
 * the mapping persists independently of the fd per mmap(2). The
 * returned isz_buffer carries dmabuf_fd = -1, is_shm = true, and the
 * mmap pointer in priv.
 *
 * Returns ISZ_OK on success, an ISZ_ERR_* code on failure. On failure
 * shm_fd has been closed and *out is untouched. */
int isz_shm_import(int shm_fd, isz_buffer_desc *desc,
                   struct isz_buffer **out) ISZ_INTERNAL;

#endif /* ISZ_SHM_FALLBACK_H */
