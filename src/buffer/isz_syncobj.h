/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_syncobj.h -- drm_syncobj helpers for explicit buffer sync (SPEC 7.5).
 *
 * Wraps the libdrm drmSyncobj* calls behind a small server-internal API
 * so the buffer and atomic-commit layers don't sprinkle raw libdrm
 * headers through their includes. Every declaration here is only
 * visible when ISHIZUE_HAVE_DRM is defined; without libdrm the whole
 * explicit-sync path compiles out and the server falls back to the
 * DMA-BUF's implicit kernel fencing (SPEC 11 fallback matrix).
 *
 * The syncobj handles returned by isz_syncobj_create are kernel-owned
 * drm_syncobj handles (uint32_t) valid for the drm_fd they were created
 * on. They are not file descriptors; the fd form is exposed on demand
 * via isz_syncobj_export_sync_file so other code (including the atomic
 * commit's IN_FENCE_FD plane property) can consume them. */
#ifndef ISZ_SYNCOBJ_H
#define ISZ_SYNCOBJ_H

#include <stdint.h>

#include "../util/isz_compiler.h"

#ifdef ISHIZUE_HAVE_DRM

/* One-time probe of the DRM fd for DRM_CAP_SYNCOBJ support. Returns
 * ISZ_OK if the kernel + driver support non-timeline syncobjs, or
 * ISZ_ERR_FEATURE_UNAVAIL otherwise (the caller falls back to implicit
 * sync, per SPEC 11). The probe result is cached by the caller on the
 * backend state so we don't repeat the drmGetCap on every commit. */
int isz_syncobj_init(int drm_fd) ISZ_INTERNAL;

/* Create a fresh, unsignalled syncobj. Returns 0 on failure (the
 * caller logs and falls back to implicit sync for that buffer). */
uint32_t isz_syncobj_create(int drm_fd) ISZ_INTERNAL;

/* Destroy a syncobj handle. Tolerates handle == 0 (no-op). */
void isz_syncobj_destroy(int drm_fd, uint32_t handle) ISZ_INTERNAL;

/* Attach an existing dma-fence (carried by sync_file_fd) to the
 * syncobj. The syncobj becomes signalled when the fence signals. The
 * caller still owns sync_file_fd and must close it. Returns ISZ_OK or
 * a negative ISZ_ERR_*. */
int isz_syncobj_import_sync_file(int drm_fd, uint32_t handle,
                                 int sync_file_fd) ISZ_INTERNAL;

/* Materialise a sync_file fd from the syncobj's current fence. The
 * caller owns the returned fd and must close it. Returns -1 on failure
 * or when the syncobj has no fence attached yet. */
int isz_syncobj_export_sync_file(int drm_fd, uint32_t handle) ISZ_INTERNAL;

/* Reset the syncobj back to unsignalled. Used by the buffer layer
 * before reusing a buffer's out_syncobj slot across commits. */
void isz_syncobj_reset(int drm_fd, uint32_t handle) ISZ_INTERNAL;

/* Signal the syncobj from the CPU. Used in test paths and when the
 * library wants to short-circuit a wait (e.g. headless backend
 * simulating a page-flip). */
void isz_syncobj_signal(int drm_fd, uint32_t handle) ISZ_INTERNAL;

/* Poll whether the syncobj's fence has signalled. Returns true if
 * signalled (or if handle == 0, treated as "no fence, ready"). The
 * wait is non-blocking; the caller is responsible for retrying on
 * later dispatch iterations. */
bool isz_syncobj_poll(int drm_fd, uint32_t handle) ISZ_INTERNAL;

#endif /* ISHIZUE_HAVE_DRM */

#endif /* ISZ_SYNCOBJ_H */
