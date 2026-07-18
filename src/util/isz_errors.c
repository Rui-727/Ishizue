/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_errors.c - implements isz_strerror() from SPEC §7.10. */

#include <ishizue/isz.h>

ISZ_API const char *isz_strerror(int err)
{
    switch (err) {
    case ISZ_OK:                        return "success";
    case ISZ_ERR_COMMIT_FAILED:         return "atomic commit failed, state rolled back";
    case ISZ_ERR_COMMIT_PENDING:        return "commit already pending on this output";
    case ISZ_ERR_RESOURCE_LIMIT:        return "build-time resource limit exceeded";
    case ISZ_ERR_SURFACE_NO_PLANE_SLOT: return "surface has no plane slot assigned";
    case ISZ_ERR_PLANE_UNAVAIL:         return "requested plane type unavailable";
    case ISZ_ERR_TRANSFORM_UNSUPPORTED: return "transform unsupported by assigned plane";
    case ISZ_ERR_OUTPUT_DISCONNECTED:   return "output has been removed";
    case ISZ_ERR_INVALID_DMABUF:        return "invalid DMA-BUF fd or format";
    case ISZ_ERR_CLIENT_DISCONNECTED:   return "client has disconnected";
    case ISZ_ERR_INVALID_ARG:           return "invalid argument";
    case ISZ_ERR_FEATURE_UNAVAIL:       return "feature not built in or unavailable";
    case ISZ_ERR_NO_MEMORY:             return "out of memory";
    case ISZ_ERR_DRM_MASTER:            return "DRM master acquisition failed";
    case ISZ_ERR_CLIENT_TOO_SLOW:       return "client event queue overflowed";
    case ISZ_ERR_ACCESS_DENIED:         return "access denied";
    default:                            return "unknown error";
    }
}
