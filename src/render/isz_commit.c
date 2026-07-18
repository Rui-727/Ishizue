/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_commit.c -- atomic KMS commit path. SPEC 7.3 (frame scheduling),
 * 7.7 (plane slot model), 12 (rollback on failure).
 *
 * Flow:
 *   1. Early state check (COMMITTING -> ISZ_ERR_COMMIT_PENDING).
 *   2. Walk the global surface list, filter by output assignment.
 *   3. Pre-commit validation: plane_type set, plane_slot set, transform
 *      supported by the slot, format supported.
 *   4. Reserve slots via isz_plane_slot_assign.
 *   5. Hand off to isz_backend_commit (state machine + backend ops).
 *   6. On success: emit presented events, clear per-surface damage.
 *      On failure: release slots reserved in step 4, return the error.
 *
 * For the headless backend, ops->commit is a synchronous no-op that
 * calls isz_backend_finish_commit before returning, so state goes
 * READY -> COMMITTING -> READY within the call. The DRM atomic path is
 * behind ISHIZUE_HAVE_DRM. */

#define _POSIX_C_SOURCE 200809L

#include "isz_surface_internal.h"

#include <stdlib.h>
#include <time.h>

#include "../backend/isz_backend.h"
#include "../util/isz_log.h"
#include "../util/isz_list.h"

/* Track which slots this commit reserved, for rollback. */
struct commit_reservation {
    isz_surface *surf;
    int          slot_id;
    bool         newly_reserved;
};

static uint64_t now_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Walk the surface list, collecting surfaces bound to this output. */
static int collect_output_surfaces(isz_output *out,
                                   isz_surface ***out_arr, size_t *out_n)
{
    *out_arr = NULL;
    *out_n = 0;

    isz_list *list = isz_render_surface_list();
    if (!list) return 0;

    size_t cap = 8, n = 0;
    isz_surface **arr = malloc(cap * sizeof(*arr));
    if (!arr) return -1;

    isz_surface *s;
    isz_list_for_each_entry(s, list, isz_surface, server_node) {
        if (s->output != out)
            continue;
        if (n == cap) {
            cap *= 2;
            isz_surface **na = realloc(arr, cap * sizeof(*arr));
            if (!na) { free(arr); return -1; }
            arr = na;
        }
        arr[n++] = s;
    }

    *out_arr = arr;
    *out_n = n;
    return 0;
}

/* Pre-commit validation. Returns 0 or a negative isz_error. */
static int validate_surface(isz_surface *s, isz_output *out)
{
    if (!s->plane_type_set)
        return ISZ_ERR_INVALID_ARG;
    if (!s->plane_slot_set)
        return ISZ_ERR_SURFACE_NO_PLANE_SLOT;

    const struct isz_output_plane_slot *slot =
        isz_plane_slot_get(out, s->plane_slot);
    if (!slot)
        return ISZ_ERR_PLANE_UNAVAIL;
    if (slot->type != s->plane_type)
        return ISZ_ERR_PLANE_UNAVAIL;

    if (s->transform != ISZ_TRANSFORM_NORMAL && !slot->supports_transform)
        return ISZ_ERR_TRANSFORM_UNSUPPORTED;

    return ISZ_OK;
}

ISZ_API int isz_commit(isz_output *out, uint32_t flags)
{
    if (!out) return ISZ_ERR_INVALID_ARG;

    isz_server *srv = out->srv;
    if (!srv) return ISZ_ERR_INVALID_ARG;

    struct isz_backend *be = out->backend;
    if (!be) return ISZ_ERR_INVALID_ARG;

    /* Fast-path the COMMITTING check so we don't do surface iteration
     * work on a redundant call. isz_backend_commit re-checks. */
    if (isz_backend_state(be) == ISZ_BACKEND_COMMITTING)
        return ISZ_ERR_COMMIT_PENDING;

    /* SPEC 10: commits to a removed output are rejected. W2-A does not
     * yet mark removed outputs; skip until the lifecycle wave adds a
     * disconnected flag or accessor. */

    /* Gather surfaces bound to this output. */
    isz_surface **surfs = NULL;
    size_t nsurf = 0;
    if (collect_output_surfaces(out, &surfs, &nsurf) < 0)
        return ISZ_ERR_NO_MEMORY;

    struct commit_reservation *resv = NULL;
    if (nsurf > 0) {
        resv = calloc(nsurf, sizeof(*resv));
        if (!resv) {
            free(surfs);
            return ISZ_ERR_NO_MEMORY;
        }
    }

    /* Validate and reserve. */
    int rc = ISZ_OK;
    for (size_t i = 0; i < nsurf; i++) {
        isz_surface *s = surfs[i];
        rc = validate_surface(s, out);
        if (rc < 0) goto fail_resv;

        bool was_in_use = isz_plane_slot_held_by(out, s->plane_slot, s);
        rc = isz_plane_slot_assign(out, s);
        if (rc < 0) goto fail_resv;

        resv[i].surf = s;
        resv[i].slot_id = s->plane_slot;
        resv[i].newly_reserved = !was_in_use;
    }

    /* ASYNC only honored when tearing is enabled (7.3). No tearing
     * support is wired yet, so the backend receives the flags as-is;
     * the headless backend ignores them. When the DRM wave lands, the
     * ASYNC bit maps to DRM_MODE_PAGE_FLIP_ASYNC only if the output
     * has tearing enabled. */
    rc = isz_backend_commit(be, out, flags);
    if (rc < 0) {
        /* Backend already rolled back KMS state. Release only the slots
         * this commit newly reserved; pre-existing reservations persist
         * so the surface stays armed for the next attempt. */
        for (size_t i = 0; i < nsurf; i++)
            if (resv[i].newly_reserved)
                isz_plane_slot_release(out, resv[i].slot_id, resv[i].surf);
        goto fail_resv;
    }

    /* Success. Emit presented events and clear damage. TEST_ONLY
     * produces no visible frame, so skip presented events for it. */
    if (!(flags & ISZ_COMMIT_TEST_ONLY)) {
        uint64_t vblank_ns = now_monotonic_ns();
        for (size_t i = 0; i < nsurf; i++) {
            isz_surface *s = surfs[i];
            if (s->current)
                isz_render_send_presented(srv, s, vblank_ns);
            s->damage_count = 0;
        }
    }

    free(resv);
    free(surfs);
    return ISZ_OK;

fail_resv:
    free(resv);
    free(surfs);
    return rc;
}
