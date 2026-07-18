/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Rui-727
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* isz_capture_consent.c -- SPEC section 6.11 / 7.11 capture consent.
 *
 * Portal-style consent: an explicit per-request user grant, with a
 * timeout (default 60s). isz_output_capture_start (W2-B) calls
 * isz_capture_check_consent before programming the writeback connector;
 * without a valid grant it returns ISZ_ERR_ACCESS_DENIED.
 *
 * The Architect wires up the granting side however they want: a
 * keybinding, a dialog, a D-Bus prompt. They call isz_capture_grant
 * when the user approves, and the library checks the flag at
 * capture_start time.
 *
 * Storage is a fixed-size array keyed by (server, output). Wave 2
 * supports up to ISZ_CAPTURE_CONSENT_SLOTS simultaneous grants. The
 * single-threaded API contract (SPEC section 5) means no locking is
 * needed. Lazy expiry: expired entries are reclaimed on the next
 * check or grant. */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <ishizue/isz.h>
#include "util/isz_compiler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define ISZ_CAPTURE_CONSENT_SLOTS 16
#define ISZ_CAPTURE_CONSENT_TIMEOUT_NS \
    (60ull * 1000ull * 1000ull * 1000ull)  /* 60 seconds */

struct isz_consent_entry {
    isz_server *srv;
    isz_output *output;
    uint64_t    expires_ns;
    bool        used;
};

static struct isz_consent_entry g_consent[ISZ_CAPTURE_CONSENT_SLOTS];

static uint64_t isz_consent_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Find a slot matching (srv, output). If find_free is true and no
 * matching slot exists, return a free slot, or if the table is full,
 * the entry closest to expiry (eviction). Returns NULL only when the
 * table is empty, which cannot happen since the array is non-empty. */
static struct isz_consent_entry *isz_consent_find(isz_server *srv,
                                                   isz_output *output,
                                                   bool find_free)
{
    struct isz_consent_entry *free_slot = NULL;
    struct isz_consent_entry *oldest = NULL;
    uint64_t oldest_expiry = UINT64_MAX;

    for (size_t i = 0; i < ISZ_CAPTURE_CONSENT_SLOTS; i++) {
        struct isz_consent_entry *e = &g_consent[i];
        if (!e->used) {
            if (find_free && !free_slot)
                free_slot = e;
            continue;
        }
        if (e->srv == srv && e->output == output)
            return e;
        if (find_free && e->expires_ns < oldest_expiry) {
            oldest_expiry = e->expires_ns;
            oldest = e;
        }
    }
    if (!find_free)
        return NULL;
    return free_slot ? free_slot : oldest;
}

/* Public: the Architect calls this when the user approves capture for
 * a specific output. The grant is valid for 60 seconds. Exported so
 * the Architect can call it from their own consent UI code linked
 * against the .so. */
ISZ_API void isz_capture_grant(isz_server *srv, isz_output *output)
{
    if (!srv || !output)
        return;
    struct isz_consent_entry *e = isz_consent_find(srv, output, true);
    if (!e)
        return;
    e->srv         = srv;
    e->output      = output;
    e->expires_ns  = isz_consent_now_ns() + ISZ_CAPTURE_CONSENT_TIMEOUT_NS;
    e->used        = true;
}

/* Internal: called by isz_output_capture_start (W2-B). Returns true if
 * a non-expired grant exists for (srv, output). Expired entries are
 * reclaimed lazily. */
ISZ_INTERNAL bool isz_capture_check_consent(isz_server *srv, isz_output *output)
{
    if (!srv || !output)
        return false;
    struct isz_consent_entry *e = isz_consent_find(srv, output, false);
    if (!e || !e->used)
        return false;
    if (isz_consent_now_ns() >= e->expires_ns) {
        e->used = false;
        return false;
    }
    return true;
}
