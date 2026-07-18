# Getting started with Ishizue

A minimal compositor built on Ishizue, end to end. The program
creates a Unix domain socket, hands it to the library, registers
listeners for the events a tiling WM needs to bootstrap, and runs
an epoll loop until SIGINT. It enables outputs as they appear and
prints keycodes as they arrive. It does no tiling, no focus policy,
no cursor rendering. That is the Architect's job; this is the
scaffold.

## Prerequisites

Build-time, installed via your distribution or from source:

- C11 compiler (cc, gcc, or clang)
- pkg-config
- libdrm dev headers
- libinput dev headers
- libseat dev headers
- libxkbcommon dev headers

Runtime, only when using the DRM backend on bare metal:

- A VT switched away from any running display server (Xorg, another
  compositor). The library calls `drmSetMaster()` at backend init;
  if another process holds master, `isz_init` returns NULL and the
  log callback reports `ISZ_ERR_DRM_MASTER`.
- Membership in the `video` group, or `CAP_SYS_ADMIN`, or root. The
  library does not raise or drop privileges itself.

For a first run, use the headless backend. It needs no privileges
and no KMS. The same program runs on DRM by changing one constant.

## Build and install the library

```
cd /path/to/Ishizue
make
make install   # defaults to /usr/local
ldconfig       # if installing into /usr/local
```

This produces `libishizue.so.1.1.0` plus the `libishizue.so` and
`libishizue.so.1` symlinks, and installs the public header to
`/usr/local/include/ishizue/isz.h`.

To override build-time limits (SPEC §4):

```
make ISZ_MAX_SURFACES_PER_CLIENT=128 ENABLE_HDR=0
```

## The example program

Save as `isz_compositor.c` next to the Ishizue source tree. The
internal header `isz_seat_internal.h` is needed because the public
API exposes `isz_event` as an opaque pointer with no field
accessors yet; a future minor version will add `isz_event_get_*`
getters and this include goes away.

```c
/* isz_compositor.c - minimal Ishizue compositor. */
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <ishizue/isz.h>
#include "isz_seat_internal.h"  /* concrete struct isz_event layout */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/tmp/ishizue-example.sock"

struct out_entry { isz_output *out; bool enabled; };
static struct out_entry g_outs[8];
static size_t g_n_outs;

static void enable_if_new(isz_server *srv)
{
    size_t n = 0;
    isz_output **list = isz_output_list(srv, &n);
    for (size_t i = 0; i < n; i++) {
        bool seen = false;
        for (size_t j = 0; j < g_n_outs; j++)
            if (g_outs[j].out == list[i]) { seen = true; break; }
        if (seen || g_n_outs >= 8) continue;
        size_t mn = 0;
        isz_mode **modes = isz_output_get_modes(list[i], &mn);
        if (!modes || mn == 0) continue;
        int rc = isz_output_enable(list[i], modes[0]);
        if (rc != ISZ_OK) {
            fprintf(stderr, "output_enable rc=%d (%s)\n",
                    rc, isz_strerror(rc));
            continue;
        }
        g_outs[g_n_outs].out = list[i];
        g_outs[g_n_outs].enabled = true;
        g_n_outs++;
        fprintf(stderr, "output enabled\n");
    }
}

static void on_output_add(void *ud, const isz_event *ev)
{
    if (ev->type != ISZ_EVENT_OUTPUT_ADD) return;
    enable_if_new(ud);
}

static void on_output_remove(void *ud, const isz_event *ev)
{
    (void)ud;
    if (ev->type != ISZ_EVENT_OUTPUT_REMOVE) return;
    fprintf(stderr, "output removed (wrapper stays valid until destroy)\n");
}

static void on_key(void *ud, const isz_event *ev)
{
    (void)ud;
    if (ev->type != ISZ_EVENT_INPUT_KEYBOARD_KEY) return;
    fprintf(stderr, "keycode=%u %s\n",
            (unsigned)ev->u.keyboard_key.keycode,
            ev->u.keyboard_key.pressed ? "down" : "up");
}

static void on_client(void *ud, const isz_event *ev)
{
    (void)ud;
    if (ev->type == ISZ_EVENT_CLIENT_CONNECT)
        fprintf(stderr, "client connected\n");
    else if (ev->type == ISZ_EVENT_CLIENT_DISCONNECT)
        fprintf(stderr, "client disconnected\n");
}

static int make_listen_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return -1; }
    unlink(SOCKET_PATH);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 16) < 0) {
        perror("listen"); close(fd); return -1;
    }
    chmod(SOCKET_PATH, 0600);
    return fd;
}

int main(void)
{
    int listen_fd = -1, sfd = -1, epfd = -1, ret = 1;
    isz_server *srv = NULL;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) { perror("sigprocmask"); goto cleanup; }
    sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0) { perror("signalfd"); goto cleanup; }

    listen_fd = make_listen_socket();
    if (listen_fd < 0) goto cleanup;

    /* Headless for the first run: no DRM master, no GPU, no real
     * input. Flip to ISZ_BACKEND_DRM with NULL config for a
     * bare-metal run on a free VT. */
    isz_headless_config cfg = { .width = 1280, .height = 720,
                                .refresh_rate = 60000 };
    srv = isz_init(ISZ_BACKEND_HEADLESS, &cfg);
    if (!srv) { fprintf(stderr, "isz_init failed\n"); goto cleanup; }

    /* The init-time OUTPUT_ADD fires inside isz_init before any
     * listener can register. Seed g_outs from isz_output_list so
     * on_output_add does not double-enable. */
    enable_if_new(srv);

    /* Empty allowlist means deny-all (SPEC §6.3). Allowlist
     * ourselves so clients can connect. */
    isz_allowlist_add_binary(srv, "/proc/self/exe");

    int rc = isz_listen(srv, listen_fd);
    if (rc != ISZ_OK) {
        fprintf(stderr, "isz_listen rc=%d (%s)\n", rc, isz_strerror(rc));
        goto cleanup;
    }
    listen_fd = -1;  /* library owns it from here */

    isz_add_listener(srv, ISZ_EVENT_OUTPUT_ADD,        on_output_add, srv);
    isz_add_listener(srv, ISZ_EVENT_OUTPUT_REMOVE,     on_output_remove, NULL);
    isz_add_listener(srv, ISZ_EVENT_INPUT_KEYBOARD_KEY, on_key, NULL);
    isz_add_listener(srv, ISZ_EVENT_CLIENT_CONNECT,    on_client, NULL);
    isz_add_listener(srv, ISZ_EVENT_CLIENT_DISCONNECT, on_client, NULL);

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); goto cleanup; }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = sfd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);

    /* isz_get_fds returns the long-lived fds (libinput, PSI) plus
     * current client sockets. The listen fd is in the library's own
     * epoll; we wake up on the long-lived ones and let the timeout
     * cover the rest. */
    int isz_fds[16];
    int n_fds = isz_get_fds(srv, isz_fds, 16);
    for (int i = 0; i < n_fds && i < 16; i++) {
        struct epoll_event iev = { .events = EPOLLIN, .data.fd = isz_fds[i] };
        epoll_ctl(epfd, EPOLL_CTL_ADD, isz_fds[i], &iev);
    }

    fprintf(stderr, "running on %s; ctrl-c to quit\n", SOCKET_PATH);
    for (;;) {
        /* 100 ms timeout so isz_dispatch gets a tick even when no
         * Architect-polled fd is ready (e.g. a client connecting
         * on headless, where the listen fd is library-internal). */
        struct epoll_event evs[8];
        int n = epoll_wait(epfd, evs, 8, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); break;
        }
        bool sigint = false;
        for (int i = 0; i < n; i++) {
            if (evs[i].data.fd == sfd) {
                struct signalfd_siginfo si;
                while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                    if (si.ssi_signo == SIGINT) sigint = true;
                }
            }
        }
        if (sigint) break;
        /* Drain the library's epoll. Non-blocking; cheap when
         * nothing is ready. */
        isz_dispatch(srv);
    }
    ret = 0;

cleanup:
    if (epfd >= 0) close(epfd);
    if (srv) isz_destroy(srv);
    if (listen_fd >= 0) close(listen_fd);
    if (sfd >= 0) close(sfd);
    unlink(SOCKET_PATH);
    return ret;
}
```

## Build the example

Assuming the Ishizue source tree is at `../Ishizue` relative to the
example file:

```
cc -std=c11 -Wall -Wextra -Wpedantic -O2 -g \
   -I/usr/local/include \
   -I../Ishizue/src/input \
   isz_compositor.c \
   -o isz_compositor \
   -L/usr/local/lib -lishizue \
   -Wl,-rpath,/usr/local/lib
```

The `-I../Ishizue/src/input` flag pulls in
`isz_seat_internal.h`. Once public `isz_event_get_*` accessors
land in a future minor version, drop the include and the `-I` flag.

## Run

```
./isz_compositor
```

Expected output on headless:

```
output enabled
running on /tmp/ishizue-example.sock; ctrl-c to quit
```

The library creates one virtual output inside `isz_init` at 1280x720
(per the config); the example enables it and then blocks in
`epoll_wait` waiting for SIGINT. Ctrl-C quits cleanly.

For a DRM run, change `ISZ_BACKEND_HEADLESS` to `ISZ_BACKEND_DRM`
and pass `NULL` as the config. Build the same way. Run from a free
VT (typically tty2 after `chvt 2`) with no other compositor
holding DRM master:

```
sudo ./isz_compositor     # or: setcap cap_sys_admin+ep ./isz_compositor
```

You should see `output enabled` for each connected display.

## Common pitfalls

**Allowlist deny-all.** If you do not call `isz_allowlist_add_binary`
or `isz_allowlist_add_cgroup` before clients connect, every
connection is closed before any handshake byte is sent (SPEC §6.3).
The example allowlists `/proc/self/exe` so any client binary that
is a copy of the compositor itself can connect. For real
deployments allowlist specific client binaries (your terminal, your
launcher) or a cgroup slice.

**DRM master race.** On a system with a running display server
(GNOME, KDE, sway), that server holds DRM master and `isz_init`
with `ISZ_BACKEND_DRM` fails. Either stop the other server or run
on a free VT. The library calls `drmSetMaster()` itself; it never
tries to steal master (SPEC §3).

**Headless env vars.** The headless backend reads three environment
variables at init (SPEC §4): `ISZ_HEADLESS_WIDTH`,
`ISZ_HEADLESS_HEIGHT`, `ISZ_HEADLESS_REFRESH` (in mHz). They
override the `isz_headless_config` fields. Useful for testing
specific geometries without recompiling.

**Socket path cleanup.** The library does not unlink the UDS path
on `isz_destroy`. The example unlinks it explicitly before `bind`
(to recover from a previous crash) and on cleanup. A production
compositor handles `SIGTERM` the same way; the example handles
only SIGINT for brevity.

**Init-time OUTPUT_ADD.** The library creates the headless default
output inside `isz_init`, which fires `ISZ_EVENT_OUTPUT_ADD`
before the caller can register a listener. The example calls
`enable_if_new(srv)` once after `isz_init` to catch the init
output, then registers the listener for hotplugs. The DRM wave
will likely defer the output enumeration to the first
`isz_dispatch` so the listener can see the initial outputs; until
then, the post-init `enable_if_new` is the workaround.

**Event struct access.** `isz_event` is opaque in `isz.h`. The
example includes the internal header
`isz_seat_internal.h` to read `ev->type` and
`ev->u.keyboard_key.{keycode,pressed}`. This is a known API gap;
the W3-B test worklog flagged it and a future minor version will
add `isz_event_get_*` accessors so callers stay off internal
headers.

## Next steps

`api.md` is the function-by-function reference. Read it for the
surface buffer attach/commit/release cycle (§8), the plane slot
model (§7.7), and the seat API.

`protocol.md` is the wire protocol reference. Read it if you are
writing a client library (the X11 bridge, a status bar, a
screenshot tool) or debugging a connection. The handshake, message
table, and byte-exact examples live there.

From here, the Architect's path is to add: a tiling layout
algorithm driven by `ISZ_EVENT_OUTPUT_ADD` / `_REMOVE` and
`ISZ_EVENT_CLIENT_CONNECT` / `_DISCONNECT`; a focus policy driven
by `ISZ_EVENT_INPUT_KEYBOARD_KEY` and
`isz_seat_set_keyboard_focus()`; plane slot allocation per
surface via `isz_output_get_plane_slots()` and
`isz_surface_set_plane_slot()`; and a render loop that calls
`isz_commit()` on vblank.
