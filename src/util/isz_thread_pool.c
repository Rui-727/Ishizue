/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_thread_pool.c - worker threads, FIFO queue, eventfd fences. SPEC §5.
 * The queue is a plain singly-linked list with head/tail; one mutex + one
 * condvar protect it. Workers pop, run, signal the fence, free the item.
 * On shutdown the condvar is broadcast and workers keep draining until the
 * queue is empty before exiting (no cancellation, SPEC §5). */

#define _POSIX_C_SOURCE 200809L

#include "isz_thread_pool.h"

#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#ifndef ENABLE_THREAD_POOL
#define ENABLE_THREAD_POOL 1
#endif

#ifndef ISZ_THREAD_POOL_SIZE
#define ISZ_THREAD_POOL_SIZE 4
#endif

#if ENABLE_THREAD_POOL

static void *isz_thread_pool_worker(void *arg)
{
    struct isz_thread_pool *pool = arg;

    for (;;) {
        isz_mutex_lock(&pool->lock);
        while (pool->head == NULL && !pool->shutdown)
            isz_cond_wait(&pool->cond, &pool->lock);

        /* Shutdown with an empty queue: exit. A non-empty queue is drained
         * first (no cancellation, SPEC §5). */
        if (pool->head == NULL && pool->shutdown) {
            isz_mutex_unlock(&pool->lock);
            return NULL;
        }

        struct isz_work_item *item = pool->head;
        pool->head = item->next;
        if (pool->head == NULL)
            pool->tail = NULL;
        isz_mutex_unlock(&pool->lock);

        item->fn(item->ctx);

        /* Signal the fence before freeing so the caller wakes immediately. */
        uint64_t one = 1;
        (void)write(item->fence_fd, &one, sizeof(one));

        free(item);
    }
}

struct isz_thread_pool *isz_thread_pool_create(size_t n_workers)
{
    if (n_workers == 0)
        n_workers = ISZ_THREAD_POOL_SIZE;

    struct isz_thread_pool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL)
        return NULL;

    pool->workers = calloc(n_workers, sizeof(*pool->workers));
    if (pool->workers == NULL) {
        free(pool);
        return NULL;
    }
    pool->n_workers = n_workers;

    if (isz_mutex_init(&pool->lock) != 0) {
        free(pool->workers);
        free(pool);
        return NULL;
    }
    if (isz_cond_init(&pool->cond) != 0) {
        isz_mutex_destroy(&pool->lock);
        free(pool->workers);
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < n_workers; i++) {
        if (pthread_create(&pool->workers[i], NULL,
                           isz_thread_pool_worker, pool) != 0) {
            /* Roll back: tell the workers already spawned to exit, join. */
            isz_mutex_lock(&pool->lock);
            pool->shutdown = true;
            isz_cond_broadcast(&pool->cond);
            isz_mutex_unlock(&pool->lock);
            for (size_t j = 0; j < pool->n_spawned; j++)
                pthread_join(pool->workers[j], NULL);
            isz_cond_destroy(&pool->cond);
            isz_mutex_destroy(&pool->lock);
            free(pool->workers);
            free(pool);
            return NULL;
        }
        pool->n_spawned++;
    }

    /* TODO: CPU pinning via an Architect-supplied cpu_set_t (SPEC §5). */
    return pool;
}

void isz_thread_pool_destroy(struct isz_thread_pool *pool)
{
    if (pool == NULL)
        return;

    isz_mutex_lock(&pool->lock);
    pool->shutdown = true;
    isz_cond_broadcast(&pool->cond);
    isz_mutex_unlock(&pool->lock);

    for (size_t i = 0; i < pool->n_spawned; i++)
        pthread_join(pool->workers[i], NULL);

    /* Workers drain on shutdown, so head is NULL here. No defensive free. */

    isz_cond_destroy(&pool->cond);
    isz_mutex_destroy(&pool->lock);
    free(pool->workers);
    free(pool);
}

int isz_thread_pool_submit_work(struct isz_thread_pool *pool,
                                isz_work_fn fn, void *ctx)
{
    if (pool == NULL || fn == NULL)
        return -1;

    int fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
    if (fd < 0)
        return -1;

    struct isz_work_item *item = malloc(sizeof(*item));
    if (item == NULL) {
        close(fd);
        return -1;
    }
    item->fn       = fn;
    item->ctx      = ctx;
    item->fence_fd = fd;
    item->next     = NULL;

    isz_mutex_lock(&pool->lock);
    if (pool->tail == NULL)
        pool->head = item;
    else
        pool->tail->next = item;
    pool->tail = item;
    isz_cond_signal(&pool->cond);
    isz_mutex_unlock(&pool->lock);

    return fd;
}

#else  /* !ENABLE_THREAD_POOL */

/* Disabled build (SPEC §4): no worker threads. submit runs the work inline
 * on the caller's thread and returns an already-signalled eventfd so the
 * caller's poll() path is identical to the enabled build. create/destroy
 * are no-ops. */

struct isz_thread_pool *isz_thread_pool_create(size_t n_workers)
{
    (void)n_workers;
    return NULL;
}

void isz_thread_pool_destroy(struct isz_thread_pool *pool)
{
    (void)pool;
}

int isz_thread_pool_submit_work(struct isz_thread_pool *pool,
                                isz_work_fn fn, void *ctx)
{
    (void)pool;
    if (fn != NULL)
        fn(ctx);

    /* initval=1 in semaphore mode: poll() fires immediately, one read
     * returns 1 and decrements to 0. */
    return eventfd(1, EFD_SEMAPHORE | EFD_CLOEXEC);
}

#endif  /* ENABLE_THREAD_POOL */
