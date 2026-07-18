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

/* isz_drm_event.h -- drmHandleEvent dispatcher, SPEC 7.3 / 10.
 *
 * Drain the DRM fd for one non-blocking iteration. Page-flip events
 * transition the backend out of COMMITTING via isz_backend_finish_commit.
 * Hotplug events (DRM_EVENT_CONNECTOR) trigger connector re-enumeration
 * and fire the output hook so the server layer can wrap/unwrap isz_output
 * objects and emit OUTPUT_ADD / OUTPUT_REMOVE. */
#ifndef ISZ_BACKEND_DRM_EVENT_H
#define ISZ_BACKEND_DRM_EVENT_H

#include "isz_backend.h"

#ifdef ISHIZUE_HAVE_DRM

struct isz_drm_state;
struct isz_backend;

/* Read and dispatch DRM events from st->drm_fd. Returns 0 on success,
 * negative isz_error on a fatal backend condition (sets the backend to
 * ERROR via isz_backend_set_error). */
int isz_drm_event_dispatch(struct isz_drm_state *st,
                           struct isz_backend *backend);

#endif /* ISHIZUE_HAVE_DRM */

#endif /* ISZ_BACKEND_DRM_EVENT_H */
