# Ishizue tests

Integration tests for `libishizue.so` built with
`-DISHIZUE_ENABLE_TEST_HOOKS`. Run from the repo root:

    make test

## What is covered

`test_headless_basic` drives the headless backend (SPEC section 10)
through the section 4 test hooks, end to end and synchronously:

- `isz_init` with `ISZ_BACKEND_HEADLESS` and an explicit geometry.
  The backend creates one virtual output during init.
- `isz_output_list` confirms the init-time output. The
  `ISZ_EVENT_OUTPUT_ADD` for it fires inside `isz_init`, before a
  listener can be registered, so the listener registered later only
  catches the hotplug in step 9.
- `isz_output_get_modes` + `isz_output_enable` on the first mode.
- `isz_add_listener` for `ISZ_EVENT_OUTPUT_ADD` and
  `ISZ_EVENT_INPUT_KEYBOARD_KEY`.
- `isz_allowlist_add_binary` + `isz_test_connect` for a fake client
  that passes the section 6.3 trust check on `/bin/true`.
- `isz_test_send_key` injects `KEY_A` press; the listener checks the
  keycode and press state inline.
- `isz_test_simulate_output_hotplug` adds a 1920x1080 output; the
  `OUTPUT_ADD` listener fires and `isz_output_list` grows to 2.
- `isz_destroy` tears the server down. A `goto cleanup` in the test's
  `CHECK` macro guarantees this runs even on assertion failure.

No `isz_dispatch` call, no timers, no real input. Every hook returns
after the listener has fired.

## What is not covered

- Real DRM/KMS mode-sets and GPU scanout. The headless backend's
  `commit` is a no-op; nothing is paged-flipped.
- Real libinput devices. The headless seat exists but has no devices.
- Multiple simultaneous clients and the wire protocol. The test client
  injects events directly via `isz_server_emit_event`; no bytes cross
  the socketpair.
- The accept/handshake path (`isz_listen`, SPEC section 6.1/6.2).
- valgrind cleanliness. `isz_test_connect` allocates an `isz_conn` and
  a client fd that `isz_destroy` does not reclaim, because the test
  client is never added to `srv->clients`. Process exit reclaims them.
  A future `isz_test_client_destroy` would close the gap.

## Running by hand

    make -C tests
    make -C tests run

or, without make:

    cd tests
    cc -std=c11 -Wall -Wextra -Wpedantic -DISHIZUE_ENABLE_TEST_HOOKS \
       -I../include -I../src/input \
       test_headless_basic.c -o test_headless_basic \
       -L.. -lishizue
    LD_LIBRARY_PATH=.. ./test_headless_basic
