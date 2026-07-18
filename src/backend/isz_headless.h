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

/* isz_headless.h - concrete headless backend (SPEC §4, §10).
 *
 * The headless backend is the test/CI path: virtual outputs, no real
 * DRM/KMS, no GPU. commit() is a synchronous no-op; read_events() returns
 * immediately. Buffers attached to surfaces on a headless output are
 * accepted (the surface layer will call through) but never scanned out.
 *
 * Env vars (read once at init, SPEC §4):
 *   ISZ_HEADLESS_WIDTH    default 1024
 *   ISZ_HEADLESS_HEIGHT   default 768
 *   ISZ_HEADLESS_REFRESH  default 60000  (mHz, matching isz_headless_config)
 *
 * Output lifecycle: init() creates one virtual output at the configured
 * default geometry. isz_test_simulate_output_hotplug (test builds only)
 * creates more. Outputs are exposed to the parent layer via a hook; the
 * parent wraps each into a real isz_output and emits ISZ_EVENT_OUTPUT_ADD.
 */
#ifndef ISZ_BACKEND_HEADLESS_H
#define ISZ_BACKEND_HEADLESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "isz_backend.h"

struct isz_headless_output {
    uint32_t id;            /* monotonic, unique within this backend */
    uint32_t width;
    uint32_t height;
    uint32_t refresh_mhz;
    bool     enabled;
    char     name[32];
};

/* Snapshot handed to the parent-layer hook. Mirrors isz_headless_output
 * field-for-field; the parent sees a copy so the backend can free the
 * original on destroy without dangling pointers. */
struct isz_headless_output_info {
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_mhz;
    bool     enabled;
    char     name[32];
};

/* Parent-layer glue. Register once after isz_backend_create(); called
 * whenever a headless output is added or removed. added=true means a new
 * output appeared (the parent should wrap it and fire
 * ISZ_EVENT_OUTPUT_ADD); added=false means it was torn down. */
typedef void (*isz_headless_output_hook_fn)(void *userdata,
                                           const struct isz_headless_output_info *info,
                                           bool added);

void isz_headless_set_output_hook(struct isz_backend *b,
                                  isz_headless_output_hook_fn fn,
                                  void *userdata);

/* Test hook (SPEC §4, ISHIZUE_ENABLE_TEST_HOOKS only). Creates a new
 * virtual output at the given geometry (refresh from backend defaults)
 * and fires the output hook. Returns 0 on success, negative isz_error
 * on allocation failure. The public isz_test_simulate_output_hotplug()
 * entry in isz.h is implemented by the server layer, which looks up the
 * server's backend and calls this helper. */
int isz_headless_simulate_output_hotplug(struct isz_backend *b,
                                         uint32_t width, uint32_t height);

/* Read-only access to the backend's output list. The returned pointer is
 * owned by the backend and valid only until the next backend call. */
const struct isz_headless_output *const *
isz_headless_outputs(const struct isz_backend *b, size_t *count);

#endif /* ISZ_BACKEND_HEADLESS_H */
