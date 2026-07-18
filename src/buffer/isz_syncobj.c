/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_syncobj.c -- drm_syncobj helper implementations (SPEC 7.5).
 *
 * Without ISHIZUE_HAVE_DRM this file is an empty translation unit so
 * the library still links and the headless backend test runs. With
 * libdrm present, every helper is a thin wrapper around the matching
 * drmSyncobj* call, with the drm_fd threaded through explicitly so
 * the helpers stay stateless from the caller's perspective. */

#include "isz_syncobj.h"

#ifdef ISHIZUE_HAVE_DRM

#include <ishizue/isz.h>
#include "../util/isz_log.h"

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* drmSyncobjWait flags: DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL would block
 * until every syncobj in the array signals; for the single-syncobj
 * non-blocking poll we want here, we pass 0 and a zero timeout so the
 * call returns immediately with 0 = signalled, -errno = not signalled
 * (ETIME) or error. */

int isz_syncobj_init(int drm_fd)
{
    if (drm_fd < 0)
        return ISZ_ERR_FEATURE_UNAVAIL;

    uint64_t val = 0;
    int rc = drmGetCap(drm_fd, DRM_CAP_SYNCOBJ, &val);
    if (rc != 0 || val == 0) {
        isz_log_internal(ISZ_LOG_DEBUG,
                         "syncobj: DRM_CAP_SYNCOBJ unavailable (rc=%d val=%llu); "
                         "falling back to implicit DMA-BUF fencing",
                         rc, (unsigned long long)val);
        return ISZ_ERR_FEATURE_UNAVAIL;
    }
    isz_log_internal(ISZ_LOG_DEBUG,
                     "syncobj: DRM_CAP_SYNCOBJ supported; explicit sync enabled");
    return ISZ_OK;
}

uint32_t isz_syncobj_create(int drm_fd)
{
    if (drm_fd < 0)
        return 0;
    uint32_t handle = 0;
    if (drmSyncobjCreate(drm_fd, 0, &handle) != 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "syncobj: drmSyncobjCreate failed: %s",
                         strerror(errno));
        return 0;
    }
    return handle;
}

void isz_syncobj_destroy(int drm_fd, uint32_t handle)
{
    if (drm_fd < 0 || handle == 0)
        return;
    if (drmSyncobjDestroy(drm_fd, handle) != 0) {
        isz_log_internal(ISZ_LOG_DEBUG,
                         "syncobj: drmSyncobjDestroy handle=%u failed: %s",
                         (unsigned)handle, strerror(errno));
    }
}

int isz_syncobj_import_sync_file(int drm_fd, uint32_t handle,
                                 int sync_file_fd)
{
    if (drm_fd < 0 || handle == 0 || sync_file_fd < 0)
        return ISZ_ERR_INVALID_ARG;
    if (drmSyncobjImportSyncFile(drm_fd, handle, sync_file_fd) != 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "syncobj: ImportSyncFile handle=%u fd=%d failed: %s",
                         (unsigned)handle, sync_file_fd, strerror(errno));
        return ISZ_ERR_COMMIT_FAILED;
    }
    return ISZ_OK;
}

int isz_syncobj_export_sync_file(int drm_fd, uint32_t handle)
{
    if (drm_fd < 0 || handle == 0)
        return -1;
    int fd = -1;
    if (drmSyncobjExportSyncFile(drm_fd, handle, &fd) != 0) {
        isz_log_internal(ISZ_LOG_DEBUG,
                         "syncobj: ExportSyncFile handle=%u failed: %s",
                         (unsigned)handle, strerror(errno));
        return -1;
    }
    return fd;
}

void isz_syncobj_reset(int drm_fd, uint32_t handle)
{
    if (drm_fd < 0 || handle == 0)
        return;
    if (drmSyncobjReset(drm_fd, &handle, 1) != 0) {
        isz_log_internal(ISZ_LOG_DEBUG,
                         "syncobj: drmSyncobjReset handle=%u failed: %s",
                         (unsigned)handle, strerror(errno));
    }
}

void isz_syncobj_signal(int drm_fd, uint32_t handle)
{
    if (drm_fd < 0 || handle == 0)
        return;
    if (drmSyncobjSignal(drm_fd, &handle, 1) != 0) {
        isz_log_internal(ISZ_LOG_DEBUG,
                         "syncobj: drmSyncobjSignal handle=%u failed: %s",
                         (unsigned)handle, strerror(errno));
    }
}

bool isz_syncobj_poll(int drm_fd, uint32_t handle)
{
    /* handle == 0 means "no explicit fence attached"; treat as already
     * signalled so the caller's release path runs immediately. This
     * mirrors the implicit-sync branch in isz_buffer_on_page_flip. */
    if (handle == 0)
        return true;
    if (drm_fd < 0)
        return true;

    uint32_t handles[1] = { handle };
    int rc = drmSyncobjWait(drm_fd, handles, 1, 0 /* timeout_ns */,
                            0 /* flags */, NULL /* first_signaled */);
    if (rc == 0)
        return true;
    /* rc == -ETIMEDOUT means the fence is still pending; any other
     * -errno is treated as "signalled" so we don't wedge the release
     * list on a transient error. */
    return rc != -ETIMEDOUT;
}

#endif /* ISHIZUE_HAVE_DRM */
