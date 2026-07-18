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

/* isz_backend.h - backend interface and state machine (internal, SPEC §10).
 *
 * Every backend (DRM, headless, nested) conforms to isz_backend_ops. The
 * generic layer in isz_backend.c owns the state machine and dispatches to
 * the concrete backend via ops. The struct isz_backend is owned by the
 * server (isz_server); the server layer obtains one via isz_backend_create()
 * at isz_init() time and releases it via isz_backend_destroy().
 *
 * State machine (SPEC §10):
 *
 *   UNINITIALIZED --init()--> READY
 *   READY --commit()--> COMMITTING --read_events()--> READY
 *   any --(fatal)--> ERROR (recovery: destroy + init again)
 *
 * commit() while COMMITTING returns ISZ_ERR_COMMIT_PENDING (no queuing).
 * commit() while ERROR returns the cached fatal error code.
 *
 * Synchronous backends (headless) call isz_backend_finish_commit() from
 * inside their ops->commit to skip the COMMITTING wait; the generic layer
 * then sees the state already back at READY by the time ops->commit returns.
 */
#ifndef ISZ_BACKEND_H
#define ISZ_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <ishizue/isz.h>

/* Forward declarations: the concrete output/surface/server structs are
 * defined elsewhere (output, surface, server layers). Backends operate on
 * opaque pointers; the generic layer passes them through unchanged. */
struct isz_backend;
struct isz_output;

enum isz_backend_state {
    ISZ_BACKEND_UNINITIALIZED = 0,
    ISZ_BACKEND_READY         = 1,
    ISZ_BACKEND_COMMITTING    = 2,
    ISZ_BACKEND_ERROR         = 3,
};

/* Backend operations. The generic layer calls these through the dispatch
 * table; concrete backends populate one static const instance.
 *
 * Contracts:
 *
 *   init(self, config)
 *     Allocate backend-private state, attach it to self->impl, probe
 *     hardware/resources. On failure, free any partial state, leave
 *     self->impl NULL, and return a negative isz_error. The generic layer
 *     will not call destroy() on a failed init.
 *
 *   commit(self, out, flags)
 *     Kick off (or synchronously complete) an atomic commit for the given
 *     output. flags is one of ISZ_COMMIT_NORMAL / _ASYNC / _TEST_ONLY.
 *     Returns 0 on success, negative isz_error on failure. A synchronous
 *     backend must call isz_backend_finish_commit(self) before returning 0.
 *     A fatal failure (DRM master lost, etc.) must call
 *     isz_backend_set_error(self, code) before returning.
 *
 *   read_events(self)
 *     Drain backend fds for one non-blocking iteration. For async backends,
 *     a completed page-flip transitions COMMITTING -> READY via
 *     isz_backend_finish_commit(). Returns 0 normally, negative isz_error
 *     on a fatal backend condition (after calling isz_backend_set_error).
 *
 *   destroy(self)
 *     Free self->impl and any resources it holds. Must be safe to call on
 *     a backend whose init() returned failure (impl == NULL). Does not
 *     free self; the generic layer does that.
 *
 *   dump(self, fp)
 *     Write a human-readable snapshot to fp for diagnostics. May be NULL.
 */
struct isz_backend_ops {
    int  (*init)(struct isz_backend *self, void *config);
    int  (*commit)(struct isz_backend *self, struct isz_output *out,
                   uint32_t flags);
    int  (*read_events)(struct isz_backend *self);
    void (*destroy)(struct isz_backend *self);
    void (*dump)(const struct isz_backend *self, FILE *fp);
};

struct isz_backend {
    const struct isz_backend_ops *ops;
    enum isz_backend_type   type;
    enum isz_backend_state  state;
    int                     last_error;   /* valid when state == ERROR */
    void                   *impl;         /* backend-private state */
};

/* Lifecycle. isz_backend_create() dispatches by type, calls ops->init, and
 * returns NULL on failure (with a log line). On success the backend is in
 * the READY state. */
struct isz_backend *isz_backend_create(enum isz_backend_type type,
                                       void *config);
void                isz_backend_destroy(struct isz_backend *b);

/* Frame loop. These enforce the state machine and call through to ops. */
int  isz_backend_commit(struct isz_backend *b, struct isz_output *out,
                        uint32_t flags);
int  isz_backend_read_events(struct isz_backend *b);

/* State helpers, called by concrete backends from inside their ops. */
void isz_backend_finish_commit(struct isz_backend *b);  /* COMMITTING -> READY */
void isz_backend_set_error(struct isz_backend *b, int err); /* any -> ERROR */

/* Accessors. */
enum isz_backend_state isz_backend_state(const struct isz_backend *b);
int  isz_backend_last_error(const struct isz_backend *b);
void isz_backend_dump(const struct isz_backend *b, FILE *fp);

/* Per-backend ops accessors. Each concrete backend exposes a static const
 * ops table through one of these. */
const struct isz_backend_ops *isz_drm_get_ops(void);
const struct isz_backend_ops *isz_headless_get_ops(void);
const struct isz_backend_ops *isz_nested_get_ops(void);

#endif /* ISZ_BACKEND_H */
