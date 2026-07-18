/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_thread.h - thin pthread wrappers for mutex and condition variable.
 * Used for the SPEC §5 thread pool queue; core lists stay lock-free. */

#ifndef ISZ_THREAD_H
#define ISZ_THREAD_H

#include <pthread.h>

#include "isz_compiler.h"

typedef struct {
    pthread_mutex_t raw;
} isz_mutex;

typedef struct {
    pthread_cond_t raw;
} isz_cond;

/* Return 0 on success, errno on failure (pthread convention). */
ISZ_INTERNAL int  isz_mutex_init(isz_mutex *m);
ISZ_INTERNAL int  isz_mutex_lock(isz_mutex *m);
ISZ_INTERNAL int  isz_mutex_unlock(isz_mutex *m);
ISZ_INTERNAL void isz_mutex_destroy(isz_mutex *m);

ISZ_INTERNAL int  isz_cond_init(isz_cond *c);
ISZ_INTERNAL int  isz_cond_signal(isz_cond *c);
ISZ_INTERNAL int  isz_cond_broadcast(isz_cond *c);
ISZ_INTERNAL int  isz_cond_wait(isz_cond *c, isz_mutex *m);
ISZ_INTERNAL void isz_cond_destroy(isz_cond *c);

#endif /* ISZ_THREAD_H */
