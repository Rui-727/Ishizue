# wlroots Xwayland integration patterns

Research note for Ishizue (W6-C). wlroots is the closest production-grade
analogue to what Ishizue wants to be: a mechanism-only library that an
Architect links to build a compositor. wlroots has shipped Xwayland
support for years. This document maps what wlroots actually does, what
works, and where it carries Wayland-specific workarounds that Ishizue
does not need.

All wlroots source quoted below is from the master branch as fetched
from the live-clones mirror on GitHub [1] (the canonical repo is on
gitlab.freedesktop.org [2], but it is behind an anti-bot challenge and
the GitHub mirror tracks it minute-by-minute). Line numbers refer to
the fetched snapshot.

## 1. wlroots architecture overview

wlroots bills itself as "pluggable, composable, unopinionated modules
for building a Wayland compositor; or about 60,000 lines of code you
were going to write anyway" [6]. The contract is exactly the same as
Ishizue's SPEC §1: the library owns mechanism, the linking program owns
policy. Stacking order, focus policy, hotkeys, tiling layout, and
render-loop pacing are all out of library scope [6].

What wlroots provides that is relevant here:

- Backends (DRM/KMS, libinput, Wayland nested, X11 nested, headless).
- A renderer abstraction (GLES2, Vulkan, pixman) that the compositor
  can drive directly or extend.
- Buffer and allocator abstractions (shm, dma-buf, GBM).
- Wayland protocol implementations, both core (wl_compositor,
  wl_shell replaced by xdg-shell, wl_seat, wl_subsurface) and
  extensions (wlr-layer-shell, wlr-foreign-toplevel, ext-session-lock,
  ext-idle-notify, xdg-output, relative-pointer, pointer-constraints,
  and dozens more).
- A `wlr_xwayland` module that bundles an Xwayland launcher, an
  xwayland-shell-v1 implementation, and an in-process X11 window
  manager (the XWM) [6,7].

How wlroots differs from Weston, Mutter, and KWin: Weston is the
reference compositor that ships with Wayland itself, it is a single
binary with its own policy. Mutter (GNOME) and KWin (KDE) are
compositor binaries tied to specific desktop shells. wlroots
explicitly is none of those things: it is a `.so` and a set of headers
with no policy, no shell, no default keybindings. Sway, Hyprland,
Cage, Labwc, River, Wayfire, Niri, and many others link wlroots and
supply their own policy.

Ishizue is structurally similar: a `.so` (libishizue.so), an
`isz_`-prefixed public API, a stable ABI from day one, no policy in
the library, an Architect (the linking program) that owns focus,
stacking, and tiling decisions (SPEC §1, §2). The main structural
difference is the wire protocol: wlroots speaks Wayland; Ishizue
speaks its own custom protocol (SPEC §6). This changes which workarounds
Ishizue needs.

## 2. The wlr_xwayland object

The top-level handle is `struct wlr_xwayland`, defined in
`include/wlr/xwayland/xwayland.h` [5]:

```c
struct wlr_xwayland {
    struct wlr_xwayland_server *server;
    bool own_server;
    struct wlr_xwm *xwm;
    struct wlr_xwayland_shell_v1 *shell_v1;

    const char *display_name;            // ":0", ":1", ... to set DISPLAY to

    struct wl_display *wl_display;
    struct wlr_compositor *compositor;
    struct wlr_seat *seat;

    struct {
        struct wl_signal destroy;
        struct wl_signal ready;
        struct wl_signal new_surface;    // struct wlr_xwayland_surface
        struct wl_signal remove_startup_info;
    } events;

    bool (*user_event_handler)(struct wlr_xwayland *wlr_xwayland,
                               xcb_generic_event_t *event);
    void *data;
    /* WLR_PRIVATE fields follow: cursor_buffer, hotspots, listeners */
};
```

The struct composes three sub-objects:

- `server` is a `wlr_xwayland_server`: the process launcher and
  lifecycle owner for the Xwayland binary. Lives in
  `include/wlr/xwayland/server.h`.
- `shell_v1` is a `wlr_xwayland_shell_v1`: the Wayland global that
  implements the xwayland-shell-v1 protocol. Lives in
  `include/wlr/xwayland/shell.h`.
- `xwm` is the in-process X11 window manager (`struct wlr_xwm`,
  private, in `include/xwayland/xwm.h`).

The compositor typically constructs this in its setup phase. The
public entry points are:

```c
struct wlr_xwayland *wlr_xwayland_create(struct wl_display *wl_display,
        struct wlr_compositor *compositor, bool lazy);
struct wlr_xwayland *wlr_xwayland_create_with_server(
        struct wl_display *display, struct wlr_compositor *compositor,
        struct wlr_xwayland_server *server);
void wlr_xwayland_destroy(struct wlr_xwayland *wlr_xwayland);
```

`wlr_xwayland_create` does both jobs: it allocates a server (with
`lazy` and a 10-second terminate-delay when lazy), allocates a
shell_v1 global with version 1, and binds them together
(`xwayland/xwayland.c:137-172`) [5]. `wlr_xwayland_create_with_server`
is the escape hatch for compositors that want to share or pre-spawn
the Xwayland process. `own_server` tracks which path was used so
destroy knows whether to reap the server [5].

Lifecycle wiring: `wlr_xwayland` registers `wl_listener`s on the
server's `start`, `ready`, and `destroy` signals (`xwayland.c:119-128`)
[5]. On `start`, it tells `shell_v1` which `wl_client` is the Xwayland
client so the shell can reject bind requests from anyone else
(`xwayland.c:26-32`). On `ready`, it spawns the XWM (`xwayland.c:34-54`,
see section 7). On `destroy`, it tears the whole stack down in order.

The compositor listens to `events.new_surface` to learn about each
new X11 top-level, and to `events.ready` to start applying focus
policy. `display_name` is what the compositor must export to
`$DISPLAY` so that X11 clients connect to the right Xwayland
(`include/wlr/xwayland/xwayland.h` comment, [5]).

## 3. Process spawning

Xwayland is a separate binary, not a library. wlroots finds it via
`WLR_XWAYLAND` env var or falls back to the build-time `XWAYLAND_PATH`
(`xwayland/server.c:124-130`) [5]. The spawn flow is:

1. Allocate an X display number and the listening sockets
   (`xwayland/server.c:308-322`, calling `open_display_sockets` in
   `xwayland/sockets.c:149-205`).
2. Create a `socketpair(AF_UNIX, SOCK_STREAM)` for the Wayland
   connection (`server.c:325-335`). This becomes `wl_fd[0]` (parent
   side) and `wl_fd[1]` (Xwayland side).
3. If `enable_wm` is set (default true), create a second socketpair
   for the XWM channel: `wm_fd[0]` (parent) and `wm_fd[1]`
   (Xwayland). The XWM uses this as its X11 transport, separate from
   the X11 transport that ordinary X clients use (`server.c:336-348`).
4. Adopt `wl_fd[0]` into libwayland as the Xwayland wl_client via
   `wl_client_create(server->wl_display, server->wl_fd[0])`
   (`server.c:352-357`). Once this call returns, `wl_fd[0]` belongs
   to libwayland.
5. Create a `pipe(notify_fd)` and register a wl_event_loop fd source
   on `notify_fd[0]` for `WL_EVENT_READABLE`, with callback
   `xserver_handle_ready` (`server.c:364-380`).
6. `fork()` once, then in the child `fork()` again so the
   grandchild becomes Xwayland and the intermediate child exits
   immediately (`server.c:384-401`). The double-fork means init
   reaps the intermediate child and the compositor never has to
   waitpid Xwayland directly.
7. The grandchild calls `exec_xwayland(server, notify_fd[1])`
   (`server.c:27-138`), which builds the argv and `execvp`s the
   Xwayland binary.

The Xwayland argv wlroots constructs (`server.c:40-100`) [5]:

```
Xwayland :N -rootless -core -terminate [delay]
    -listenfd <fd0> -listenfd <fd1>   (or -listen on older Xwayland)
    -displayfd <notify_fd>
    -wm <wm_fd1>
    [-noTouchPointerEmulation] [-force-xrandr-emulation]
```

`-displayfd <fd>` is the readiness signal. Xwayland writes its
chosen display number as ASCII followed by newline to that fd once
it has finished initial setup. wlroots reads it in
`xserver_handle_ready` (`server.c:237-306`):

```c
if (mask & WL_EVENT_READABLE) {
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno != EINTR) { ...; mask = 0; }
    else if (n <= 0 || buf[n-1] != '\n') {
        return 1;   // recheck, more to read
    }
}
...
waitpid(server->pid, NULL, 0);
...
server->ready = true;
wl_signal_emit_mutable(&server->events.ready, &event);
```

Two details matter. First, wlroots waits for the newline because
Xwayland writes to the fd twice during startup and an early close
would cause the second write to fail and kill Xwayland
(`server.c:241-247` comment). Second, wlroots `waitpid`s the Xwayland
pid even though it double-forked, as a defence against host
applications that install their own SIGCHLD handler and steal the
reap (`server.c:257-274` comment).

`-displayfd` replaced the older SIGUSR1 readiness signal. wlroots
no longer uses SIGUSR1 for readiness; the comment in
`xserver_handle_ready` references the displayfd mechanism
exclusively [5]. The X server historically raised SIGUSR1 to its
parent on completion of initial setup. With `-displayfd` the parent
just reads from a pipe. That is race-free and works under the
double-fork. wlroots only handles SIGUSR1 indirectly through the X
server's own signal disposition.

Environment setup is minimal. The compositor must export `DISPLAY`
(it gets `display_name` from the wlr_xwayland struct). wlroots
itself only sets `WAYLAND_SOCKET` in the child before exec, pointing
it at `wl_fd[1]` (`server.c:104-106`):

```c
char wayland_socket_str[16];
snprintf(wayland_socket_str, sizeof(wayland_socket_str),
         "%d", server->wl_fd[1]);
setenv("WAYLAND_SOCKET", wayland_socket_str, true);
```

`XDG_RUNTIME_DIR` is the compositor's responsibility, not wlroots',
because libwayland already requires it for its own socket and the
compositor has already arranged it by the time `wlr_xwayland_create`
is called.

Display socket allocation lives in `xwayland/sockets.c`. wlroots
walks display numbers 0 to 32, takes an exclusive lock on
`/tmp/.X%d-lock`, and if successful opens both the Linux abstract
namespace socket (`\0/tmp/.X11-unix/X%d`) and the filesystem socket
(`/tmp/.X11-unix/X%d`) (`sockets.c:97-132`). It validates the
`/tmp/.X11-unix` directory: must be owned by root or the compositor
uid, must have the sticky bit set or be group/other read-only
(`sockets.c:71-95`). If a lock file exists for a dead PID, wlroots
unlinks it and retries (`sockets.c:172-196`).

Lazy mode is opt-in: the compositor passes `lazy=true` to
`wlr_xwayland_create`. In lazy mode wlroots opens the X display
sockets but does not spawn Xwayland. It registers wl_event_loop fd
sources on both `x_fd[0]` and `x_fd[1]` (`server.c:425-441`). When
the first X11 client connects to either socket, the callback fires
and `server_start()` is called, spawning Xwayland. The terminate
delay then lets Xwayland self-terminate after 10 seconds with no
clients, and wlroots re-arms the lazy watcher for the next connect.

## 4. The wlroots-Xwayland Wayland connection

Xwayland is a Wayland client. wlroots makes it a special one. The
key call in `server_start` (`server.c:352`) [5]:

```c
server->client = wl_client_create(server->wl_display, server->wl_fd[0]);
```

`wl_client_create` is libwayland-server's API for adopting a
pre-connected socket fd as a wl_client. It bypasses the normal
`wl_display` accept path. The compositor gets a `wl_client *` it can
recognise as Xwayland and grant privileges to.

Why this matters: Wayland is a capability system. Globals are
advertised to all clients via wl_registry, but the compositor can
filter them per-client with `wl_display_set_global_filter`. wlroots
uses this to expose `xwayland_shell_v1` only to the Xwayland client.
The shell's bind function (`xwayland/shell.c:139-156`) [5]
double-checks:

```c
static void shell_bind(struct wl_client *client, void *data,
                       uint32_t version, uint32_t id) {
    struct wlr_xwayland_shell_v1 *shell = data;
    if (client != shell->client) {
        wl_client_post_implementation_error(client,
            "Permission denied to bind to %s",
            xwayland_shell_v1_interface.name);
        return;
    }
    ...
}
```

The `shell->client` is set once, in `handle_server_start`
(`xwayland.c:26-32`), to `xwayland->server->client`. From that
point the global is effectively private to Xwayland. The
`include/wlr/xwayland/xwayland.h` doc comment tells compositors to
do the same filter via `wl_display_set_global_filter()` for full
defence in depth [5].

Other than xwayland-shell-v1, Xwayland is a normal wl_client. It
binds wl_compositor, wl_shm, wl_drm (or linux-dmabuf), wl_seat,
wl_output, and uses them like any other client. wlroots does not
grant it elevated buffer import rights or anything similar.

When Xwayland disconnects, `handle_client_destroy` fires
(`server.c:198-222`). If Xwayland was up for less than 5 seconds
the compositor assumes a startup failure and does not restart. If
it ran for more than 5 seconds, wlroots restarts it (lazy or
non-lazy depending on options). The 5-second heuristic is the only
crash-restart policy in the library; otherwise restart decisions
belong to the compositor.

## 5. xwayland-shell-v1 in wlroots

xwayland-shell-v1 is the Wayland-protocol side of the X11-to-Wayland
surface association. The protocol description on the Wayland Explorer
[3] explains the historical problem it solves:

> Before this protocol, this would be done via the Xwayland server
> providing the wl_surface's resource id via a client message with
> the WL_SURFACE_ID atom on the X window. This was problematic as
> a race could occur if the wl_surface associated with a WL_SURFACE_ID
> for a window was destroyed before the client message was processed
> by the compositor and another surface (or other object) had taken
> its id due to recycling.

The fix moves association to the Wayland side. Xwayland creates a
wl_surface, then creates an xwayland_surface_v1 object for it via
`xwayland_shell_v1.get_xwayland_surface`, then calls
`xwayland_surface_v1.set_serial(lo, hi)` with a 64-bit serial it
also broadcasts as the WL_SURFACE_SERIAL client message on the X11
window. The compositor matches the two. Because the surface role
is bound to the wl_surface's lifetime on the Wayland side, no
recycling race is possible. The serial pairs the two timelines
without reintroducing the race on the X11 side [3].

wlroots implements the server side in `xwayland/shell.c` [5]. The
shell global is created by `wlr_xwayland_shell_v1_create`
(`shell.c:164-191`):

```c
shell->global = wl_global_create(display, &xwayland_shell_v1_interface,
                                 version, shell, shell_bind);
```

The `get_xwayland_surface` request handler
(`shell.c:97-132`) [5]:

```c
static void shell_handle_get_xwayland_surface(struct wl_client *client,
        struct wl_resource *shell_resource, uint32_t id,
        struct wl_resource *surface_resource) {
    ...
    if (!wlr_surface_set_role(surface, &xwl_surface_role,
                    shell_resource, XWAYLAND_SHELL_V1_ERROR_ROLE)) {
        free(xwl_surface);
        return;
    }
    xwl_surface->surface = surface;
    xwl_surface->shell = shell;
    ...
    wl_list_insert(&shell->surfaces, &xwl_surface->link);
    wlr_surface_set_role_object(surface, xwl_surface->resource);
}
```

The role's commit hook (`xwl_surface_role_commit`, `shell.c:46-57`)
is where the new_surface signal fires. The serial set via
`set_serial` is stored on `xwl_surface->serial` as a uint64
(`shell.c:74-90`). When the XWM receives a WL_SURFACE_SERIAL client
message from Xwayland (section 7), it looks up the matching
`xwl_surface->serial` in the shell's surface list
(`xwm.c:1504-1531`) and calls `xwayland_surface_associate` to bind
the `wlr_xwayland_surface` to the `wlr_surface` [5].

The protocol still allows `WL_SURFACE_ID` as a fallback for old
Xwayland binaries. wlroots implements both paths
(`xwm_handle_surface_id_message` at `xwm.c:1465-1502` and
`xwm_handle_surface_serial_message` at `xwm.c:1504-1531`) [5].
The comment at `xwm.c:1482-1484` notes the ordering problem the
shell_v1 path fixes:

```c
// Because the X11 and Wayland connections are separate sockets, the
// WL_SURFACE_ID and wl_compositor.create_surface messages may be
// received in any order.
```

With shell_v1 the surface-creation message and the serial-binding
message both arrive on the Wayland socket, in order, tied to the
wl_surface's lifetime. The race is gone.

## 6. wlr_xwayland_surface

The per-window struct lives in `include/wlr/xwayland/xwayland.h`
[5]:

```c
struct wlr_xwayland_surface {
    xcb_window_t window_id;
    struct wlr_xwm *xwm;
    uint32_t surface_id;        // legacy WL_SURFACE_ID, 0 once paired
    uint64_t serial;            // WL_SURFACE_SERIAL, 0 once paired

    struct wl_list link;        // xwm->surfaces, creation order
    struct wl_list stack_link;  // xwm->surfaces_in_stack_order
    struct wl_list unpaired_link;

    struct wlr_surface *surface;
    struct wlr_addon surface_addon;

    int16_t x, y;
    uint16_t width, height;
    bool override_redirect;
    float opacity;

    char *title;
    char *class;
    char *instance;
    char *role;
    char *startup_id;
    pid_t pid;

    struct wl_list children;
    struct wlr_xwayland_surface *parent;
    struct wl_list parent_link;

    xcb_atom_t *window_type;
    size_t window_type_len;
    xcb_atom_t *protocols;
    size_t protocols_len;

    uint32_t decorations;
    xcb_icccm_wm_hints_t *hints;
    xcb_size_hints_t *size_hints;
    xcb_ewmh_wm_strut_partial_t *strut_partial;

    bool pinging;
    struct wl_event_source *ping_timer;

    // _NET_WM_STATE bits, one bool per atom
    bool modal, fullscreen, maximized_vert, maximized_horz;
    bool minimized, withdrawn, sticky, shaded;
    bool skip_taskbar, skip_pager, above, below, demands_attention;

    bool has_alpha;             // depth == 32

    struct { /* signals: destroy, request_*, associate, set_*, ... */ } events;
    void *data;
    /* WLR_PRIVATE: wm_name, net_wm_name, surface_commit/map/unmap listeners */
};
```

The fields map onto X11 properties nearly 1:1:

| Field | X11 source |
|---|---|
| `window_id` | X11 window XID (CreateNotify) |
| `x, y, width, height` | ConfigureNotify, initial from CreateNotify |
| `override_redirect` | CreateNotify.override_redirect, ConfigureNotify |
| `title` | `_NET_WM_NAME` (UTF8_STRING) preferred, else `WM_NAME` (STRING) |
| `class`, `instance` | `WM_CLASS` (two NUL-separated strings) |
| `role` | `WM_WINDOW_ROLE` |
| `startup_id` | `_NET_STARTUP_ID` |
| `pid` | X-Resource extension `xcb_res_query_client_ids`, not `_NET_WM_PID` |
| `parent` | `WM_TRANSIENT_FOR` |
| `window_type` | `_NET_WM_WINDOW_TYPE` (atom list) |
| `protocols` | `WM_PROTOCOLS` (atom list) |
| `hints` | `WM_HINTS` (parsed via `xcb_icccm_get_wm_hints_from_reply`) |
| `size_hints` | `WM_NORMAL_HINTS` (parsed via `xcb_icccm_get_wm_size_hints_from_reply`) |
| `decorations` | `_MOTIF_WM_HINTS` decorations field |
| `strut_partial` | `_NET_WM_STRUT_PARTIAL` |
| `modal, fullscreen, ...` | `_NET_WM_STATE` atom list, decoded |
| `has_alpha` | xcb_get_geometry reply, depth == 32 |
| `opacity` | `_NET_WM_WINDOW_OPACITY` (CARDINAL, scaled by UINT32_MAX) |

Note the `_NET_WM_PID` ignore: wlroots reads the PID through the
X-Resource extension (`xcb_res_query_client_ids`) instead, because
`_NET_WM_PID` is set by the client and can lie. See
`read_surface_client_id` (`xwm.c:149-175`) and the `xres` setup in
`xwm_get_resources` (`xwm.c:2359-2380`) [5]. If `xres` is older than
1.2, wlroots falls back to having no PID.

The fields are populated by `xwayland_surface_associate`
(`xwm.c:1186-1240`), which fires a burst of `xcb_get_property`
requests for the full property list and dispatches each reply to
the matching `read_surface_*` function. The list of properties
read on associate (`xwm.c:1207-1221`) [5]:

```c
const xcb_atom_t props[] = {
    XCB_ATOM_WM_CLASS,
    XCB_ATOM_WM_NAME,
    XCB_ATOM_WM_TRANSIENT_FOR,
    xwm->atoms[WM_PROTOCOLS],
    xwm->atoms[WM_HINTS],
    xwm->atoms[WM_NORMAL_HINTS],
    xwm->atoms[MOTIF_WM_HINTS],
    xwm->atoms[NET_STARTUP_ID],
    xwm->atoms[NET_WM_STATE],
    xwm->atoms[NET_WM_STRUT_PARTIAL],
    xwm->atoms[NET_WM_WINDOW_TYPE],
    xwm->atoms[NET_WM_NAME],
    xwm->atoms[NET_WM_ICON],
};
```

Each `read_surface_*` function re-reads on `PropertyNotify`
(`xwm_handle_property_notify`, `xwm.c:1446-1463`) and emits a
`set_*` signal so the compositor can react to runtime changes
(title renames, parent changes, state toggles) [5].

## 7. The XWM (X window manager)

The XWM is the most surprising part of wlroots' Xwayland story if
you come to it fresh. Xwayland is an X server. X11 clients expect an
X11 window manager to be present: to honour `_NET_WM_STATE`, to
handle MapRequest, to set input focus, to maintain `_NET_CLIENT_LIST`,
to negotiate `WM_TAKE_FOCUS`, and so on. wlroots runs that WM inside
the compositor process. It is not a separate binary.

The XWM is created when the Xwayland server signals ready
(`xwayland.c:34-54`) [5]:

```c
static void xwayland_mark_ready(struct wlr_xwayland *xwayland) {
    assert(xwayland->server->wm_fd[0] >= 0);
    xwayland->xwm = xwm_create(xwayland, xwayland->server->wm_fd[0]);
    // xwm_create takes ownership of wm_fd[0] under all circumstances
    xwayland->server->wm_fd[0] = -1;
    ...
    if (xwayland->seat) {
        xwm_set_seat(xwayland->xwm, xwayland->seat);
    }
    if (xwayland->cursor_buffer != NULL) {
        xwm_set_cursor(xwayland->xwm, xwayland->cursor_buffer,
                       xwayland->cursor_hotspot.x,
                       xwayland->cursor_hotspot.y);
    }
    wl_signal_emit_mutable(&xwayland->events.ready, NULL);
}
```

`xwm_create` (`xwm.c:2577-2706`) takes ownership of `wm_fd[0]`,
wraps it in an `xcb_connection_t` via `xcb_connect_to_fd`
(`xwm.c:2596`), and registers it with the wl_event_loop
(`xwm.c:2618-2622`) so xcb's read events drive the compositor's
main loop [5]. From that point on, the XWM and the wl_display share
one event loop, one thread.

Then the XWM does the WM-claim dance on the X11 server
(`xwm_create_wm_window`, `xwm.c:2383-2432`) [5]:

1. Create a window named "wlroots wm".
2. Set `_NET_SUPPORTING_WM_CHECK` on both the root and the wm
   window, pointing at the wm window. This is the EWMH way to
   announce a WM is present and identify it.
3. Take ownership of `WM_S0` and `_NET_WM_CM_S0` selections on the
   root. These are how X11 normally arbitrates which WM and which
   compositing manager is active.
4. Set the root window event mask to `SUBSTRUCTURE_NOTIFY |
   SUBSTRUCTURE_REDIRECT | PROPERTY_CHANGE` (`xwm.c:2628-2636`).
   `SUBSTRUCTURE_REDIRECT` is the magic mask that turns
   `MapRequest`, `ConfigureRequest`, `CirculateRequest` into
   requests-to-the-WM instead of letting the X server act on them
   directly. This is what makes the XWM an X11 WM.
5. `xcb_composite_redirect_subwindows` on the root with
   `XCB_COMPOSITE_REDIRECT_MANUAL` (`xwm.c:2638-2640`). This tells
   Xwayland: do not composite X11 windows yourself, the WM (us)
   will. This is what makes Xwayland rootless: each top-level X11
   window's pixels are exposed via the Composite extension and
   Xwayland attaches them to a wl_surface.
6. Set `_NET_SUPPORTED` listing the EWMH atoms the WM understands
   (`xwm.c:2642-2670`).
7. Create a 1x1 override-redirect `no_focus_window` at (-100,-100)
   to use as a focus sink when no surface should have keyboard
   focus (`xwm_create_no_focus_window`, `xwm.c:2434-2457`).

The XWM is bidirectional. X11 clients talk to the XWM via the X
protocol (MapRequest, ConfigureRequest, ClientMessage for
`_NET_WM_STATE`, etc.). Xwayland talks to the compositor via
Wayland (wl_surface, xwayland-shell-v1). The XWM bridges both.
Without the XWM, X11 clients would still display (Xwayland would
honour their MapWindow requests directly), but no WM would enforce
`_NET_WM_STATE_FULLSCREEN`, no `_NET_WM_STATE` would be honoured,
focus would not be set via `XSetInputFocus`, `_NET_ACTIVE_WINDOW`
would not be maintained, and ICCCM `WM_TAKE_FOCUS`/`WM_DELETE_WINDOW`
protocols would not be negotiated. Most X11 apps would misbehave.

The XWM uses a separate xcb connection from the one X11 clients use.
`wm_fd` is dedicated to the WM. X11 clients connect to the listening
sockets in `x_fd[]`. wlroots does not let X11 clients and the WM
share an X connection, because the WM needs `SUBSTRUCTURE_REDIRECT`
access that is unsafe to grant to ordinary clients.

## 8. Property reading

`read_surface_property` (`xwm.c:1085-1125`) is the dispatcher [5].
Given an atom and a `xcb_get_property_reply_t`, it routes to the
right `read_surface_*` function:

| Atom | Reader | Notes |
|---|---|---|
| `WM_CLASS` | `read_surface_class` | Two NUL-separated strings: instance, then class. Parsed at `xwm.c:654-683`. |
| `WM_NAME` / `_NET_WM_NAME` | `read_surface_title` | `_NET_WM_NAME` (UTF8_STRING) preferred over `WM_NAME` (STRING). See `xwm.c:751-787`. |
| `WM_TRANSIENT_FOR` | `read_surface_parent` | Window XID, looked up in the surface list. Cycle-check via `has_parent` (`xwm.c:789-800`) so a client cannot create a transient-for loop. |
| `_NET_WM_PID` | (ignored) | Comment in `xwm.c:1095-1096`: "intentionally ignored". wlroots uses X-Resource instead. |
| `_NET_WM_WINDOW_TYPE` | `read_surface_window_type` | Atom list, stored as `xcb_atom_t[]` (`xwm.c:838-863`). |
| `_NET_WM_ICON` | (no parse, signal only) | Compositor fetches on demand via `wlr_xwayland_surface_fetch_icon` (`xwm.c:1154-1174`). |
| `WM_PROTOCOLS` | `read_surface_protocols` | Atom list (`xwm.c:865-888`). Used to detect `WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`, `_NET_WM_PING`. |
| `_NET_WM_STATE` | `read_surface_net_wm_state` | Atom list decoded into bools (`xwm.c:1035-1068`). Resets all state bits first, then sets the ones present. |
| `WM_HINTS` | `read_surface_hints` | Parsed via `xcb_icccm_get_wm_hints_from_reply`. If the `INPUT` flag is absent, defaults `hints->input = true` ("assume it does", `xwm.c:909-913`). |
| `WM_NORMAL_HINTS` | `read_surface_normal_hints` | Parsed via `xcb_icccm_get_wm_size_hints_from_reply`. Fills in missing base/min size per ICCCM: if neither min nor base is set, both go to -1; if only one is set, the other mirrors it (`xwm.c:942-957`). |
| `_MOTIF_WM_HINTS` | `read_surface_motif_hints` | MwmHints decorations field, decoded into NO_BORDER / NO_TITLE flags (`xwm.c:967-1007`). |
| `_NET_WM_STRUT_PARTIAL` | `read_surface_strut_partial` | Panel struts. Parsed via `xcb_ewmh_get_wm_strut_partial_from_reply` (`xwm.c:1009-1033`). |
| `WM_WINDOW_ROLE` | `read_surface_role` | UTF8 or STRING (`xwm.c:729-749`). |
| `_NET_STARTUP_ID` | `read_surface_startup_id` | Startup notification ID (`xwm.c:685-707`). |
| `_NET_WM_WINDOW_OPACITY` | `read_surface_opacity` | uint32 scaled by UINT32_MAX to double in [0,1] (`xwm.c:709-727`). |

Two patterns recur across the readers. First, every reader
validates the property type before parsing. X11 properties carry a
type atom and a format (8/16/32). wlroots checks `reply->type` and
`reply->format`, logs `Invalid <name> property type` at DEBUG level
on mismatch, and bails. Second, every reader emits a `set_*` signal
after mutating the surface, so the compositor can re-derive its
state (window title bar text, focus-stealing policy, decoration
state).

The ICCCM size-hint defaulting at `xwm.c:942-957` is worth singling
out. ICCCM says: if min size is absent, treat it as equal to base
size, and vice versa. wlroots implements this exactly. Compositors
that consume `size_hints` directly can rely on this invariant
without re-implementing it.

## 9. Configure flow

X11 ConfigureNotify and Wayland configure-serial are different
mechanisms. wlroots bridges them in two directions.

Compositor to X11 client (resize request): the compositor calls
`wlr_xwayland_surface_configure(xsurface, x, y, w, h)`
(`xwm.c:2176-2216`) [5]:

```c
void wlr_xwayland_surface_configure(struct wlr_xwayland_surface *xsurface,
        int16_t x, int16_t y, uint16_t width, uint16_t height) {
    int old_w = xsurface->width;
    int old_h = xsurface->height;
    xsurface->x = x; xsurface->y = y;
    xsurface->width = width; xsurface->height = height;

    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
        XCB_CONFIG_WINDOW_BORDER_WIDTH;
    uint32_t values[] = {x, y, width, height, 0};
    xcb_configure_window(xwm->xcb_conn, xsurface->window_id, mask, values);

    // If the window size did not change, then we cannot rely on
    // the X server to generate a ConfigureNotify event. Instead,
    // we are supposed to send a synthetic event. See ICCCM part
    // 4.1.5. But we ignore override-redirect windows as ICCCM does
    // not apply to them.
    if (width == old_w && height == old_h && !xsurface->override_redirect) {
        xcb_configure_notify_event_t configure_notify = {
            .response_type = XCB_CONFIGURE_NOTIFY,
            .event = xsurface->window_id,
            .window = xsurface->window_id,
            .x = x, .y = y, .width = width, .height = height,
        };
        xwm_send_event_with_size(xwm->xcb_conn, 0, xsurface->window_id,
            XCB_EVENT_MASK_STRUCTURE_NOTIFY,
            &configure_notify, sizeof(configure_notify));
    }
    xwm_schedule_flush(xwm);
}
```

The synthetic ConfigureNotify is the ICCCM-required workaround for
the case where the WM issues a ConfigureWindow that does not change
geometry (ICCCM 4.1.5). X11 clients are entitled to a ConfigureNotify
after every WM-initiated configuration, and the X server only
generates one if geometry changed.

X11 client to compositor (resize request): the client calls
`XConfigureWindow`, which becomes a ConfigureRequest the X server
redirects to the WM. `xwm_handle_configure_request`
(`xwm.c:1265-1291`) [5] builds a `wlr_xwayland_surface_configure_event`
with the requested geometry (or the current geometry for any field
not in the request's value_mask) and emits the `request_configure`
signal. The compositor decides the actual geometry, then calls
`wlr_xwayland_surface_configure` to apply it. This is the policy/mechanism
split: the WM (wlroots) routes the request; the compositor decides.

The full round trip for a compositor-initiated resize is:

1. Compositor calls `wlr_xwayland_surface_configure(xsurface, x, y, w, h)`.
2. XWM sends `xcb_configure_window` to Xwayland.
3. Xwayland sends ConfigureNotify to the X11 client.
4. X11 client renders at the new size, calls `xcb_create_pixmap` /
   copies into its rootless pixmap, and the new pixels flow to the
   wl_surface Xwayland created for this window.
5. wlroots sees `wl_surface.commit` via the `surface_commit` listener
   (`xwm.c:1197-1198`, `xwm.c:1127-1132`) and maps the surface if it
   has a buffer.

Serial bookkeeping matters because Wayland surfaces carry an
implicit configure state in xdg-shell. Xwayland does not use
xdg-shell for its surfaces (it uses xwayland-shell-v1, which has no
configure events), so there is no Wayland serial to ack. The
ConfigureNotify round-trip lives entirely on the X11 side. The
Wayland surface commits whenever the X11 client gets new pixels in,
independent of any configure ack. This is a real divergence from
how xdg-shell surfaces work and is the reason wlroots can treat the
wl_surface for an Xwayland window as a mostly-passive target.

## 10. Damage tracking

wlroots does not have a separate "Xwayland damage" path. Xwayland
rendering into its rootless windows is exposed as standard
wl_surface.commit with wl_buffer, and the buffer carries damage
through the normal wl_surface.damage mechanism [4].

The emersion damage-tracking blog [4] lays out the model: damage
comes in two flavours, frame damage (what changed on the visible
output) and buffer damage (what changed in the buffer being drawn
into, which because of multi-buffering is not the same thing). A
compositor needs both: buffer damage to know what to redraw into
its render buffer, frame damage to know what to scan out from the
render buffer to the screen.

Xwayland, when an X11 client draws into its pixmap, does its own
damage tracking inside the X server (the X Damage extension) and
then translates that into wl_surface.damage on the corresponding
wl_surface before committing. The compositor (Sway, Hyprland,
whoever) sees damage on the wl_surface, intersects it with output
geometry, and decides what to redraw. There is no `wlr_xwayland`
involvement here beyond ensuring the wl_surface exists.

`wlr_surface` is the damage extension point. wlroots extends the
core wl_surface with `wlr_surface_get_current_damage` and the
`wlr_surface_state` damage list, which compositors consume through
the scene graph API (`wlr_scene`) or directly. The scene graph
automatically accumulates damage across commits and exposes it as
`wlr_output` damage via `wlr_output_damage`.

For Xwayland specifically the only wlroots-specific damage note is
the `surface_commit` listener in `xwm.c:1197-1198` and its handler
`xwayland_surface_handle_commit` (`xwm.c:1127-1132`):

```c
static void xwayland_surface_handle_commit(struct wl_listener *listener,
                                           void *data) {
    struct wlr_xwayland_surface *xsurface =
        wl_container_of(listener, xsurface, surface_commit);
    if (wlr_surface_has_buffer(xsurface->surface)) {
        wlr_surface_map(xsurface->surface);
    }
}
```

That is the entire wlroots X11-to-Wayland damage bridge: when
Xwayland commits a buffer, map the surface. wlroots relies on the
wlr_surface's existing damage tracking to propagate the actual
region.

## 11. Input forwarding

`wlr_seat` is wlroots' seat abstraction. wlroots does not have a
separate `wlr_xwayland_seat` type; the XWM keeps a single
`xwm->seat` pointer to the same `wlr_seat` the compositor uses for
Wayland clients (`include/xwayland/xwm.h:112`) [5].

Keyboard focus is routed to X11 windows via `XSetInputFocus`. The
flow:

1. Compositor decides an Xwayland surface should have keyboard
   focus, calls `wlr_xwayland_surface_activate(xsurface, true)`
   (`xwm.c:2166-2174`).
2. That calls `xwm_surface_activate` (`xwm.c:474-486`), which calls
   `xwm_focus_window` (`xwm.c:387-422`) if the focus was not already
   offered.
3. `xwm_focus_window` reads `xsurface->hints->input` to pick the
   ICCCM input model. If the window does not want passive focus
   (`!hints->input`), it sends `WM_TAKE_FOCUS` only and lets the
   client take focus itself. Otherwise it sends `WM_TAKE_FOCUS`
   followed by `xcb_set_input_focus(POINTER_ROOT, window_id,
   CURRENT_TIME)` (`xwm.c:407-421`).
4. The X11 client sees a `WM_TAKE_FOCUS` ClientMessage and a
   FocusIn event, knows it has focus, and starts receiving
   KeyPress events.

`XCB_INPUT_FOCUS_POINTER_ROOT` is used (rather than `XCB_NONE`)
when clearing focus, because `XCB_NONE` disables keyboard input
entirely and breaks keyboard grabs for popups (`xwm.c:392-401`
comment). The XWM's `no_focus_window` (section 7) is the actual
focus sink when no Xwayland surface has focus; `XCB_POINTER_ROOT`
is the fallback for the no-focus-window-also-unmapped case.

The `last_focus_seq` field on `struct wlr_xwm` (`xwm.h:149`) and
the `validate_focus_serial` logic in `xwm_handle_focus_in`
(`xwm.c:1908-1960`) defend against stale FocusIn events arriving
after the compositor has already moved focus elsewhere. wlroots
tracks the X11 sequence number of the last `xcb_set_input_focus`
request it issued, and discards FocusIn events whose sequence is
older. Without this, focus ping-pong between two X11 windows
results in the wrong one ending up focused.

The XIM/XKB translation is handled by Xwayland, not wlroots. Xwayland
receives Wayland wl_keyboard.keycode events with Linux evdev
keycodes, translates them through its XKB state, and synthesises X11
KeyPress/KeyRelease events with X11 keycodes. The compositor must
use the same keymap on its wl_keyboard as Xwayland uses internally
(which it does, because wlroots exports the keymap via
wl_keyboard.keymap and Xwayland consumes that). The compositor does
not need to know X11 keysyms.

Pointer and touch are similar: wl_seat.pointer motion and buttons
are delivered to the wl_surface under the cursor. If that surface
is an Xwayland surface, Xwayland translates them into X11
MotionNotify, ButtonPress, ButtonRelease. Touch is translated to
X11 motion+button events unless Xwayland is started with
`-noTouchPointerEmulation` (the `no_touch_pointer_emulation` server
option, `server.c:84-90`).

## 12. Selection forwarding

X11 selections and Wayland selections are different mechanisms.
wlroots bridges them in `xwayland/selection/`. Three selections
are bridged (`xwm.c:2682-2684`) [5]:

```c
xwm_selection_init(&xwm->clipboard_selection, xwm, xwm->atoms[CLIPBOARD]);
xwm_selection_init(&xwm->primary_selection, xwm, xwm->atoms[PRIMARY]);
xwm_selection_init(&xwm->dnd_selection, xwm, xwm->atoms[DND_SELECTION]);
```

Each `wlr_xwm_selection` (`include/xwayland/selection.h:39-49`) [5]
owns:

- an `atom` (CLIPBOARD, PRIMARY, or XDND_SELECTION),
- a `window` (an X11 window the XWM created to act as the
  selection owner on the X11 side),
- the current `owner` (X11 window that owns the selection on the
  X11 side),
- the `timestamp` of the last ownership change,
- incoming and outgoing transfer lists.

The bridge uses the XFIXES selection-notify extension to hear
about X11 ownership changes. `xwm_selection_init`
(`selection/selection.c:176-244`) calls
`xcb_xfixes_select_selection_input` with
`SET_SELECTION_OWNER | SELECTION_WINDOW_DESTROY |
SELECTION_CLIENT_CLOSE` masks. When an X11 client sets the X11
CLIPBOARD selection owner, the XWM gets a
`xcb_xfixes_selection_notify_event_t` and reacts by creating a
Wayland `wlr_data_source` that proxies the X11 selection
(`xwm_handle_xfixes_selection_notify`, declared in `selection.h:83`).

The XWM uses a separate X11 window per selection as its proxy
owner. This window has the `WL_SELECTION` atom on it during
transfers and the `CLIPBOARD_MANAGER` atom ownership for
clipboard-manager persistence (`selection.c:213-235`).

MIME translation: `xwm_mime_type_to_atom` and
`xwm_mime_type_from_atom` (`selection.c:62-89`) [5]. Two MIME
types are special-cased: `text/plain;charset=utf-8` maps to the
`UTF8_STRING` atom, `text/plain` maps to `TEXT`. Everything else
is interned as a literal X11 atom with the MIME type string as its
name.

Timestamp translation: X11 selection timestamps are millisecond
counters since some arbitrary epoch; Wayland selection serials are
monotonic. The bridge does not translate one to the other; it
treats them as opaque. The XWM uses `XCB_CURRENT_TIME` in its own
requests and stores the X11 timestamp from XFixesSelectionNotify
for use when releasing ownership (`selection.c:265-282`).

INCR transfers: X11 selections larger than a property-size
threshold are transferred incrementally. The sender writes a chunk
to a property on the requester's window, sends a SelectionNotify,
and waits for the requester to delete the property before writing
the next chunk. wlroots implements both directions.

- Incoming INCR (X11 client owns the selection, Wayland client
  wants it): `xwm_selection_transfer_get_data` checks if the
  reply type is `INCR` (`selection/incoming.c:178-194`). If so,
  it sets `transfer->incr = true` and waits for PropertyNotify
  events on the transfer window, calling `xwm_get_incr_chunk`
  (`incoming.c:158-176`) for each chunk. Each chunk is written to
  the Wayland client's pipe fd via
  `xwm_write_selection_property_to_wl_client` (`incoming.c:135-156`).
  When the X11 client writes a zero-length property, the transfer
  is complete.
- Outgoing INCR (Wayland client owns the selection, X11 client
  wants it): implemented in `selection/outgoing.c`. The XWM
  creates a property on the requester's window with the first
  chunk and the INCR type, then on each PropertyDelete writes the
  next chunk via `xwm_send_incr_chunk`.

INCR chunk size is `64 * 1024` bytes (`selection.h:8`) [5].

PRIMARY and CLIPBOARD share most code; the differences are just
which atom they own and which Wayland selection they proxy. The
CLIPBOARD selection also takes ownership of the `CLIPBOARD_MANAGER`
atom (`selection.c:230-232`), so that X11 clipboard managers do
not fight the XWM for ownership.

## 13. DnD forwarding

Wayland drag-and-drop (wl_data_device) and X11 drag-and-drop (XDND)
are bridged through the `dnd_selection` and the code in
`selection/dnd.c` [5].

For a Wayland client dragging over an X11 window:

1. Compositor starts a `wlr_drag` on the wl_seat. The XWM's
   `seat_handle_start_drag` listener fires (`selection.c:313-319`)
   and calls `xwm_seat_handle_start_drag` (`dnd.c:355-379`).
2. `xwm_selection_set_owner(&xwm->dnd_selection, true)` is called
   so the XWM's DnD proxy window becomes the X11 XDND selection
   owner (`selection.c:317`).
3. As the drag moves, `seat_handle_drag_focus` (`dnd.c:279-289`)
   fires. If the new focus is an Xwayland surface,
   `xwm_set_drag_focus` (`dnd.c:255-277`) sends an XdndEnter
   message to that surface's X11 window.
4. `seat_handle_drag_motion` (`dnd.c:291-302`) sends XdndPosition
   messages with the current coordinates and the source's
   supported actions.
5. The X11 client responds with XdndStatus (handled in
   `xwm_handle_selection_client_message`, `dnd.c:174-204`).
6. On pointer release, `seat_handle_drag_drop` (`dnd.c:304-320`)
   sends XdndDrop.
7. The X11 client requests the selection, performs the transfer
   (same INCR path as section 12), and on completion sends
   XdndFinished (`dnd.c:205-238`).

For an X11 client dragging over a Wayland surface, the reverse
path applies: the XWM gets XdndEnter on its DnD proxy window
(because it owns the XDND selection proxy), translates it into
wl_data_device enter events on the Wayland surface under the
pointer. The DnD proxy window is created at 8192x8192 in
`xwm_selection_init` (`selection.c:187-212`) [5]:

```c
if (atom == xwm->atoms[DND_SELECTION]) {
    xcb_create_window(..., 8192, 8192, ..., XCB_WINDOW_CLASS_INPUT_ONLY, ...);
    xcb_change_property(..., xwm->atoms[DND_AWARE], XCB_ATOM_ATOM, 32, 1,
                        &(uint32_t){XDND_VERSION});
}
```

That window is `INPUT_ONLY` (no pixmap, never visible), tagged
`_XdndAware` so X11 DnD initiators see it as a drop target, and
placed at the screen origin. The 8192x8192 size is the bounding
box for the screen; XdndPosition events come in with root-relative
coordinates, and the XWM translates them to surface-local
coordinates for the Wayland drag events. The XDND protocol version
is 5 (`selection.h:10`) [5].

The `drop_focus` field on `struct wlr_xwm` is distinct from
`drag_focus` (`xwm.h:140-141`). After the drag ends but before the
X11 client has read the data, the drag source still exists; the
XWM needs to remember which window was the drop target so
XdndFinished can be matched. `drop_focus` holds that.

## 14. EGL/GL rendering

Xwayland uses glamor (its GL-accelerated drawing layer) to render
X11 client pixmaps into GBM/dma-buf buffers and attaches them to
its wl_surface. wlroots treats the resulting wl_buffer like any
other. There is no `wlr_xwayland_surface` rendering path; the
surface flows through the standard `wlr_surface` ->
`wlr_buffer` -> renderer pipeline.

The compositor does not need to know whether a buffer came from an
Xwayland client or a native Wayland client. Both produce
`wlr_buffer`s via `wl_buffer` resources, both go through
`wlr_renderer_begin_buffer_pass` or the scene graph's automatic
texture-from-buffer path, both get composited into the output.

The one Xwayland-specific quirk is `has_alpha`
(`include/wlr/xwayland/xwayland.h:194`). X11 windows with depth 32
have an alpha channel; depth-24 windows do not. wlroots populates
this once at surface creation from `xcb_get_geometry`'s depth
field (`xwm.c:255-260`). Compositors use it to decide whether to
blend or to fast-path opaque surfaces. Without it the compositor
would have to inspect the wl_buffer format on every commit, which
is more work and arrives late (the buffer is created by Xwayland
on demand, not at surface creation).

Explicit synchronisation ( dma-buf sync_files / sync_timeline /
drm_syncobj) is handled by wlroots' generic buffer sync framework,
not by anything Xwayland-specific. wlroots 0.19 added full explicit
sync support; Xwayland gained explicit-sync awareness in the same
window, so the two compose cleanly without per-surface glue.

## 15. Cursor handling

X11 cursor fonts (XDefineCursor on a window) and Wayland cursor
surfaces (wl_pointer.set_cursor) are different mechanisms. wlroots
bridges them in `xwm_set_cursor` (`xwm.c:2522-2575`) [5]:

```c
void xwm_set_cursor(struct wlr_xwm *xwm, struct wlr_buffer *buffer,
                    int32_t hotspot_x, int32_t hotspot_y) {
    ...
    void *pixels = NULL;
    uint32_t format = DRM_FORMAT_INVALID;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buffer,
            WLR_BUFFER_DATA_PTR_ACCESS_READ, &pixels, &format, &stride)) {
        return;
    }
    if (format != DRM_FORMAT_ARGB8888) {
        wlr_buffer_end_data_ptr_access(buffer);
        wlr_log(WLR_ERROR, "Only ARGB8888 is supported for Xwayland cursors");
        return;
    }
    int depth = 32;

    xcb_pixmap_t pix = xcb_generate_id(xwm->xcb_conn);
    xcb_create_pixmap(xwm->xcb_conn, depth, pix, xwm->screen->root,
                      buffer->width, buffer->height);
    ...
    xcb_put_image(xwm->xcb_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pix, gc,
                  buffer->width, buffer->height, 0, 0, 0, depth,
                  stride * buffer->height, pixels);
    ...
    xwm->cursor = xcb_generate_id(xwm->xcb_conn);
    xcb_render_create_cursor(xwm->xcb_conn, xwm->cursor, pic,
                             hotspot_x, hotspot_y);
    ...
    uint32_t values[] = {xwm->cursor};
    xcb_change_window_attributes(xwm->xcb_conn, xwm->screen->root,
                                 XCB_CW_CURSOR, values);
    xwm_schedule_flush(xwm);
}
```

This is the X11-side cursor: it sets the root window cursor, which
X11 clients see as the cursor when no X11 client has set its own.
The compositor calls `wlr_xwayland_set_cursor` (which queues a
call to `xwm_set_cursor` if the XWM is up, or stashes the buffer
for when it comes up) to push the current wl_pointer cursor image
into X11.

`wlr_xcursor_manager` is the deprecated older API for loading
cursor themes. Newer wlroots exposes `wlr_cursor` plus
xcursor-theme-loaded `wlr_buffer`s, and compositors push those
buffers through `wlr_xwayland_set_cursor`. The X11 cursor font
mechanism (XCreateFontCursor) is not used; wlroots always uploads
a pixel buffer via xcb_put_image and XRender's
`xcb_render_create_cursor`.

The reverse direction (an X11 client calling XDefineCursor on its
own window) does not need wlroots involvement. Xwayland handles
it: when the wl_pointer enters an X11 window that has a cursor
defined, Xwayland emits wl_pointer.set_cursor to the compositor
with a cursor surface it synthesises. The compositor sees a normal
wl_pointer cursor and composites it.

## 16. What wlroots got wrong (with hindsight)

The wlroots maintainers have publicly revised several decisions
over the years. The clearest examples:

**The WL_SURFACE_ID race.** The original association mechanism
between X11 windows and wl_surfaces was a ClientMessage on the X11
window carrying the wl_surface resource id. Because the X11
socket and the Wayland socket are independent, the surface could
be destroyed and its id recycled before the compositor processed
the ClientMessage. The compositor would then associate the X11
window with the wrong wl_surface or a non-surface object. The
xwayland-shell-v1 protocol [3] was added specifically to fix this
by moving association to the Wayland side and using a 64-bit
serial to pair the two timelines. wlroots still carries the
WL_SURFACE_ID code path as a fallback for old Xwayland
(`xwm.c:1465-1502`), but new Xwayland uses shell_v1
(`xwm.c:1504-1531`).

**The single-Xwayland assumption.** wlroots historically allowed
exactly one Xwayland per compositor. Issue #1442 on the old
swaywm/wlroots tracker [9] asked for multiple instances and was
deferred for years. The split of `wlr_xwayland` into
`wlr_xwayland_server` (process launcher) and `wlr_xwayland`
(WM + shell_v1) is the eventual fix: a compositor can now create
multiple servers and wrap each in its own `wlr_xwayland` via
`wlr_xwayland_create_with_server`. The `own_server` bool on
`wlr_xwayland` (`xwayland.h:42`) exists because of this split.

**`_NET_CLIENT_LIST` ordering.** The comment at `xwm.c:331-333`
is a self-admitted bug:

```
// FIXME: _NET_CLIENT_LIST is expected to be ordered by map time, but
// the order of surfaces in `xwm->surfaces` is by creation time. The
// order of windows _NET_CLIENT_LIST exposed by wlroots is wrong.
```

This has been there for a long time and breaks taskbars that rely
on the EWMH ordering. It is a structural issue: `xwm->surfaces` is
the creation-order list, and reordering it for `_NET_CLIENT_LIST`
would need a separate map-time list.

**Globally-active input model.** X11 ICCCM defines four input
models; the Globally Active model lets a client grab focus on its
own terms via WM_TAKE_FOCUS. wlroots added
`wlr_xwayland_surface_offer_focus` (`xwm.c:453-472`) and
`wlr_xwayland_icccm_input_model` to give compositors a way to
offer focus without forcing it. Mutter has a parallel issue with
globally-active windows [10]. The wlroots API comment for
`offer_focus` admits the model is fragile ("there is no reliable
way to know in advance whether these windows want to be focused",
`xwayland.h:354-361`).

**Override-redirect focus.** `wlr_xwayland_surface_override_redirect_wants_focus`
(`xwayland.h:373-395`) is a heuristic for handing focus to OR
windows like rofi or dzen that grab input via mechanisms wlroots
does not support. The doc comment is unusually candid: "It's
probably not perfect, nor exactly intended but works in practice"
(`xwayland.h:389-390`). This is a known workaround for an X11
design wart.

**The dual surface-association path.** Having to keep both
`WL_SURFACE_ID` and `WL_SURFACE_SERIAL` paths in `xwm.c` is
maintenance burden. wlroots cannot drop the old path as long as
it supports pre-shell-v1 Xwayland, which is still in the wild.
Ishizue does not have this constraint (section 18).

**The implicit reliance on libwayland for filtering globals.** The
shell_v1 global is exposed to all clients by default; the
compositor has to set up a `wl_display_set_global_filter` to hide
it from non-Xwayland clients. wlroots documents this in the
`xwayland.h` header comment (`xwayland.h:35-39`) but does not
enforce it. Several wlroots-based compositors have shipped
without the filter and exposed the shell to every client.

## 17. What Ishizue should copy

The XWM-in-compositor-process approach. wlroots runs the X11 WM
in the compositor process, sharing the event loop with the
Wayland dispatch. This avoids an IPC boundary for every X11
property read and ConfigureRequest, lets the WM and the
compositor share the wlr_surface objects, and makes focus routing
a function call instead of a message. Ishizue's SPEC §13 already
specifies the bridge as a separate process, but the bridge should
not contain the X11 WM; the XWM equivalent should live in the
library process and talk to the bridge over an established
channel. The split is: bridge = X11 protocol translator + wl_proxy
for Ishizue; XWM = lives in the library, owns the EWMH/ICCCM
compliance.

The `wlr_xwayland_server` / `wlr_xwayland` / `wlr_xwm`
three-layer split. Server (process + lifecycle), shell
(Wayland-protocol surface role), XWM (X11 WM logic) are cleanly
separated with explicit ownership and signal-based wiring.
Ishizue should adopt this layout: one struct for the Xwayland
process, one for the protocol binding, one for the X11 WM
behaviour. The `own_server` bool and `wlr_xwayland_create_with_server`
escape hatch matter for testing.

The socketpair-per-channel pattern. wlroots uses one socketpair
for the Wayland client connection and a separate socketpair for
the XWM X11 connection. The two sockets are independent: the
Wayland side carries the wl_surface protocol, the X11 side
carries the X11 protocol. The wl_client is created directly via
`wl_client_create(display, fd)` rather than through wl_display's
accept path. Ishizue should do the same: the bridge connects to
the Ishizue socket like any other client (SPEC §6.3 allowlist
already supports this), but the bridge-to-library X11-WM channel
should be a dedicated socketpair the library creates.

The double-fork spawn. `fork()` then `fork()` again so the
intermediate child exits and the compositor does not have to
reap Xwayland (`server.c:384-401`). This is the standard daemon
spawn pattern and avoids SIGCHLD handler contention with the
compositor's own children.

The `-displayfd` readiness mechanism. Readiness via pipe is
race-free and works under double-fork. SIGUSR1 is fragile because
it requires the parent process to install a signal handler and
because signal disposition does not survive fork/exec cleanly.
Ishizue's bridge should use a similar fd-based readiness signal
if it spawns a separate process.

The property-reading layer. Each `read_surface_*` function does
exactly one property: validates the reply type, parses, mutates
the surface struct, emits a signal. The dispatcher
(`read_surface_property`) is a flat if-else on the atom. This is
the right shape; Ishizue should reproduce it. The signal-per-property
pattern lets the compositor subscribe to exactly the changes it
cares about (title bars listen for `set_title`, taskbars listen
for `set_class` and `set_window_type`, focus policy listens for
`set_hints`).

The "validate before parse" discipline. Every reader checks
`reply->type` and `reply->format` before consuming the value, and
logs + bails on mismatch. X11 clients set broken properties
regularly. This defensive parsing is load-bearing.

The ICCCM size-hint defaulting (`xwm.c:942-957`). ICCCM says
absent min size equals base size and vice versa. Implementing
this in the library means every compositor does not have to.

The pid-via-X-Resource path. `_NET_WM_PID` is set by the client
and lies. The X-Resource extension's
`xcb_res_query_client_ids` returns the actual PID. wlroots uses
the latter when available and ignores the former
(`xwm.c:1095-1096`).

The separate DnD proxy window. An INPUT_ONLY X11 window tagged
`_XdndAware`, sized to the screen, used as the XDND selection
owner. This is how a Wayland-initiated drag reaches X11 drop
targets without exposing the compositor's wm window.

## 18. What Ishizue should do differently

Ishizue is not bound by Wayland protocol constraints. Several
wlroots workarounds exist purely because Wayland lacks a feature;
Ishizue can build the feature in.

**Native X11-surface association, no shell_v1.** xwayland-shell-v1
exists because Wayland has no notion of "this wl_surface is an X11
top-level". The protocol [3] is a Wayland-side binding with a
serial-pairing dance to recover the surface identity that
Wayland protocol lost. Ishizue's protocol owns object IDs and
their semantics (SPEC §6.4). A surface created by the bridge can
carry a "this is an X11 top-level" tag natively, with the X11
window XID attached. No serial pairing, no shell-v1, no
association race. The bridge sends one message: "create surface,
role=x11_toplevel, xid=0x1234". The library knows immediately.

**Single association path, not two.** wlroots carries both
WL_SURFACE_ID and WL_SURFACE_SERIAL because it must support old
Xwayland. Ishizue's bridge is the only X11 client that ever
exists. There is one association mechanism, defined once.

**No global-filter requirement.** wlroots exposes
xwayland_shell_v1 to all Wayland clients by default and tells the
compositor to filter. Ishizue can mark the "x11_toplevel" surface
role as a privileged operation from the start (SPEC §6.3
allowlist already exists for this). The bridge binary is the only
client allowed to create x11_toplevel surfaces. No filter needed.

**No xdg-shell-style configure round-trip for X11 windows.**
wlroots' Xwayland surfaces do not use xdg-shell, so they do not
have configure events or ack serials. But they still flow through
the wl_surface commit machinery, which was designed with
xdg-shell in mind. Ishizue can have a first-class configure flow
for X11 windows: the bridge gets a "configure request" event from
the library carrying the X11 geometry, calls back with the actual
geometry, and the library issues the ConfigureNotify on the X11
side. The serial bookkeeping that wlroots avoids by accident (X11
windows have no wl-side configure) Ishizue can avoid by design.

**Bridge as ordinary client, XWM in library.** wlroots runs the
XWM in the compositor process because the compositor and the
library are the same binary in a wlroots world. Ishizue's bridge
is a separate process (SPEC §13). The XWM logic should live in
the library, with the bridge acting as a thin translator that
forwards X11 wire messages to the library's XWM over a dedicated
fd. This preserves the "XWM in the mechanism layer" benefit
without putting X11 logic in the bridge.

**Selection timestamps as first-class.** wlroots treats the X11
timestamp and the Wayland serial as opaque and unrelated.
Ishizue's protocol can define a selection-ownership timestamp
natively (the library already runs its own clock). The bridge
translates the X11 timestamp on the way in. No opaque types.

**No `wlr_xcursor_manager` legacy.** wlroots has a deprecated
xcursor manager and a newer cursor-buffer API coexisting. Ishizue
starts clean: cursor themes are loaded by the Architect (SPEC
§6.10), the library accepts `isz_buffer` for cursor images, and
the bridge pushes them to X11 via the same xcb_put_image path
wlroots uses. One API.

**EWMH compliance as a library responsibility.** wlroots exposes
_NET_SUPPORTED, _NET_CLIENT_LIST, _NET_CLIENT_LIST_STACKING,
_NET_ACTIVE_WINDOW, _NET_WORKAREA, and the _NET_WM_STATE bits as
the compositor's job to keep consistent (the library provides
helpers but the compositor wires them). Ishizue should pull all
of this into the library: the XWM knows the surface list, knows
the stacking order, knows the focused window. The Architect does
not need to maintain EWMH state. wlroots leaves it to the
compositor because of the policy/mechanism line, but EWMH
maintenance is mechanism, not policy; it is what an X11 WM does.

**No ICCCM input-model guessing at the API boundary.** wlroots
exposes both `wlr_xwayland_surface_activate` (force focus) and
`wlr_xwayland_surface_offer_focus` (offer, let client decide) and
makes the compositor pick. The choice is determined by the
window's ICCCM hints, which the library already reads. Ishizue
can have one `isz_x11_surface_focus()` that internally inspects
the hints and does the right thing. The compositor should not
need to know ICCCM input models exist.

**No `override_redirect_wants_focus` heuristic in the public API.**
wlroots exposes its OR-focus guess as a public function with a
candid doc comment. Ishizue should keep the heuristic, but make
it internal. The Architect says "focus surface X"; the library
figures out whether X is an OR window, whether it wants focus,
and whether to use WM_TAKE_FOCUS. That decision is mechanism.

**One surface list, ordered correctly.** wlroots carries the
_NET_CLIENT_LIST ordering bug because it has one surface list in
creation order and uses it for both creation-time iteration and
map-time ordering. Ishizue should keep two lists from day one:
creation order (for stable iteration and ID lookup) and map-time
order (for EWMH). The cost is one extra `wl_list` per surface.

## References

[1] live-clones/wlroots mirror on GitHub, tracks gitlab.freedesktop.org/wlroots/wlroots minute-by-minute: https://github.com/live-clones/wlroots

[2] wlroots canonical repo on freedesktop GitLab: https://gitlab.freedesktop.org/wlroots/wlroots

[3] xwayland-shell-v1 protocol description, Wayland Explorer: https://wayland.app/protocols/xwayland-shell-v1

[4] emersion, "Introduction to damage tracking" (May 2019): https://emersion.fr/blog/2019/intro-to-damage-tracking

[5] wlroots master branch source files fetched from [1]:
    - `xwayland/xwayland.c` (https://github.com/live-clones/wlroots/blob/master/xwayland/xwayland.c)
    - `xwayland/server.c` (https://github.com/live-clones/wlroots/blob/master/xwayland/server.c)
    - `xwayland/shell.c` (https://github.com/live-clones/wlroots/blob/master/xwayland/shell.c)
    - `xwayland/sockets.c` (https://github.com/live-clones/wlroots/blob/master/xwayland/sockets.c)
    - `xwayland/xwm.c` (https://github.com/live-clones/wlroots/blob/master/xwayland/xwm.c)
    - `xwayland/selection/selection.c` (https://github.com/live-clones/wlroots/blob/master/xwayland/selection/selection.c)
    - `xwayland/selection/incoming.c` (https://github.com/live-clones/wlroots/blob/master/xwayland/selection/incoming.c)
    - `xwayland/selection/outgoing.c` (https://github.com/live-clones/wlroots/blob/master/xwayland/selection/outgoing.c)
    - `xwayland/selection/dnd.c` (https://github.com/live-clones/wlroots/blob/master/xwayland/selection/dnd.c)
    - `include/wlr/xwayland/xwayland.h` (https://github.com/live-clones/wlroots/blob/master/include/wlr/xwayland/xwayland.h)
    - `include/wlr/xwayland/server.h` (https://github.com/live-clones/wlroots/blob/master/include/wlr/xwayland/server.h)
    - `include/wlr/xwayland/shell.h` (https://github.com/live-clones/wlroots/blob/master/include/wlr/xwayland/shell.h)
    - `include/xwayland/xwm.h` (https://github.com/live-clones/wlroots/blob/master/include/xwayland/xwm.h)
    - `include/xwayland/selection.h` (https://github.com/live-clones/wlroots/blob/master/include/xwayland/selection.h)

[6] wlroots README.md, fetched from [1]: https://github.com/live-clones/wlroots/blob/master/README.md

[7] wlroots generated docs for `<wlr/xwayland/xwayland.h>`: https://wlroots.pages.freedesktop.org/wlroots/wlr/xwayland/xwayland.h.html

[8] Commit that added `xwayland_shell_v1` to wayland-protocols (staging): https://forge.quantum6.ca/quantum/wayland-protocols/commit/8d79352851199eeb4fe1ad7644c06502c1cb518f

[9] wlroots issue #1442, "Support for multiple xwayland instances": https://github.com/swaywm/wlroots/issues/1442

[10] Mutter issue #3328, "Keyboard input focus broken for globally active Xwayland windows": https://gitlab.gnome.org/GNOME/mutter/-/issues/3328

[11] Ishizue SPEC, sections 1 (What This Is), 2 (Non-Goals), 6 (Client Protocol, esp. §6.3 client trust, §6.4 object model, §6.8 clipboard, §6.9 DnD, §6.10 cursor themes), 13 (X11 Compatibility): `/home/z/my-project/repos/Ishizue/SPEC.md`
