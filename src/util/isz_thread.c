/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_thread.c - pthread wrappers, see SPEC §5. */

#include "isz_thread.h"

int isz_mutex_init(isz_mutex *m)
{
    return pthread_mutex_init(&m->raw, NULL);
}

int isz_mutex_lock(isz_mutex *m)
{
    return pthread_mutex_lock(&m->raw);
}

int isz_mutex_unlock(isz_mutex *m)
{
    return pthread_mutex_unlock(&m->raw);
}

void isz_mutex_destroy(isz_mutex *m)
{
    pthread_mutex_destroy(&m->raw);
}

int isz_cond_init(isz_cond *c)
{
    return pthread_cond_init(&c->raw, NULL);
}

int isz_cond_signal(isz_cond *c)
{
    return pthread_cond_signal(&c->raw);
}

int isz_cond_broadcast(isz_cond *c)
{
    return pthread_cond_broadcast(&c->raw);
}

int isz_cond_wait(isz_cond *c, isz_mutex *m)
{
    return pthread_cond_wait(&c->raw, &m->raw);
}

void isz_cond_destroy(isz_cond *c)
{
    pthread_cond_destroy(&c->raw);
}
