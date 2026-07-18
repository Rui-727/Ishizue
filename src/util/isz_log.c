/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_log.c - logging callback registration and internal dispatch.
 * SPEC §4 (env vars read once, cached) and §12 (no built-in stderr). */

#include "isz_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ISZ_LOG_BUF 1024

static _Atomic(isz_log_fn) s_callback;
static _Atomic(void *)     s_userdata;
static enum isz_log_level  s_max_level;
static pthread_once_t      s_once = PTHREAD_ONCE_INIT;

static enum isz_log_level parse_level(const char *s)
{
    if (s == NULL)                  return ISZ_LOG_WARN;
    if (strcmp(s, "error") == 0)    return ISZ_LOG_ERROR;
    if (strcmp(s, "warn")  == 0)    return ISZ_LOG_WARN;
    if (strcmp(s, "info")  == 0)    return ISZ_LOG_INFO;
    if (strcmp(s, "debug") == 0)    return ISZ_LOG_DEBUG;
    return ISZ_LOG_WARN;
}

static void init_level(void)
{
    s_max_level = parse_level(getenv("ISZ_LOG_LEVEL"));
}

ISZ_API void isz_set_log_callback(isz_log_fn fn, void *userdata)
{
    /* userdata first so a worker that picks up the new callback also sees
     * the matching userdata (release/acquire pairing in isz_log_internal). */
    atomic_store_explicit(&s_userdata, userdata, memory_order_release);
    atomic_store_explicit(&s_callback, fn,       memory_order_release);
}

ISZ_INTERNAL void isz_log_internal(enum isz_log_level level, const char *fmt, ...)
{
    pthread_once(&s_once, init_level);

    if (level > s_max_level)
        return;

    isz_log_fn cb = atomic_load_explicit(&s_callback, memory_order_acquire);
    if (cb == NULL)
        return;
    void *ud = atomic_load_explicit(&s_userdata, memory_order_acquire);

    char buf[ISZ_LOG_BUF];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    cb(ud, level, buf);
}
