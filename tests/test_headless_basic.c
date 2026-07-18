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

/* test_headless_basic.c: headless backend integration test (W3-B).
 *
 * Drives the SPEC section 4 test hooks end to end against the section 10
 * headless backend: init, output enumeration, output enable, fake
 * client connect, key injection, output hotplug, teardown. Every test
 * hook is synchronous, so the test never calls isz_dispatch and never
 * sleeps on a timer or real input.
 *
 * API gap: isz_event is opaque in the public header and has no field
 * accessors. To read ev->type and ev->u.keyboard_key, this test
 * includes the internal input header (src/input/isz_seat_internal.h)
 * where struct isz_event is defined. A future minor version should
 * grow public accessors (isz_event_get_type,
 * isz_event_get_keyboard_keycode, isz_event_get_keyboard_pressed, ...)
 * so tests and Architects stay off internal headers. */

#define _POSIX_C_SOURCE 200809L

#include <ishizue/isz.h>

/* Pulls in the concrete struct isz_event layout. The header only
 * declares internal functions (isz_log_internal, isz_server_emit_event,
 * ...) which this test never calls, so there are no link-time
 * dependencies on hidden symbols. ISHIZUE_HAVE_LIBINPUT / _XKBCOMMON /
 * _LIBSEAT are not defined for this build, so the corresponding system
 * headers are not pulled in. */
#include "isz_seat_internal.h"

#include <linux/input-event-codes.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* CHECK: print the failure and jump to cleanup so isz_destroy always
 * runs. All locals are declared before the first CHECK so the jump
 * never crosses an initialization. */
#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
            goto cleanup; \
        } \
    } while (0)

/* Listener state. Zero-initialized, mutated inline by the callbacks. */
struct key_state {
    int      fired;
    uint32_t keycode;
    bool     pressed;
};

struct output_state {
    int fired;
};

static void on_key(void *ud, const isz_event *ev)
{
    struct key_state *s = ud;
    if (ev->type != ISZ_EVENT_INPUT_KEYBOARD_KEY)
        return;
    s->fired++;
    s->keycode  = ev->u.keyboard_key.keycode;
    s->pressed  = ev->u.keyboard_key.pressed;
}

static void on_output_add(void *ud, const isz_event *ev)
{
    struct output_state *s = ud;
    if (ev->type != ISZ_EVENT_OUTPUT_ADD)
        return;
    s->fired++;
}

int main(void)
{
    isz_server      *srv     = NULL;
    isz_test_client *tc      = NULL;
    isz_output     **outputs = NULL;
    isz_mode       **modes   = NULL;
    size_t           n       = 0;
    int              rc      = 0;
    int              ret     = 1;

    struct key_state    ks = {0};
    struct output_state os = {0};

    /* 1. Init with the headless backend at a known geometry. */
    isz_headless_config cfg = {
        .width        = 1024,
        .height       = 768,
        .refresh_rate = 60000,
    };
    srv = isz_init(ISZ_BACKEND_HEADLESS, &cfg);
    CHECK(srv != NULL, "isz_init returned NULL");

    /* 2. The headless backend creates one virtual output inside
     *    isz_init and fires ISZ_EVENT_OUTPUT_ADD before this test
     *    can register a listener. Verify the side effect (the output
     *    is in the list) here; the listener registered next catches
     *    the hotplugged second output in step 9. */
    n = 0;
    outputs = isz_output_list(srv, &n);
    CHECK(outputs != NULL && n == 1,
          "expected 1 output after init, got %zu", n);

    rc = isz_add_listener(srv, ISZ_EVENT_OUTPUT_ADD,
                          on_output_add, &os);
    CHECK(rc == ISZ_OK, "isz_add_listener(OUTPUT_ADD) rc=%d", rc);

    /* 3. Enable the output with its first (and, for headless, only)
     *    mode. isz_output_enable validates that the mode pointer
     *    belongs to the output. */
    n = 0;
    modes = isz_output_get_modes(outputs[0], &n);
    CHECK(modes != NULL && n >= 1, "expected >=1 mode, got %zu", n);
    rc = isz_output_enable(outputs[0], modes[0]);
    CHECK(rc == ISZ_OK, "isz_output_enable rc=%d", rc);

    /* 4. Keyboard key listener. */
    rc = isz_add_listener(srv, ISZ_EVENT_INPUT_KEYBOARD_KEY,
                          on_key, &ks);
    CHECK(rc == ISZ_OK, "isz_add_listener(KEY) rc=%d", rc);

    /* 5. Allowlist a real on-disk binary so isz_test_connect's
     *    section 6.3 trust check (inode/dev match) passes. */
    rc = isz_allowlist_add_binary(srv, "/bin/true");
    CHECK(rc == ISZ_OK, "isz_allowlist_add_binary rc=%d", rc);

    /* 6. Fake client connect. The hook stats /bin/true, matches it
     *    against the allowlist, creates a socketpair, hands one end
     *    to isz_conn_create, and emits CLIENT_CONNECT. */
    tc = isz_test_connect(srv, "/bin/true");
    CHECK(tc != NULL, "isz_test_connect returned NULL");

    /* 7. Inject a KEY_A press. isz_test_send_key is synchronous: it
     *    builds an isz_event and calls isz_server_emit_event, which
     *    walks the listener list and calls on_key before returning. */
    isz_test_send_key(tc, KEY_A, true);

    /* 8. Verify the listener saw the right keycode and press state. */
    CHECK(ks.fired == 1, "expected 1 key event, got %d", ks.fired);
    CHECK(ks.keycode == (uint32_t)KEY_A,
          "expected keycode %d, got %u", KEY_A, (unsigned)ks.keycode);
    CHECK(ks.pressed, "expected pressed=true");

    /* 9. Hotplug a second virtual output. Also synchronous: the
     *    backend fires its output hook, the server wraps the new
     *    output and emits OUTPUT_ADD, and on_output_add runs before
     *    isz_test_simulate_output_hotplug returns. */
    isz_test_simulate_output_hotplug(srv, 1920, 1080);

    /* 10. Verify the listener fired and the output list grew to 2. */
    CHECK(os.fired == 1,
          "expected 1 output_add event, got %d", os.fired);
    n = 0;
    outputs = isz_output_list(srv, &n);
    CHECK(outputs != NULL && n == 2,
          "expected 2 outputs after hotplug, got %zu", n);

    ret = 0;
    printf("test_headless_basic: PASS\n");

cleanup:
    /* isz_destroy tears down the server and its outputs, listeners,
     * allowlist, seat, and backend. The test client's isz_conn and
     * client_fd are not in srv->clients (isz_test_connect keeps them
     * out), so they are not reclaimed here; see tests/README.md. */
    if (srv)
        isz_destroy(srv);
    return ret;
}
