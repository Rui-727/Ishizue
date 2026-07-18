/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_thread_pool.h - worker pool for off-main-thread work (texture upload,
 * shader compile, decode). SPEC §5. One mutex + one condvar guard a FIFO
 * queue; submit() hands back a pollable eventfd fence the caller waits on
 * before releasing anything the work function references. */

#ifndef ISZ_THREAD_POOL_H
#define ISZ_THREAD_POOL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include <ishizue/isz.h>

#include "isz_compiler.h"
#include "isz_thread.h"

struct isz_work_item {
    isz_work_fn           fn;
    void                 *ctx;
    int                   fence_fd;  /* eventfd, semaphore mode; caller-owned */
    struct isz_work_item *next;
};

struct isz_thread_pool {
    pthread_t            *workers;
    size_t               n_workers;   /* requested worker count */
    size_t               n_spawned;   /* workers actually created (for join on partial init) */
    isz_mutex            lock;
    isz_cond             cond;
    struct isz_work_item *head;
    struct isz_work_item *tail;
    bool                 shutdown;
};

/* n_workers == 0 falls back to ISZ_THREAD_POOL_SIZE (SPEC §4). Returns NULL
 * on allocation/thread failure. When ENABLE_THREAD_POOL=0 at build time this
 * is a no-op returning NULL. */
ISZ_INTERNAL struct isz_thread_pool *isz_thread_pool_create(size_t n_workers);

/* Sets shutdown, broadcasts the condvar, lets workers drain the remaining
 * queue (no cancellation, SPEC §5), joins them, frees the pool. NULL is a
 * no-op. */
ISZ_INTERNAL void isz_thread_pool_destroy(struct isz_thread_pool *pool);

/* Pushes fn/ctx onto the queue and returns a pollable eventfd fence
 * (EFD_SEMAPHORE | EFD_CLOEXEC). Caller owns the fd and closes it after
 * poll() fires. Returns -1 on failure.
 *
 * When ENABLE_THREAD_POOL=0 at build time: runs fn(ctx) inline and returns
 * an already-signalled eventfd so the caller's poll() fires immediately;
 * pool is ignored (and typically NULL).
 *
 * Suffix _work avoids colliding with the public isz_thread_pool_submit()
 * declared in isz.h, which wraps this. */
ISZ_INTERNAL int isz_thread_pool_submit_work(struct isz_thread_pool *pool,
                                             isz_work_fn fn, void *ctx);

#endif /* ISZ_THREAD_POOL_H */
