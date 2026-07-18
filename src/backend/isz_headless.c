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

/* isz_headless.c - concrete headless backend (SPEC §4, §10).
 *
 * Virtual outputs, no DRM, no GPU. commit() is a synchronous no-op that
 * immediately returns the backend to READY. read_events() returns at once.
 * ISZ_HEADLESS_WIDTH/HEIGHT/REFRESH env vars override the default geometry
 * once at init.
 *
 * The backend does not own real isz_output objects; it owns
 * isz_headless_output records and notifies the parent layer (the server)
 * via a hook when outputs come and go. The parent wraps each into a real
 * isz_output and emits ISZ_EVENT_OUTPUT_ADD/_REMOVE. This keeps the
 * backend decoupled from the still-unspecified struct isz_output layout.
 */
#include "isz_headless.h"
#include "isz_backend.h"
#include "isz_log_bridge.h"

#include <stdlib.h>
#include <string.h>

#define ISZ_HEADLESS_DEFAULT_WIDTH    1024u
#define ISZ_HEADLESS_DEFAULT_HEIGHT    768u
#define ISZ_HEADLESS_DEFAULT_REFRESH   60000u  /* mHz, 60 Hz */

#define ISZ_HEADLESS_NAME_MAX  (sizeof(((struct isz_headless_output *)0)->name))

struct isz_headless_state {
    struct isz_headless_output **outputs;
    size_t output_count;
    size_t output_cap;
    uint32_t next_id;

    uint32_t def_width;
    uint32_t def_height;
    uint32_t def_refresh_mhz;

    isz_headless_output_hook_fn hook_fn;
    void                       *hook_userdata;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static uint32_t parse_u32_env(const char *name, uint32_t def)
{
    const char *s = getenv(name);
    if (!s || !*s)
        return def;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v == 0 || v > 0xfffffffful)
        return def;
    return (uint32_t)v;
}

static void fill_info(struct isz_headless_output_info *info,
                      const struct isz_headless_output *o)
{
    info->id           = o->id;
    info->width        = o->width;
    info->height       = o->height;
    info->refresh_mhz  = o->refresh_mhz;
    info->enabled      = o->enabled;
    memcpy(info->name, o->name, sizeof(info->name));
}

static void fire_hook(struct isz_headless_state *st,
                      const struct isz_headless_output *o, bool added)
{
    if (!st->hook_fn)
        return;
    struct isz_headless_output_info info;
    fill_info(&info, o);
    st->hook_fn(st->hook_userdata, &info, added);
}

static int ensure_capacity(struct isz_headless_state *st, size_t need)
{
    if (need <= st->output_cap)
        return ISZ_OK;
    size_t ncap = st->output_cap ? st->output_cap * 2 : 4;
    while (ncap < need)
        ncap *= 2;
    struct isz_headless_output **n =
        realloc(st->outputs, ncap * sizeof(*n));
    if (!n)
        return ISZ_ERR_NO_MEMORY;
    st->outputs = n;
    st->output_cap = ncap;
    return ISZ_OK;
}

static int append_output(struct isz_headless_state *st,
                         uint32_t width, uint32_t height,
                         uint32_t refresh_mhz)
{
    int rc = ensure_capacity(st, st->output_count + 1);
    if (rc < 0)
        return rc;

    struct isz_headless_output *o = calloc(1, sizeof(*o));
    if (!o)
        return ISZ_ERR_NO_MEMORY;
    o->id          = ++st->next_id;
    o->width       = width;
    o->height      = height;
    o->refresh_mhz = refresh_mhz;
    o->enabled     = false;
    snprintf(o->name, ISZ_HEADLESS_NAME_MAX, "HEADLESS-%u",
             (unsigned)o->id);

    st->outputs[st->output_count++] = o;
    fire_hook(st, o, true);
    return ISZ_OK;
}

/* ------------------------------------------------------------------ */
/* Backend ops                                                        */
/* ------------------------------------------------------------------ */

static int isz_headless_init(struct isz_backend *self, void *config)
{
    struct isz_headless_state *st = calloc(1, sizeof(*st));
    if (!st)
        return ISZ_ERR_NO_MEMORY;
    self->impl = st;

    /* Env vars override the config defaults (SPEC §4). The config struct
     * itself is the lowest-priority source: a zero-valued field means
     * "use built-in default". */
    st->def_width       = ISZ_HEADLESS_DEFAULT_WIDTH;
    st->def_height      = ISZ_HEADLESS_DEFAULT_HEIGHT;
    st->def_refresh_mhz = ISZ_HEADLESS_DEFAULT_REFRESH;

    if (config) {
        const isz_headless_config *c = config;
        if (c->width)       st->def_width       = c->width;
        if (c->height)      st->def_height      = c->height;
        if (c->refresh_rate) st->def_refresh_mhz = c->refresh_rate;
    }

    st->def_width       = parse_u32_env("ISZ_HEADLESS_WIDTH",
                                        st->def_width);
    st->def_height      = parse_u32_env("ISZ_HEADLESS_HEIGHT",
                                        st->def_height);
    st->def_refresh_mhz = parse_u32_env("ISZ_HEADLESS_REFRESH",
                                        st->def_refresh_mhz);

    st->outputs     = NULL;
    st->output_count = 0;
    st->output_cap   = 0;
    st->next_id      = 0;
    st->hook_fn      = NULL;
    st->hook_userdata = NULL;

    isz_log_internal(ISZ_LOG_INFO,
                     "headless: init %ux%u@%u mHz",
                     (unsigned)st->def_width, (unsigned)st->def_height,
                     (unsigned)st->def_refresh_mhz);

    int rc = append_output(st, st->def_width, st->def_height,
                           st->def_refresh_mhz);
    if (rc < 0) {
        /* init owns its partial-state cleanup. */
        free(st->outputs);
        free(st);
        self->impl = NULL;
        return rc;
    }
    return ISZ_OK;
}

static int isz_headless_commit(struct isz_backend *self,
                               struct isz_output *out, uint32_t flags)
{
    (void)out;
    (void)flags;
    /* No real scanout. Surfaces attached to `out` would have their buffers
     * accepted by the surface layer but never paged-flipped; we have
     * nothing to wait on, so finish immediately. */
    isz_backend_finish_commit(self);
    return ISZ_OK;
}

static int isz_headless_read_events(struct isz_backend *self)
{
    (void)self;
    /* No fds to drain; vblanks don't exist. */
    return ISZ_OK;
}

static void isz_headless_destroy(struct isz_backend *self)
{
    if (!self->impl)
        return;
    struct isz_headless_state *st = self->impl;
    for (size_t i = 0; i < st->output_count; i++) {
        if (st->outputs[i])
            fire_hook(st, st->outputs[i], false);
        free(st->outputs[i]);
    }
    free(st->outputs);
    free(st);
    self->impl = NULL;
}

static void isz_headless_dump(const struct isz_backend *self, FILE *fp)
{
    const struct isz_headless_state *st = self->impl;
    if (!st)
        return;
    fprintf(fp, "  headless: default %ux%u@%u mHz, %zu output(s)\n",
            (unsigned)st->def_width, (unsigned)st->def_height,
            (unsigned)st->def_refresh_mhz, st->output_count);
    for (size_t i = 0; i < st->output_count; i++) {
        const struct isz_headless_output *o = st->outputs[i];
        fprintf(fp, "    [%s] id=%u %ux%u@%u mHz enabled=%d\n",
                o->name, (unsigned)o->id, (unsigned)o->width,
                (unsigned)o->height, (unsigned)o->refresh_mhz,
                (int)o->enabled);
    }
}

/* Headless owns no real CRTCs, so blanking is a no-op. */
static void isz_headless_blank_all_crtcs(struct isz_backend *self)
{
    (void)self;
}

static const struct isz_backend_ops isz_headless_ops = {
    .init            = isz_headless_init,
    .commit          = isz_headless_commit,
    .read_events     = isz_headless_read_events,
    .destroy         = isz_headless_destroy,
    .dump            = isz_headless_dump,
    .blank_all_crtcs = isz_headless_blank_all_crtcs,
};

const struct isz_backend_ops *isz_headless_get_ops(void)
{
    return &isz_headless_ops;
}

/* ------------------------------------------------------------------ */
/* Parent-layer glue                                                  */
/* ------------------------------------------------------------------ */

void isz_headless_set_output_hook(struct isz_backend *b,
                                  isz_headless_output_hook_fn fn,
                                  void *userdata)
{
    if (!b || b->type != ISZ_BACKEND_HEADLESS || !b->impl)
        return;
    struct isz_headless_state *st = b->impl;
    st->hook_fn = fn;
    st->hook_userdata = userdata;
}

int isz_headless_simulate_output_hotplug(struct isz_backend *b,
                                         uint32_t width, uint32_t height)
{
    if (!b || b->type != ISZ_BACKEND_HEADLESS || !b->impl)
        return ISZ_ERR_INVALID_ARG;
    if (width == 0 || height == 0)
        return ISZ_ERR_INVALID_ARG;
    struct isz_headless_state *st = b->impl;
    return append_output(st, width, height, st->def_refresh_mhz);
}

const struct isz_headless_output *const *
isz_headless_outputs(const struct isz_backend *b, size_t *count)
{
    if (count)
        *count = 0;
    if (!b || b->type != ISZ_BACKEND_HEADLESS || !b->impl)
        return NULL;
    const struct isz_headless_state *st = b->impl;
    if (count)
        *count = st->output_count;
    return (const struct isz_headless_output *const *)st->outputs;
}
