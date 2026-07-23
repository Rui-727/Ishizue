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

/* isz_backend.c - generic backend lifecycle and state machine (SPEC §10).
 *
 * The generic layer:
 *   - dispatches isz_backend_create() to the right concrete backend by type
 *   - enforces the UNINITIALIZED -> READY -> COMMITTING -> READY state machine
 *   - propagates fatal errors into the ERROR state
 *   - never touches backend-private state (impl) directly
 *
 * Concrete backends own their own commit/read_events semantics inside ops;
 * the generic layer only walks the state transitions. */
#include "isz_backend.h"
#include "isz_log_bridge.h"

#include <stdlib.h>

struct isz_backend *isz_backend_create(enum isz_backend_type type, void *config)
{
    const struct isz_backend_ops *ops;
    switch (type) {
    case ISZ_BACKEND_DRM:
        ops = isz_drm_get_ops();
        break;
    case ISZ_BACKEND_HEADLESS:
        ops = isz_headless_get_ops();
        break;
    default:
        isz_log_internal(ISZ_LOG_ERROR,
                         "isz_backend_create: unknown backend type %d", (int)type);
        return NULL;
    }

    struct isz_backend *b = calloc(1, sizeof(*b));
    if (!b) {
        isz_log_internal(ISZ_LOG_ERROR, "isz_backend_create: out of memory");
        return NULL;
    }
    b->ops   = ops;
    b->type  = type;
    b->state = ISZ_BACKEND_UNINITIALIZED;
    b->last_error = ISZ_OK;
    b->impl   = NULL;

    int rc = ops->init(b, config);
    if (rc < 0) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "isz_backend_create: init failed (type=%d rc=%d)",
                         (int)type, rc);
        /* ops->init is responsible for freeing its own partial state and
         * leaving b->impl NULL on failure. */
        free(b);
        return NULL;
    }
    b->state = ISZ_BACKEND_READY;
    return b;
}

void isz_backend_destroy(struct isz_backend *b)
{
    if (!b)
        return;
    if (b->ops && b->ops->destroy)
        b->ops->destroy(b);
    free(b);
}

int isz_backend_commit(struct isz_backend *b, struct isz_output *out,
                       uint32_t flags)
{
    if (!b || !b->ops || !b->ops->commit)
        return ISZ_ERR_INVALID_ARG;

    switch (b->state) {
    case ISZ_BACKEND_UNINITIALIZED:
        return ISZ_ERR_INVALID_ARG;
    case ISZ_BACKEND_ERROR:
        return b->last_error != ISZ_OK ? b->last_error : ISZ_ERR_COMMIT_FAILED;
    case ISZ_BACKEND_COMMITTING:
        return ISZ_ERR_COMMIT_PENDING;
    case ISZ_BACKEND_READY:
        break;
    }

    b->state = ISZ_BACKEND_COMMITTING;
    int rc = b->ops->commit(b, out, flags);
    if (rc < 0) {
        /* A fatal backend will have set state = ERROR itself. Anything else
         * is a recoverable commit failure: fall back to READY so the
         * Architect can retry. */
        if (b->state == ISZ_BACKEND_COMMITTING)
            b->state = ISZ_BACKEND_READY;
        return rc;
    }
    /* Synchronous backends (headless) finish_commit() inside ops->commit,
     * leaving state at READY. Asynchronous backends stay at COMMITTING
     * until read_events() reports completion. */
    return ISZ_OK;
}

int isz_backend_read_events(struct isz_backend *b)
{
    if (!b || !b->ops || !b->ops->read_events)
        return ISZ_ERR_INVALID_ARG;
    if (b->state == ISZ_BACKEND_UNINITIALIZED)
        return ISZ_ERR_INVALID_ARG;

    int rc = b->ops->read_events(b);
    if (rc < 0) {
        /* Fatal: backend set ERROR itself. */
        return rc;
    }
    /* If the backend reported a finished commit via finish_commit during
     * read_events, state is already READY. Otherwise, an async backend may
     * still be COMMITTING; that's fine, leave it. */
    return ISZ_OK;
}

void isz_backend_finish_commit(struct isz_backend *b)
{
    if (b && b->state == ISZ_BACKEND_COMMITTING)
        b->state = ISZ_BACKEND_READY;
}

void isz_backend_set_error(struct isz_backend *b, int err)
{
    if (!b)
        return;
    b->state = ISZ_BACKEND_ERROR;
    b->last_error = err;
}

enum isz_backend_state isz_backend_state(const struct isz_backend *b)
{
    return b ? b->state : ISZ_BACKEND_UNINITIALIZED;
}

int isz_backend_last_error(const struct isz_backend *b)
{
    return b ? b->last_error : ISZ_OK;
}

void isz_backend_dump(const struct isz_backend *b, FILE *fp)
{
    if (!b || !fp)
        return;
    static const char *const names[] = {
        [ISZ_BACKEND_UNINITIALIZED] = "UNINITIALIZED",
        [ISZ_BACKEND_READY]         = "READY",
        [ISZ_BACKEND_COMMITTING]    = "COMMITTING",
        [ISZ_BACKEND_ERROR]         = "ERROR",
    };
    const char *s = (b->state < (int)(sizeof(names) / sizeof(names[0])) &&
                     names[b->state]) ? names[b->state] : "?";

    fprintf(fp, "backend: type=%d state=%s last_error=%d\n",
            (int)b->type, s, b->last_error);
    if (b->ops && b->ops->dump)
        b->ops->dump(b, fp);
}
