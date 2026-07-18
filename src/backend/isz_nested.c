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

/* isz_nested.c - nested backend stub (SPEC §10, post-v1).
 *
 * Same pattern as the DRM stub: wire the ops table, fail init with
 * ISZ_ERR_FEATURE_UNAVAIL so the build resolves. Real implementation
 * (parent-window surface, translation of input/output events to/from a
 * host compositor) comes later. */
#include "isz_nested.h"
#include "isz_backend.h"
#include "isz_log_bridge.h"

#include <ishizue/isz.h>

static int isz_nested_init(struct isz_backend *self, void *config)
{
    (void)self;
    (void)config;
    isz_log_internal(ISZ_LOG_ERROR, "Nested backend not yet implemented");
    return ISZ_ERR_FEATURE_UNAVAIL;
}

static int isz_nested_commit(struct isz_backend *self,
                             struct isz_output *out, uint32_t flags)
{
    (void)self; (void)out; (void)flags;
    return ISZ_ERR_FEATURE_UNAVAIL;
}

static int isz_nested_read_events(struct isz_backend *self)
{
    (void)self;
    return ISZ_ERR_FEATURE_UNAVAIL;
}

static void isz_nested_destroy(struct isz_backend *self)
{
    (void)self;
}

static void isz_nested_dump(const struct isz_backend *self, FILE *fp)
{
    (void)self;
    fprintf(fp, "  nested: stub (not yet implemented)\n");
}

static void isz_nested_blank_all_crtcs(struct isz_backend *self)
{
    (void)self;
}

static const struct isz_backend_ops isz_nested_ops = {
    .init            = isz_nested_init,
    .commit          = isz_nested_commit,
    .read_events     = isz_nested_read_events,
    .destroy         = isz_nested_destroy,
    .dump            = isz_nested_dump,
    .blank_all_crtcs = isz_nested_blank_all_crtcs,
};

const struct isz_backend_ops *isz_nested_get_ops(void)
{
    return &isz_nested_ops;
}
