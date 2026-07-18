/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_thread_pool_api.c - public isz_thread_pool_submit() declared in isz.h.
 * Looks up the server's thread pool via isz_server_get_thread_pool (defined
 * by the lifecycle wave / W2-A server struct) and forwards to the internal
 * submit. SPEC §5. */

#define _POSIX_C_SOURCE 200809L

#include <ishizue/isz.h>

#include "util/isz_log.h"
#include "util/isz_thread_pool.h"

/* Forward declaration: the lifecycle wave owns struct isz_server and the
 * pool pointer inside it. Declared extern here so this translation unit
 * links against whichever wave lands the server struct. */
extern struct isz_thread_pool *isz_server_get_thread_pool(isz_server *srv);

ISZ_API int isz_thread_pool_submit(isz_server *srv, isz_work_fn fn, void *ctx)
{
    if (srv == NULL || fn == NULL) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "isz_thread_pool_submit: NULL server or work fn");
        return -1;
    }

    struct isz_thread_pool *pool = isz_server_get_thread_pool(srv);
    /* When ENABLE_THREAD_POOL=0 the pool is NULL and submit_work runs the
     * work inline, returning a signalled eventfd - same caller path. */
    int fd = isz_thread_pool_submit_work(pool, fn, ctx);
    if (fd < 0)
        isz_log_internal(ISZ_LOG_ERROR,
                         "isz_thread_pool_submit: submit failed (pool=%p)",
                         (void *)pool);
    return fd;
}
