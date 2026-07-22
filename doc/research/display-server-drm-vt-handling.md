# How display servers handle DRM master, VT switching, and input (W14-A)

Ishizue's DRM backend has hit a cluster of bugs around VT switching,
DRM master acquisition and release, and modeset handling. This document
reverse-engineers how six production display servers (wlroots, Weston,
Sway, Hyprland/Aquamarine, Xorg, and seatd) handle the same problems,
then maps every difference onto our code in `src/backend/isz_drm.c`,
`src/input/isz_session.c`, and `src/isz_lifecycle.c`. Sources are cited
inline as `[N]` and listed at the end. Line numbers refer to the
snapshots fetched for this task.

The pattern is consistent: every compositor that ships today delegates VT
handling and DRM master management to a session layer outside the
compositor process. wlroots, Aquamarine, and Sway all link libseat and
treat `drmSetMaster`/`drmDropMaster` as something the session layer
does, not the compositor. Weston has its own direct launcher that does
call `drmDropMaster` from the VT signal handler, but only when neither
logind nor libseat is available. Xorg is the only one that drives the
full SIGUSR1/VT_RELDISP dance in its own process. Ishizue currently
does a hybrid: it links libseat AND calls `drmSetMaster`/`drmDropMaster`
itself, AND has a direct-VT fallback path that the libseat path never
disables cleanly. That hybrid is the source of most of the bugs.

## 1. wlroots session management

The wlroots session layer lives in `backend/session/session.c` [1]. The
entry point is `wlr_session_create` (line 205), which calls
`libseat_session_init` (line 75). The init function does three things
in order:

1. `libseat_set_log_handler` + `libseat_set_log_level` (lines 76-77):
   route libseat logs through wlroots's logger.
2. `libseat_open_seat(&seat_listener, session)` (line 82): open the
   seat. The seat listener is two callbacks, defined at lines 26-37:

```c
static void handle_enable_seat(struct libseat *seat, void *data) {
        struct wlr_session *session = data;
        session->active = true;
        wlr_signal_emit_safe(&session->events.active, NULL);
}

static void handle_disable_seat(struct libseat *seat, void *data) {
        struct wlr_session *session = data;
        session->active = false;
        wlr_signal_emit_safe(&session->events.active, NULL);
        libseat_disable_seat(session->seat_handle);
}
```

`handle_disable_seat` is the load-bearing one. It sets `session->active
= false`, emits the `events.active` signal so backends can pause, then
calls `libseat_disable_seat` to acknowledge. The order matters: the
compositor's cleanup runs first, the acknowledgement second. The
libseat protocol contract [9, lines 25-36] requires the disable event
to be acknowledged "shortly after receiving this event" or "seat
devices may be forcibly revoked."

3. `wl_event_loop_add_fd(event_loop, libseat_get_fd(session->seat_handle),
   WL_EVENT_READABLE, libseat_event, session)` (lines 96-97): add the
   libseat socket to the wayland event loop. The handler is
   `libseat_event` (lines 39-46):

```c
static int libseat_event(int fd, uint32_t mask, void *data) {
        struct wlr_session *session = data;
        if (libseat_dispatch(session->seat_handle, 0) == -1) {
                wlr_log_errno(WLR_ERROR, "Failed to dispatch libseat");
                wl_display_terminate(session->display);
        }
        return 1;
}
```

A final `libseat_dispatch(session->seat_handle, 0)` (line 105) drains
any events that fired between `libseat_open_seat` returning and the
event source being added. Without this drain, an `enable_seat` that
arrived during `open_seat` would sit in the socket buffer until the
next external wakeup.

After `libseat_session_init` returns, `wlr_session_create` sets up a
udev monitor for DRM device add/change/remove events (lines 222-245).
This is how wlroots does GPU hotplug: udev tells the session layer a
new card appeared, the session emits `events.add_drm_card`, and the
multi-backend code opens it.

`wlr_session_open_file` (line 286) opens a device through libseat:

```c
struct wlr_device *wlr_session_open_file(struct wlr_session *session,
                const char *path) {
        int fd;
        int device_id = libseat_open_device(session->seat_handle, path, &fd);
        ...
        dev->fd = fd;
        dev->dev = st.st_rdev;
        dev->device_id = device_id;
        ...
}
```

The fd returned by `libseat_open_device` is the DRM fd. wlroots does
NOT call `drmSetMaster` on it. The fd already has master privileges
because the seatd daemon (or logind) opened it with master and passed
it via SCM_RIGHTS. The struct `drm_file` is shared between the daemon
and the compositor because SCM_RIGHTS dups the underlying `struct
file`, not the fd table entry. This is documented in the kernel DRM
UAPI [7]: "For every struct drm_file which has had at least once
successfully became the device master ... there exists one
drm_master."

`wlr_session_change_vt` (line 333) calls `libseat_switch_session`,
which is the only VT-related call the compositor makes. It does not
touch VT ioctls directly.

The sequence on VT switch away:

1. Kernel sends SIGUSR1 to the seatd daemon (the process that called
   `VT_SETMODE`).
2. seatd's signal handler calls `seat_vt_release` [8, line 742].
3. `seat_vt_release` calls `seat_disable_client` [8, line 593], which
   deactivates all client devices and sends `SERVER_DISABLE_SEAT` to
   the compositor.
4. `seat_vt_release` then calls `vt_ack(seat, true)` [8, line 754]
   which calls `ioctl(fd, VT_RELDISP, 1)` [10, line 225]. The kernel
   switches the VT.
5. The compositor's libseat fd becomes readable. `libseat_event` calls
   `libseat_dispatch`, which reads `SERVER_DISABLE_SEAT` and invokes
   `handle_disable_seat`.
6. `handle_disable_seat` emits `events.active` (false), then calls
   `libseat_disable_seat` to ack back to the daemon.

The sequence on VT switch back:

1. Kernel sends SIGUSR2 to seatd.
2. seatd's `server_handle_vt_acq` [11, line 80] calls
   `seat_vt_activate` [8, line 723].
3. `seat_vt_activate` calls `vt_ack(seat, false)` which calls
   `ioctl(fd, VT_RELDISP, VT_ACKACQ)` [10, line 235], then calls
   `seat_activate` to find the right client.
4. `seat_open_client` re-activates devices and sends
   `SERVER_ENABLE_SEAT` to the compositor.
5. The compositor's `handle_enable_seat` fires, sets `session->active
   = true`, emits `events.active` (true).
6. The DRM backend's `handle_session_active` listener (see section 2)
   re-commits KMS state on every connector.

The key insight: the compositor never calls `VT_SETMODE`,
`VT_RELDISP`, `drmSetMaster`, or `drmDropMaster`. All four are the
session layer's job. The compositor only pauses rendering on disable
and re-commits state on enable.

## 2. wlroots DRM backend

The DRM backend lives in `backend/drm/backend.c` [2] (the file at
`backend/drm/drm.c` in the archived swaywm mirror [3] is the older
single-file version that has since been split). The entry point is
`wlr_drm_backend_create` (line 164). The relevant sequence:

```c
drm->dev = dev;
drm->fd = dev->fd;
...
drm->session_active.notify = handle_session_active;
wl_signal_add(&session->events.active, &drm->session_active);

if (!check_drm_features(drm)) { ... }
if (!init_drm_resources(drm)) { ... }
```

The fd is `dev->fd`, which is the fd `wlr_session_open_file` returned.
No `drmSetMaster` call. `check_drm_features` queries caps
(`DRM_CLIENT_CAP_ATOMIC`, `DRM_CAP_SYNCOBJ`, etc.) but does not touch
master. `init_drm_resources` enumerates connectors, CRTCs, planes
without touching master.

The session-active listener (line 98) is the only place the DRM
backend reacts to VT switches:

```c
static void handle_session_active(struct wl_listener *listener, void *data) {
        struct wlr_drm_backend *drm =
                wl_container_of(listener, drm, session_active);
        struct wlr_session *session = drm->session;

        if (session->active) {
                wlr_log(WLR_INFO, "DRM fd resumed");
                scan_drm_connectors(drm);
                struct wlr_drm_connector *conn;
                wl_list_for_each(conn, &drm->outputs, link) {
                        ...
                        drm_connector_commit_state(conn, &state);
                }
        } else {
                wlr_log(WLR_INFO, "DRM fd paused");
        }
}
```

The "DRM fd paused" branch is empty. No `drmDropMaster`. No
`VT_RELDISP`. The session layer already did both. The "DRM fd resumed"
branch calls `scan_drm_connectors` (in case connectors changed while
we were away) and re-commits each connector's state. This re-commit is
what makes the screen come back after a VT switch: the CRTC was
disabled by the new VT owner (or by the kernel on switch), so we have
to re-enable it.

What happens to surfaces on VT switch: nothing, in wlroots itself.
Surfaces stay in their last committed state. The compositor (Sway,
Hyprland, etc.) may decide to drop frame scheduling during the pause,
but the backend does not destroy or invalidate surfaces. Page-flip
events that arrive during the pause are processed by `handle_drm_event`
[3, line 1481], but the page-flip handler checks `drm->session->active`
[3, line 1476] and suppresses the frame event when paused:

```c
if (drm->session->active) {
        wlr_output_send_frame(&conn->output);
}
```

This prevents the compositor from scheduling a new frame while the VT
is away, which would queue a page flip that can never complete (the
CRTC is owned by another VT).

The DRM fd is added to the wayland event loop at line 208:

```c
drm->drm_event = wl_event_loop_add_fd(event_loop, drm->fd,
        WL_EVENT_READABLE, handle_drm_event, drm);
```

`handle_drm_event` calls `drmHandleEvent` with a `page_flip_handler2`
callback. There is no separate vblank or page-flip fd; the DRM fd
itself is the event source.

The DRM backend also listens on `dev->events.change` and
`dev->events.remove` (lines 199-203). The change event fires when udev
sends a "change" action for the DRM device (hotplug, DPMS state
change, etc.) and triggers `scan_drm_connectors`. The remove event
fires when the device is gone and destroys the backend.

## 3. wlroots libseat integration

In the swaywm/wlroots mirror [1], the libseat code is inline in
`backend/session/session.c`, not a separate file. The task asks for
`backend/session/libseat.c`; that file does not exist in this snapshot
of the tree. The libseat integration is the four functions quoted in
section 1: `handle_enable_seat`, `handle_disable_seat`, `libseat_event`,
and `libseat_session_init`. The seat listener struct is at lines 48-51:

```c
static struct libseat_seat_listener seat_listener = {
        .enable_seat = handle_enable_seat,
        .disable_seat = handle_disable_seat,
};
```

The order of operations in `handle_disable_seat` is fixed by the
libseat protocol [9, lines 25-36]:

> The seat has been disabled. This event signals that the application
> is going to lose its seat access. The event *must* be acknowledged
> with libseat_disable_seat shortly after receiving this event.

wlroots's order: set `session->active = false`, emit
`events.active` (which fires `handle_session_active` in every backend
that cares, e.g. the DRM backend logs "DRM fd paused" and the libinput
backend calls `libinput_suspend`), then call `libseat_disable_seat`.
This means backends pause themselves before the ack goes back to the
daemon. If the daemon was blocking on the ack, it would be blocked
while the backends pause, which is fine because the kernel has not yet
switched the VT (the daemon acks the kernel after the compositor acks
the daemon, see section 9).

The `libseat_get_fd` is added to the event loop in
`libseat_session_init` (line 96) with `WL_EVENT_READABLE`. The
dispatch function `libseat_event` calls `libseat_dispatch(handle, 0)`
with zero timeout. There is no loop; one call per fd wakeup. This is
correct because libseat's `dispatch` drains everything currently
readable and returns. If more events arrive after dispatch returns,
the fd stays readable and the event loop fires again.

The `libseat_dispatch` documentation [9, lines 130-140] says it
"Reads and dispatches events on the libseat connection fd." The
timeout argument is "how long libseat might wait for data if none is
available": 0 means non-blocking, -1 means infinite, positive is
milliseconds. wlroots always uses 0 because the event loop already
did the blocking wait via epoll.

## 4. Weston's session handling

Weston has three launcher backends: `launcher-direct.c` [4],
`launcher-logind.c` [5], and (in newer versions) a libseat launcher.
The direct launcher is the one that does VT handling itself, and only
when running as root without logind. The code is short enough to quote
in full.

The VT signal handler is `vt_handler` [4, line 100]:

```c
static int
vt_handler(int signal_number, void *data)
{
        struct launcher_direct *launcher = data;
        struct weston_compositor *compositor = launcher->compositor;

        if (compositor->session_active) {
                compositor->session_active = 0;
                wl_signal_emit(&compositor->session_signal, compositor);
                drmDropMaster(launcher->drm_fd);
                ioctl(launcher->tty, VT_RELDISP, 1);
        } else {
                ioctl(launcher->tty, VT_RELDISP, VT_ACKACQ);
                drmSetMaster(launcher->drm_fd);
                compositor->session_active = 1;
                wl_signal_emit(&compositor->session_signal, compositor);
        }

        return 1;
}
```

Key observations:

1. Weston's `vt_handler` is installed via `wl_event_loop_add_signal`
   [4, line 205], not via `sigaction`. The wayland event loop uses
   `signalfd` on Linux, so the handler runs in the main thread, not in
   a signal-interrupt context. This means it can call any function,
   including ones that take locks or allocate memory. Ishizue uses
   `sigaction` and runs in a signal-interrupt context, which limits
   what it can safely do.

2. The handler uses ONE signal (`SIGRTMIN`) for both release and
   acquire. It distinguishes them by checking
   `compositor->session_active`: if currently active, this must be a
   release; if currently inactive, this must be an acquire. This is
   valid because the kernel serializes the signals: only one of
   relsig/acqsig can be pending at a time. Xorg [13, line 196] does
   the same with SIGUSR1. seatd [11, lines 34-35] uses two signals
   (SIGUSR1 for release, SIGUSR2 for acquire), which avoids the need
   to track state but uses one more signal slot.

3. The release order is: emit session_signal (so backends pause),
   then `drmDropMaster`, then `VT_RELDISP(1)`. The `drmDropMaster`
   must happen before the kernel switches the VT, otherwise the new
   VT owner cannot acquire master. The kernel does NOT enforce this
   order: `VT_RELDISP(1)` just acks the switch, and the kernel
   switches immediately. If you ack first and drop master second,
   there is a window where the new VT is active but you still hold
   master, and the new VT owner's `drmSetMaster` call will fail with
   EBUSY. Weston drops master first to close that window.

4. The acquire order is the reverse: `VT_RELDISP(VT_ACKACQ)` first,
   then `drmSetMaster`. The `VT_ACKACQ` ack tells the kernel we
   accepted the switch; the kernel switches us back. Then we call
   `drmSetMaster` to re-acquire. If we tried `drmSetMaster` first, it
   would fail because we are not yet the active VT (the kernel only
   lets the active VT's process hold master).

5. `drmSetMaster` and `drmDropMaster` are called directly from the
   handler. Weston does not have a session layer separate from the
   compositor process in this mode; the compositor IS the session.
   The `launcher->drm_fd` is opened by `launcher_direct_open` [4,
   line 217] which calls `is_drm_master(fd)` [4, line 62] to verify
   the fd has master. `is_drm_master` works by calling
   `drmGetMagic` + `drmAuthMagic`: if the fd is master, auth succeeds
   trivially; if not, it fails.

The `setup_tty` function [4, line 121] does the full VT initialization:

1. Open `/dev/ttyN` (or dup stdin if N is 0).
2. `VT_ACTIVATE` + `VT_WAITACTIVE` to switch to our VT.
3. `KDGKBMODE` to save the current keyboard mode.
4. `KDSKBMUTE` (or `KDSKBMODE K_OFF`) to disable kernel keyboard
   processing.
5. `KDSETMODE KD_GRAPHICS` to switch the VT from text mode to
   graphics mode. This stops the kernel from drawing text on the
   screen.
6. `VT_SETMODE` with `mode=VT_PROCESS`, `relsig=acqsig=SIGRTMIN`.
7. `wl_event_loop_add_signal(loop, SIGRTMIN, vt_handler, launcher)`
   to install the handler.

Steps 4 and 5 are things Ishizue does not do, and their absence
causes real bugs: without `KD_GRAPHICS`, the kernel keeps drawing
text on the VT, which corrupts the display. Without `K_OFF`, the
keyboard sends both kernel-processed input (which goes nowhere
useful) and libinput events (which is what we want), causing
double-processing.

The `launcher_direct_restore` function [4, line 251] undoes all of
this on shutdown: restore keyboard mode, set `KD_TEXT`, drop master,
set `VT_AUTO`. The order matters: drop master BEFORE `VT_AUTO`,
otherwise switching back to `VT_AUTO` mode might switch the VT to a
VT where another display server is running and fail to set master
[4, lines 263-265 comment].

## 5. Sway's integration with wlroots

Sway is a wlroots compositor. It does not implement its own session
layer. The server setup is in `sway/server.c` [6, line 269]:

```c
server->backend = wlr_backend_autocreate(server->wl_event_loop, &server->session);
```

`wlr_backend_autocreate` is the wlroots multi-backend factory. It
calls `wlr_session_create` internally, which calls
`libseat_session_init` as described in section 1. The session pointer
is returned to Sway via the out-parameter `&server->session`. Sway
stores it but never calls any libseat function directly.

Sway does not add anything on top of wlroots for VT handling. The
only VT-related code in Sway is `sway_change_vt` (in
`sway/input/keyboard.c`, not shown here) which calls
`wlr_session_change_vt(session, vt)`, which calls
`libseat_switch_session`. That is the full extent of Sway's VT
involvement.

The rest of Sway's setup is concerned with Wayland protocol globals,
renderers, allocators, and the desktop tree. The DRM fd, libseat fd,
and libinput fd are all managed by wlroots's event loop. Sway's own
event loop is the wayland display's event loop, which wlroots
registers its sources on.

This is the cleanest separation: the compositor process has zero
knowledge of DRM master, VT ioctls, or libseat internals. Everything
goes through `struct wlr_session` and the `events.active` signal.
Ishizue's design goal is similar (the library owns mechanism, the
Architect owns policy), but Ishizue's current implementation does
not achieve this separation because the DRM backend reaches into
libseat directly.

## 6. Hyprland's approach

Hyprland uses Aquamarine [12] as its rendering backend library.
Aquamarine is structurally similar to wlroots's backend layer but is
C++ and is its own project. The session layer is in
`src/backend/Session.cpp` [13].

Aquamarine's libseat integration is functionally identical to
wlroots's. The seat listener [13, lines 79-99]:

```cpp
static void libseatEnableSeat(struct libseat* seat, void* data) {
    auto PSESSION    = (Aquamarine::CSession*)data;
    PSESSION->active = true;
    if (PSESSION->libinputHandle)
        libinput_resume(PSESSION->libinputHandle);
    PSESSION->events.changeActive.emit();
}

static void libseatDisableSeat(struct libseat* seat, void* data) {
    auto PSESSION    = (Aquamarine::CSession*)data;
    PSESSION->active = false;
    if (PSESSION->libinputHandle)
        libinput_suspend(PSESSION->libinputHandle);
    PSESSION->events.changeActive.emit();
    libseat_disable_seat(PSESSION->libseatHandle);
}
```

The pattern matches wlroots: set `active = false`, suspend libinput,
emit the change signal, then call `libseat_disable_seat`. Aquamarine
also explicitly calls `libinput_suspend` and `libinput_resume` here,
which wlroots does in the libinput backend's session-signal handler
[14, lines 185-189]. The effect is the same: libinput is told to
stop reading from evdev devices while the seat is disabled, and to
re-scan them on resume.

The DRM backend (`src/backend/drm/DRM.cpp` [15]) listens on
`session->events.changeActive` [15, line 62]:

```cpp
listeners.sessionActivate = backend->session->events.changeActive.listen([this] {
    if (backend->session->active) {
        // session got activated, we need to restore
        restoreAfterVT();
    }
});
```

`restoreAfterVT` [15, line 395] does the same thing as wlroots's
`handle_session_active` resume branch: clear stale page-flip state,
rescan connectors, re-commit each output's state. The Aquamarine
version has a longer comment about why the stale page-flip state
needs clearing [15, lines 398-414]: during S3 suspend, the display
hardware powers off and pending page-flip completion events are
lost. Without clearing the `isPageFlipPending` flag, every subsequent
commit is rejected with "Cannot commit when a page-flip is
awaiting." Ishizue has a similar issue documented in
`doc/research/drm-backend-audit.md` but has not fixed it.

Aquamarine does NOT call `drmSetMaster` or `drmDropMaster` on VT
switch. The only place it calls `drmDropMaster` is when generating a
non-master fd for client access [15, line 1316]:

```cpp
if (drmIsMaster(fd) && drmDropMaster(fd) < 0) {
    backend->log(AQ_LOG_ERROR, "drm: couldn't drop master from duped fd");
    close(fd);
    return -1;
}
```

This is for the "drm lease" or "client fd" path, where a client
needs a DRM fd without master privileges. It is not part of the VT
switch flow.

The `CSession::attempt` function [13, line 291] is the entry point.
It calls `libseat_open_seat`, gets the seat name, dispatches pending
events, sets up udev, sets up libinput via
`libinput_udev_create_context` and `libinput_udev_assign_seat`. The
libinput fd is added to the event loop via `SPollFD` [13, line 520]:

```cpp
makeShared<SPollFD>(libseat_get_fd(libseatHandle), [this](){ dispatchLibseatEvents(); }),
```

`dispatchLibseatEvents` calls `libseat_dispatch(libseatHandle, 0)`
[13, line 502]. Same pattern as wlroots.

Hyprland itself (the compositor binary, not Aquamarine) does not add
any VT handling. It links Aquamarine and listens on
`backend->session->events.changeActive` for its own purposes
(presumably to pause rendering and re-paint on resume). No direct
libseat, DRM master, or VT ioctl calls.

## 7. The kernel DRM master API

The kernel DRM master documentation is in
`Documentation/gpu/drm-uapi.rst` [7]. The relevant section is "Primary
Nodes, DRM Master and Authentication":

> struct drm_master is used to track groups of clients with open
> primary device nodes. For every struct drm_file which has had at
> least once successfully became the device master (either through
> the SET_MASTER IOCTL, or implicitly through opening the primary
> device node when no one else is the current master that time)
> there exists one drm_master. This is noted in drm_file.is_master.
> All other clients have just a pointer to the drm_master they are
> associated with.
>
> In addition only one drm_master can be the current master for a
> drm_device. It can be switched through the DROP_MASTER and
> SET_MASTER IOCTL, or implicitly through closing/opening the
> primary device node.

The rules, distilled:

1. Opening a primary DRM node (`/dev/dri/cardN`) grants master
   implicitly IF no one else is currently master. If someone else is
   master, the new fd is a non-master client.

2. `DRM_IOCTL_SET_MASTER` (libdrm wrapper: `drmSetMaster`) [16, line
   2634] promotes the calling fd to master. The kernel's
   `drm_setmaster` (in `drivers/gpu/drm/drm_auth.c`) returns
   `-EINVAL` if the fd is already the current master, and otherwise
   assigns master to the calling fd. The caller needs `CAP_SYS_ADMIN`
   or root; otherwise `-EPERM`. If another fd is currently master,
   `drmSetMaster` from a privileged fd transfers master to the new fd
   and the previous master loses master status. From a
   non-privileged fd it fails. This is how `seatd` can take master
   from a stale process.

3. `DRM_IOCTL_DROP_MASTER` (libdrm wrapper: `drmDropMaster`) [16, line
   2639] demotes the calling fd from master. It fails with `-EINVAL`
   if the fd is not currently master, and with `-EPERM` if the caller
   lacks `CAP_SYS_ADMIN`. After `drmDropMaster`, the fd is still open
   but is no longer master.

4. Only one fd at a time can be the current master for a given
   `drm_device`. Multiple fds can hold `is_master = true` (each is a
   separate `drm_master`), but only one of them is the "current"
   master. The non-current masters are "lessees" or "lessors" in the
   DRM leasing terminology.

5. When the master process dies (its fds are closed), the kernel
   automatically drops master. The next process to call `open()` on
   the primary node gets master implicitly. This is the "implicitly
   through closing/opening the primary device node" rule.

6. The `drmIsMaster` libdrm helper (not in the snippet I fetched, but
   in `xf86drm.c`) calls `drmGetMagic` + `drmAuthMagic` to test
   whether the fd is the current master. If `drmAuthMagic` succeeds,
   the fd is master; if it fails with `-EACCES` or `-EPERM`, the fd
   is not master. Weston's `is_drm_master` [4, line 62] uses this
   trick.

For Ishizue, the practical implications:

- `drmSetMaster(fd)` at init will succeed if no one else is master,
  fail with `EPERM` if we lack `CAP_SYS_ADMIN` (e.g., running as a
  regular user without seatd/logind), or fail with `EINVAL` if we
  are already master (which can happen if `libseat_open_device`
  returned a master fd).
- `drmDropMaster(fd)` on a non-master fd fails with `EINVAL`. This
  is harmless but spams logs.
- When using libseat, the fd returned by `libseat_open_device` may
  or may not be master, depending on the backend. With the seatd
  daemon, the daemon opens the device with master and the fd passed
  to the compositor shares the `struct file` (and thus the
  `drm_file`), so the compositor's fd IS master. With the logind
  backend, logind calls `TakeControl("drm", ...)` which grants
  master, and the fd passed via `sd_bus_get_fd` or similar is also
  master.

The Ishizue code at `isz_drm.c:779` calls `drmSetMaster(fd)` after
`open_primary_drm_node` returns. If libseat gave us a master fd
already, this returns `EINVAL` and Ishizue aborts with
`ISZ_ERR_DRM_MASTER`. That is a bug: the fd is usable, we should
not abort.

## 8. The kernel VT API

The `ioctl_vt(2)` man page [17] documents the VT ioctls. The
relevant ones:

- `VT_GETMODE`: returns the current `struct vt_mode`, which has
  `mode` (VT_AUTO or VT_PROCESS), `waitv`, `relsig`, `acqsig`,
  `frsig` (unused).
- `VT_SETMODE`: sets the mode. In `VT_PROCESS` mode, the kernel
  sends `relsig` when another VT is requested and `acqsig` when this
  VT is re-activated. The process must ack with `VT_RELDISP` before
  the switch completes.
- `VT_RELDISP`: ack a VT switch. The argument is:
  - `0`: pending switch-from is NOT OK. The kernel aborts the switch.
  - `1`: pending switch-from is OK. The kernel switches the VT.
  - `VT_ACKACQ` (= 2): completed switch-to is OK. Sent in response
    to `acqsig`.
- `VT_ACTIVATE`: switch to a specific VT.
- `VT_WAITACTIVE`: block until a specific VT is activated.
- `VT_GETSTATE`: returns the current VT number and a bitmask of
  in-use VTs.

The kernel source comment in `drivers/tty/vt/vt_ioctl.c` [18, lines
866-878] is more explicit about `VT_RELDISP` semantics:

```c
/*
 * If a vt is under process control, the kernel will not switch to it
 * immediately, but postpone the operation until the process calls this
 * ioctl, allowing the switch to complete.
 *
 * According to the X sources this is the behavior:
 *  0:  pending switch-from not OK
 *  1:  pending switch-from OK
 *  2:  completed switch-to OK
 */
case VT_RELDISP:
```

The order of operations on switch away:

1. User presses Ctrl+Alt+F3.
2. Kernel checks the current VT's mode. If `VT_PROCESS`, kernel
   sends `relsig` to the process that called `VT_SETMODE` and waits.
3. The process (seatd daemon, weston, or xorg) receives the signal.
4. The process does its cleanup: drop DRM master, pause rendering,
   etc.
5. The process calls `ioctl(tty_fd, VT_RELDISP, 1)`.
6. Kernel switches the VT.

If the process does not call `VT_RELDISP`, the kernel blocks the
switch indefinitely. The user sees a frozen screen. This is exactly
the bug Ishizue had: the libseat disable_seat callback was not
firing because the libseat fd was not in the epoll set, so
`libseat_disable_seat` was never called, so the seatd daemon never
received the ack it needed, so the daemon never called
`VT_RELDISP`, so the kernel blocked the switch.

The order of `drmDropMaster` vs `VT_RELDISP` on switch away:
`drmDropMaster` first, then `VT_RELDISP(1)`. The kernel does not
enforce this order, but if you ack first and drop master second,
there is a window where the new VT is active but you still hold
master. The new VT owner's `drmSetMaster` call will fail with
`-EBUSY` (or `-EPERM` depending on kernel version and privileges).
Weston [4, lines 108-109] does it in the right order. seatd does it
in the right order (the daemon drops master before acking the
kernel, see section 9). Ishizue's direct path does it in the right
order (`isz_drm.c:261-267`).

On switch back, the order is reversed: `VT_RELDISP(VT_ACKACQ)`
first, then `drmSetMaster`. The kernel switches us back to our VT,
then we re-acquire master. If we tried `drmSetMaster` first, it
would fail because we are not yet the active VT. Weston [4, lines
111-112] does it in the right order. Ishizue's direct path does it
in the right order (`isz_drm.c:268-281`).

The signal choice: `relsig` and `acqsig` can be any signal the
kernel can deliver. Common choices:

- Weston: `SIGRTMIN` for both [4, lines 196-197]. Distinguished by
  checking `compositor->session_active`.
- Xorg: `SIGUSR1` for both [13, lines 198-200]. Same disambiguation
  trick.
- seatd: `SIGUSR1` for release, `SIGUSR2` for acquire [10, lines
  200-201]. No disambiguation needed.

Ishizue's direct path uses `SIGUSR1` for release and `SIGUSR2` for
acquire (`isz_drm.c:225-226`), matching seatd. This is fine.

The signal handler installation matters. `sigaction` with
`sa_flags = 0` (Ishizue's choice) means:

- No `SA_RESTART`: blocking syscalls return `EINTR` when the signal
  fires. The dispatch loop must handle `EINTR` gracefully. Ishizue
  does (`isz_lifecycle.c:364`).
- No `SA_RESETHAND`: the handler stays installed. Some older SysV
  systems reset the handler to `SIG_DFL` after one delivery; Linux
  does not. Ishizue is fine here.
- No `SA_NODEFER`: the signal is blocked during the handler. This
  prevents re-entrant delivery. Fine.

A better choice is `signalfd`, which delivers signals via a file
descriptor that can be added to epoll. This is what wayland's
`wl_event_loop_add_signal` uses internally, and what seatd's
`poller_add_signal` uses. With signalfd, the signal handler runs in
normal thread context, not signal-interrupt context, so it can call
any function. Ishizue uses `sigaction` and runs in signal-interrupt
context, which limits what the handler can safely do. Ishizue's
handler only sets a `sig_atomic_t` flag (`isz_drm.c:196-204`), which
is async-signal-safe, so this is not a bug. But it is a limitation:
the actual `VT_RELDISP` call has to happen later, in
`vt_dispatch`, which means there is a delay between the signal and
the ack. The delay is bounded by one dispatch tick, which is
typically sub-millisecond, but it is non-zero.

## 9. libseat internals

The libseat API is in `include/libseat.h` [9]. The implementation is
in `libseat/libseat.c` [19] and `libseat/backend/seatd.c` [20].
There are three backends, compiled in conditionally:

- `seatd`: connects to a running `seatd` daemon over a Unix socket.
  The daemon handles VT, DRM master, and device opening.
- `logind`: uses systemd's logind D-Bus API (`TakeControl`,
  `TakeDevice`, `PauseDevice`, `ResumeDevice`). logind handles VT
  via the kernel VT API internally.
- `builtin`: forks a `seatd` daemon in a child process and connects
  to it over a socketpair. The child runs the same `seatd` server
  code as the standalone daemon.

`libseat_open_seat` [19, line 37] tries the backends in order. If
the `LIBSEAT_BACKEND` environment variable is set, only that backend
is tried. Otherwise, each enabled backend is tried in the order:
seatd, logind, builtin. The first one that succeeds wins. If all
fail, `libseat_open_seat` returns NULL with `errno = ENOSYS` [19,
line 80].

The `builtin` backend [20, line 677] is interesting:

```c
static struct libseat *builtin_open_seat(const struct libseat_seat_listener *listener, void *data) {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == -1) {
                ...
        }
        pid_t pid = fork();
        if (pid == -1) { ... }
        else if (pid == 0) {
                close(fds[1]);
                int fd = fds[0];
                struct server server = {0};
                if (server_init(&server) == -1) { ... }
                if (server_add_client(&server, fd) == -1) { ... }
                while (server.running) {
                        if (poller_poll(&server.poller) == -1) { ... }
                }
                ...
                _exit(res);
        } else {
                close(fds[0]);
                int fd = fds[1];
                return _open_seat(listener, data, fd);
        }
}
```

The builtin backend forks a child, the child runs `server_init` +
`server_add_client` + the poll loop, the parent gets the other end
of the socketpair and treats it as a seatd connection. So even
"builtin" has a separate process handling VT. The compositor never
handles VT itself with any libseat backend.

The seatd daemon's VT handling is in `seatd/server.c` [11, lines
34-37]:

```c
if (poller_add_signal(&server->poller, SIGUSR1, server_handle_vt_rel, server) == NULL ||
    poller_add_signal(&server->poller, SIGUSR2, server_handle_vt_acq, server) == NULL ||
    poller_add_signal(&server->poller, SIGINT, server_handle_kill, server) == NULL ||
    poller_add_signal(&server->poller, SIGTERM, server_handle_kill, server) == NULL) {
```

`poller_add_signal` uses `signalfd` (or `eventfd` fallback) so the
signal is delivered as a readable event on a fd, not as a
signal-interrupt. The handlers `server_handle_vt_rel` and
`server_handle_vt_acq` [11, lines 80-102] call `seat_vt_release` and
`seat_vt_activate` respectively.

`seat_vt_release` [8, line 742]:

```c
int seat_vt_release(struct seat *seat) {
        if (!seat->vt_bound) { ... }
        seat_update_vt(seat);
        if (seat->active_client != NULL) {
                seat_disable_client(seat->active_client);
        }
        vt_ack(seat, true);
        seat->cur_vt = -1;
        return 0;
}
```

`seat_disable_client` [8, line 593] deactivates all client devices
(but does NOT close them, see the comment at line 604: "certain
device fds, such as for DRM, must maintain the exact same file
description for their contexts to remain valid") and sends
`SERVER_DISABLE_SEAT` to the compositor. Then `vt_ack(seat, true)`
calls `terminal_ack_release` [10, line 223] which calls
`ioctl(fd, VT_RELDISP, 1)`. The kernel switches the VT.

The compositor receives `SERVER_DISABLE_SEAT` via its libseat fd.
The libseat seatd backend's `execute_events` [20, line 200] invokes
the compositor's `disable_seat` callback. The compositor acks by
calling `libseat_disable_seat`, which sends `CLIENT_DISABLE_SEAT`
to the daemon. The daemon's `seat_ack_disable_client` [8, line 631]
sets the client state to `CLIENT_DISABLED` and may activate the
next client.

The order is: daemon sends `SERVER_DISABLE_SEAT`, daemon acks
kernel (`VT_RELDISP(1)`), kernel switches VT, compositor processes
`SERVER_DISABLE_SEAT` (whenever its event loop next runs), compositor
acks daemon (`libseat_disable_seat`). The compositor's ack is for
the daemon's bookkeeping, not for the kernel. The kernel has already
switched by the time the compositor acks.

This means the compositor's `disable_seat` callback runs AFTER the
VT has switched. The compositor cannot prevent the switch; it can
only clean up (pause rendering, suspend libinput). Ishizue's
`drm_disable_seat` callback (`isz_drm.c:701`) calls `drmDropMaster`
at this point, which is redundant: the daemon has already dropped
master on its fd, and the compositor's fd shares the same
`struct file`, so master is already gone. The call is harmless
(returns `EINVAL` on a non-master fd) but adds noise.

On switch back, `seat_vt_activate` [8, line 723] calls
`vt_ack(seat, false)` which calls `terminal_ack_acquire` [10, line
233] which calls `ioctl(fd, VT_RELDISP, VT_ACKACQ)`. Then
`seat_activate` finds the right client (the one whose session
matches the current VT) and calls `seat_open_client`, which
re-activates devices and sends `SERVER_ENABLE_SEAT`. The compositor's
`enable_seat` callback fires; it re-commits KMS state.

## 10. Direct VT handling without libseat

When neither logind nor seatd is available, a display server must
handle VT itself. The two reference implementations are Weston's
`launcher-direct.c` [4] and Xorg's `hw/xfree86/os-support/linux/lnx_init.c`
[13] plus `hw/xfree86/os-support/shared/VTsw_usl.c` [21].

Xorg's `lnx_init.c` does the setup in `xf86OpenConsole` [13, lines
183-211]:

```c
if (!xf86Info.ShareVTs) {
    ...
    switch_to(xf86Info.vtno, "xf86OpenConsole");

    SYSCALL(ret = ioctl(xf86Info.consoleFd, VT_GETMODE, &VT));
    if (ret < 0)
        FatalError("xf86OpenConsole: VT_GETMODE failed %s\n", strerror(errno));

    signal(SIGUSR1, xf86VTRequest);

    VT.mode = VT_PROCESS;
    VT.relsig = SIGUSR1;
    VT.acqsig = SIGUSR1;

    SYSCALL(ret = ioctl(xf86Info.consoleFd, VT_SETMODE, &VT));
    if (ret < 0)
        FatalError("xf86OpenConsole: VT_SETMODE VT_PROCESS failed: %s\n", strerror(errno));

    SYSCALL(ret = ioctl(xf86Info.consoleFd, KDSETMODE, KD_GRAPHICS));
    ...
    SYSCALL(ioctl(xf86Info.consoleFd, KDGKBMODE, &tty_mode));
    SYSCALL(ret = ioctl(xf86Info.consoleFd, KDSKBMODE, K_OFF));
    ...
}
```

Notes:

- Xorg uses `SIGUSR1` for both `relsig` and `acqsig` [13, lines
  199-200], like Weston uses `SIGRTMIN` for both. The signal handler
  `xf86VTRequest` [21, line 39] just sets a flag:

```c
void
xf86VTRequest(int sig)
{
    signal(sig, (void (*)(int)) xf86VTRequest);
    xf86Info.vtRequestsPending = TRUE;
    return;
}
```

  The handler re-installs itself via `signal(sig, ...)` [21, line
  41]. This is for portability to SysV systems that reset the
  handler to `SIG_DFL` after delivery. Linux does not reset, so the
  re-install is a no-op on Linux. Ishizue does not re-install
  (`isz_drm.c:236-247`), which is fine on Linux but would break on
  old SysV.

- The actual `VT_RELDISP` call is in `VTsw_usl.c` [21, lines 56-73]:

```c
Bool
xf86VTSwitchAway(void)
{
    xf86Info.vtRequestsPending = FALSE;
    if (ioctl(xf86Info.consoleFd, VT_RELDISP, 1) < 0)
        return FALSE;
    else
        return TRUE;
}

Bool
xf86VTSwitchTo(void)
{
    xf86Info.vtRequestsPending = FALSE;
    if (ioctl(xf86Info.consoleFd, VT_RELDISP, VT_ACKACQ) < 0)
        return FALSE;
    else
        return TRUE;
}
```

  These are called from Xorg's main loop when
  `xf86Info.vtRequestsPending` is true. The signal handler only
  sets the flag; the main loop polls the flag and does the actual
  `VT_RELDISP`. This is the same pattern as Ishizue's
  `g_vt_switch_away` flag (`isz_drm.c:194-204`) checked by
  `vt_dispatch` (`isz_drm.c:254-282`).

- Xorg does NOT call `drmDropMaster` from the VT signal handler or
  from `xf86VTSwitchAway`. The DRM master drop happens in the DDX
  driver's `LeaveVT` function (e.g., `modesetting_LeaveVT` in
  `xf86-video-modesetting`). The DDX driver is loaded by Xorg at
  runtime and is responsible for the DRM-specific cleanup. This is a
  cleaner separation than Weston's `vt_handler` which calls
  `drmDropMaster` directly.

- Xorg calls `KDSETMODE KD_GRAPHICS` [13, line 208] and `KDSKBMODE
  K_OFF` [13, line 218] in `xf86OpenConsole`. These are required to
  stop the kernel from drawing text and processing keyboard input.
  Ishizue does neither, which causes two visible bugs: text-mode
  cursor blink shows through the compositor's output, and keyboard
  input is doubled (kernel-processed input goes to the TTY,
  libinput input goes to the compositor).

- The teardown in `xf86CloseConsole` [13, lines 286-289] restores
  `VT_AUTO` mode. Ishizue does the same in `teardown_vt_signals`
  (`isz_drm.c:297-310`).

The SIGUSR1/SIGUSR2 pattern (or SIGRTMIN) is:

1. Open the VT device (`/dev/ttyN` where N is the current VT
   number, or `/dev/tty0` which is the alias for the current VT).
2. `VT_GETMODE` to read the current mode.
3. Install signal handler(s) for the chosen signal(s).
4. `VT_SETMODE` with `mode=VT_PROCESS`, `relsig` and `acqsig` set
   to the chosen signal(s).
5. `KDSETMODE KD_GRAPHICS` to switch to graphics mode.
6. `KDSKBMODE K_OFF` (or `KDSKBMUTE 1`) to disable kernel keyboard
   processing.
7. On switch away: drop DRM master, then `VT_RELDISP(1)`.
8. On switch back: `VT_RELDISP(VT_ACKACQ)`, then re-acquire DRM
   master.
9. On teardown: restore `KDSKBMODE`, restore `KDSETMODE KD_TEXT`,
   drop DRM master, `VT_SETMODE VT_AUTO`.

Ishizue's direct path does steps 1, 3, 4, 7, 8, 9 but skips 2, 5,
6. The missing steps are the source of several bugs documented in
section 12.

## 11. Input device management

All four modern compositors (wlroots, Aquamarine, Weston, Sway) use
libinput for input device management. libinput provides a
seat-aware API that handles device discovery, hotplug, and event
processing. The compositor's job is to:

1. Create a libinput context.
2. Assign it to a seat.
3. Add `libinput_get_fd(context)` to the event loop.
4. On readable, call `libinput_dispatch(context)` and drain
   `libinput_get_event(context)` in a loop.
5. On VT switch away, call `libinput_suspend(context)` to close
   evdev fds.
6. On VT switch back, call `libinput_resume(context)` to re-open
   them.

The wlroots libinput backend is in `backend/libinput/backend.c` [14].
The context is created in `backend_start` [14, line 89]:

```c
backend->libinput_context = libinput_udev_create_context(&libinput_impl,
        backend, backend->session->udev);
if (!backend->libinput_context) { ... }

if (libinput_udev_assign_seat(backend->libinput_context,
                backend->session->seat) != 0) { ... }
```

`libinput_udev_create_context` takes a `libinput_interface` with
`open_restricted` and `close_restricted` callbacks [14, lines 17-46].
These callbacks are how libinput asks the compositor to open and
close evdev device files. wlroots's implementation routes through
`wlr_session_open_file` and `wlr_session_close_file`, so evdev fds
are opened via libseat (or logind). This means evdev fds are
session-managed: on VT switch away, the session layer revokes them;
on switch back, libinput re-opens them via the callbacks.

The fd is added to the event loop at line 127:

```c
backend->input_event = wl_event_loop_add_fd(event_loop, libinput_fd,
                WL_EVENT_READABLE, handle_libinput_readable, backend);
```

The handler [14, line 48]:

```c
static int handle_libinput_readable(int fd, uint32_t mask, void *_backend) {
        struct wlr_libinput_backend *backend = _backend;
        int ret = libinput_dispatch(backend->libinput_context);
        if (ret != 0) { ... }
        struct libinput_event *event;
        while ((event = libinput_get_event(backend->libinput_context))) {
                handle_libinput_event(backend, event);
                libinput_event_destroy(event);
        }
        return 0;
}
```

The session-signal handler [14, line 176]:

```c
static void session_signal(struct wl_listener *listener, void *data) {
        struct wlr_libinput_backend *backend =
                wl_container_of(listener, backend, session_signal);
        struct wlr_session *session = backend->session;

        if (!backend->libinput_context) {
                return;
        }

        if (session->active) {
                libinput_resume(backend->libinput_context);
        } else {
                libinput_suspend(backend->libinput_context);
        }
}
```

This is the same pattern as Aquamarine [13, lines 79-94]: suspend on
disable, resume on enable. The suspend call closes all evdev fds
internally (libinput keeps the device objects but closes their fds).
The resume call re-opens them via the `open_restricted` callback,
which goes through the session layer.

Hotplug is handled by libinput itself: when udev reports a new
device, libinput notices (via its udev monitoring) and emits a
`LIBINPUT_EVENT_DEVICE_ADDED` event on the next dispatch. The
compositor's `handle_libinput_event` switch statement has a case
for `LIBINPUT_EVENT_DEVICE_ADDED` that creates a `wlr_input_device`
and emits the appropriate `new_pointer`/`new_keyboard`/etc. signal.
Removal is symmetric via `LIBINPUT_EVENT_DEVICE_REMOVED`.

Ishizue's input layer is in `src/input/`. The session management is
in `isz_session.c` (91 lines). It creates a libseat session and
emits `ISZ_EVENT_SESSION_ACTIVE`/`INACTIVE` on enable/disable. It
does NOT create a libinput context or handle libinput fds. The
libinput wiring is presumably in another file (not read for this
task). The session-active/inactive events are listened for by the
DRM backend (`isz_drm.c:1115-1145`) which calls
`drmDropMaster`/`drmSetMaster`. There is no listener that calls
`libinput_suspend`/`libinput_resume` on session events. If
libinput is wired up at all, it does not suspend on VT switch,
which means evdev fds stay open across the switch. With libseat,
the session layer revokes the fds anyway (via `seat_deactivate_device`
in the daemon), so libinput's view of them becomes stale. On
resume, libinput does not know to re-open them. This would cause
input to stop working after a VT switch.

## 12. Comparison with our implementation

This section maps every difference found in sections 1-11 onto
Ishizue's code. Each difference is a potential bug source.

### 12.1. DRM master acquisition

Ishizue (`isz_drm.c:779`):

```c
if (drmSetMaster(fd) != 0) {
    ...
    return ISZ_ERR_DRM_MASTER;
}
```

wlroots [2] and Aquamarine [15] do NOT call `drmSetMaster` at init.
The fd returned by `libseat_open_device` already has master
privileges because the seatd daemon opened it with master and the
fd shares the `struct file`. Calling `drmSetMaster` on an
already-master fd returns `EINVAL`, which Ishizue treats as a fatal
error.

Fix: drop the `drmSetMaster` call when libseat is in use, OR ignore
`EINVAL` from `drmSetMaster` (the fd is already master). The
direct-open path (no libseat) still needs `drmSetMaster` because
`open()` only grants master implicitly when no one else is master.

### 12.2. VT signal handler installation

Ishizue (`isz_drm.c:206-252`): `sigaction` with `sa_flags = 0`,
handler sets a `sig_atomic_t` flag. The flag is polled in
`vt_dispatch` which is called from `isz_dispatch` on every tick.

Weston [4, line 205]: `wl_event_loop_add_signal`, which uses
`signalfd`. Handler runs in main thread context, can call any
function.

seatd [11, lines 34-37]: `poller_add_signal`, also `signalfd`-based.

Xorg [13, line 196]: `signal(SIGUSR1, xf86VTRequest)`, handler sets
a flag.

Ishizue's pattern matches Xorg: signal handler sets a flag, main
loop polls. This is async-signal-safe and works. The downside is
that the `VT_RELDISP` call is delayed by up to one dispatch tick.
Weston and seatd avoid this delay by using signalfd, which delivers
the signal as a readable event on a fd in the event loop. The
handler runs in normal thread context and can call `VT_RELDISP`
immediately.

Fix: switch to `signalfd` for the direct-VT path. This is a
modernization, not a bug fix. The current code works but has
higher latency.

### 12.3. Missing KD_GRAPHICS and K_OFF

Ishizue's `setup_vt_signals` (`isz_drm.c:206-252`) does:

1. Open `/dev/tty` (or `/dev/tty0` fallback).
2. `VT_SETMODE` with `VT_PROCESS`, `relsig=SIGUSR1`,
   `acqsig=SIGUSR2`.
3. Install signal handlers.

It does NOT call:

- `KDSETMODE KD_GRAPHICS`: switch the VT from text mode to graphics
  mode. Without this, the kernel keeps drawing text on the VT,
  including the cursor blink. The compositor's output is corrupted
  by text-mode drawing.
- `KDSKBMODE K_OFF` (or `KDSKBMUTE 1`): disable kernel keyboard
  processing. Without this, the keyboard sends both kernel-processed
  input (which goes to the TTY's input queue, where it is
  interpreted as TTY escape sequences) and libinput events (which go
  to the compositor). The user sees doubled input and the TTY's
  input queue fills up.
- `KDGKBMODE` to save the current keyboard mode for later restore.
- `VT_ACTIVATE` + `VT_WAITACTIVE` to switch to our VT explicitly.

Weston [4, lines 152-181] does all four. Xorg [13, lines 208-218]
does `KDSETMODE` and `KDSKBMODE`. seatd's `terminal.c` [10] has
`terminal_set_keyboard` and `terminal_set_graphics` helpers that
are called by the daemon.

Fix: add `KDSETMODE KD_GRAPHICS` and `KDSKBMODE K_OFF` to
`setup_vt_signals`. Save the old keyboard mode and restore it in
`teardown_vt_signals`. Add `VT_ACTIVATE` + `VT_WAITACTIVE` to
switch to our VT on init.

### 12.4. Opening /dev/tty vs /dev/ttyN

Ishizue (`isz_drm.c:208-212`):

```c
st->vt_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
if (st->vt_fd < 0) {
    st->vt_fd = open("/dev/tty0", O_RDWR);
}
```

`/dev/tty` is the controlling TTY of the calling process. If the
process has no controlling TTY (e.g., started from a daemon
manager), `open("/dev/tty")` fails. The fallback to `/dev/tty0`
opens the current foreground VT, which is not necessarily the VT
the process was started on.

Weston [4, line 137] opens `/dev/ttyN` where N is the explicit VT
number passed via `--tty`. Xorg [13, line 151] opens `/dev/ttyN`
where N is determined by `VT_OPENQRY` (find a free VT) or by the
`-vt` command-line argument. seatd's `terminal_open` [10, line 154]
opens `/dev/ttyN` for a specific N.

The reliable pattern is: determine the VT number (via `VT_OPENQRY` or
`VT_GETSTATE` on stdin), open `/dev/ttyN` for that N. Ishizue's
`/dev/tty` approach works when run from a TTY login but breaks
when run from a display manager or systemd unit.

Fix: use `VT_GETSTATE` on stdin (or `/dev/tty0`) to find the
current VT number, then open `/dev/ttyN` for that N. Fall back to
`/dev/tty0` only if `VT_GETSTATE` fails.

### 12.5. Redundant drmDropMaster on libseat path

Ishizue's `drm_disable_seat` (`isz_drm.c:701-714`):

```c
static void drm_disable_seat(struct libseat *seat, void *userdata) {
    ...
    st->session_active = false;
    isz_log_internal(ISZ_LOG_INFO, "drm: VT switch away, dropping master");
    if (drmDropMaster(st->drm_fd) != 0) {
        isz_log_internal(ISZ_LOG_WARN, "drm: drmDropMaster failed: %s",
                         strerror(errno));
    }
    libseat_disable_seat(st->seat);
}
```

This calls `drmDropMaster` before `libseat_disable_seat`. But the
seatd daemon has already dropped master on its fd (which shares
the `struct file` with the compositor's fd) before sending
`SERVER_DISABLE_SEAT`. So `drmDropMaster` here returns `EINVAL`
(not master) and logs a warning. The warning is noise.

wlroots [1, lines 32-37] does NOT call `drmDropMaster` in
`handle_disable_seat`. Neither does Aquamarine [13, lines 87-94].
Both rely on the session layer to handle master.

Fix: remove the `drmDropMaster` call from `drm_disable_seat`. The
session layer handles it. Keep the `libseat_disable_seat` ack.

### 12.6. Redundant drmSetMaster on libseat path

Ishizue's `drm_enable_seat` (`isz_drm.c:716-728`):

```c
static void drm_enable_seat(struct libseat *seat, void *userdata) {
    ...
    if (drmSetMaster(st->drm_fd) != 0) {
        isz_log_internal(ISZ_LOG_ERROR, "drm: drmSetMaster failed: %s",
                         strerror(errno));
        isz_backend_set_error(st->backend, ISZ_ERR_DRM_MASTER);
        return;
    }
    st->session_active = true;
}
```

This calls `drmSetMaster` on resume. But the seatd daemon has
already re-acquired master on its fd before sending
`SERVER_ENABLE_SEAT`. So `drmSetMaster` here either succeeds
(no-op, already master) or fails with `EINVAL` (already master).
If it fails, Ishizue surfaces `ISZ_ERR_DRM_MASTER` and the
compositor aborts. That is wrong: the fd IS master, the abort is
spurious.

wlroots [2, lines 98-126] does NOT call `drmSetMaster` in
`handle_session_active`. Aquamarine [15, line 395] calls
`restoreAfterVT` which re-commits state but does not call
`drmSetMaster`.

Fix: remove the `drmSetMaster` call from `drm_enable_seat`. The
session layer handles master re-acquisition. If the re-acquire
fails, the session layer (libseat) will not send `SERVER_ENABLE_SEAT`,
so `drm_enable_seat` will not fire.

### 12.7. Double session-active/inactive handling

Ishizue has TWO listeners for session events:

1. The libseat seat listener in `isz_drm.c` (`drm_disable_seat`,
   `drm_enable_seat`) which fires inline from `libseat_dispatch`.
2. The `ISZ_EVENT_SESSION_ACTIVE`/`INACTIVE` listener in
   `isz_drm.c:1168-1180` (`isz_drm_session_active_listener`,
   `isz_drm_session_inactive_listener`) which fires from
   `isz_session.c`'s `session_enable_seat`/`session_disable_seat`.

On VT switch away with libseat, BOTH fire:

1. `drm_disable_seat` fires: `drmDropMaster` (redundant, see 12.5),
   `libseat_disable_seat` (ack).
2. `session_disable_seat` in `isz_session.c:42` fires: emits
   `ISZ_EVENT_SESSION_INACTIVE`.
3. `isz_drm_session_inactive_listener` catches the event and calls
   `isz_drm_on_session_inactive` (`isz_drm.c:1115-1126`): another
   `drmDropMaster` (redundant, returns `EINVAL`).

On switch back, the same double-firing happens with
`drmSetMaster` (redundant, see 12.6).

This is not a correctness bug (the second call is a no-op), but it
is a design smell. wlroots has ONE listener (`handle_session_active`
on `session->events.active`) and the DRM backend's reaction is in
that one listener. Ishizue should consolidate: pick one path (either
the libseat seat listener or the ISZ_EVENT_SESSION_* listener) and
put the master drop/acquire there.

Fix: remove the libseat seat listener in `isz_drm.c` and rely on
the `ISZ_EVENT_SESSION_*` events. OR remove the
`ISZ_EVENT_SESSION_*` listener in `isz_drm.c` and put the master
drop/acquire directly in the libseat seat listener. The first
option is cleaner because it lets the Architect also listen on
`ISZ_EVENT_SESSION_*` for their own purposes.

### 12.8. Direct-VT path not properly guarded

Ishizue's `isz_drm_init` (`isz_drm.c:802-804`):

```c
if (!st->seat) {
    setup_vt_signals(st);
}
```

The direct-VT path is only set up when `libseat_open_seat` failed.
But the direct-VT path's `vt_dispatch` (`isz_drm.c:254-282`) is
called unconditionally from `isz_drm_read_events` (`isz_drm.c:928`)
and from `isz_drm_vt_dispatch` (`isz_drm.c:289`) which is called
from `isz_dispatch` (`isz_lifecycle.c:386`).

`vt_dispatch` has a guard: `if (st->vt_fd < 0) return;` (line 255).
So when libseat is in use, `vt_fd` is -1 and `vt_dispatch` is a
no-op. Good.

But the signal handlers for SIGUSR1/SIGUSR2 are NOT installed when
libseat is in use (because `setup_vt_signals` was skipped). So if
SIGUSR1 or SIGUSR2 arrives from another source (e.g., a parent
process), the default action (terminate) is taken. This is a
latent bug: if the compositor is started under a wrapper that
sends SIGUSR1 for any reason, the compositor dies.

Fix: block SIGUSR1 and SIGUSR2 when libseat is in use. Or always
install the handlers but have them check `st->seat` and ignore the
signal if libseat is handling VT.

### 12.9. libseat fd in epoll

Ishizue's `isz_drm_set_server` (`isz_drm.c:1082-1097`) adds the
libseat fd to epoll with `EPOLLIN` only. The `ISZ_FD_SEAT` handler
in `isz_dispatch` (`isz_lifecycle.c:352-361`) calls
`isz_session_dispatch` and `isz_backend_read_events`.

wlroots [1, line 96] uses `wl_event_loop_add_fd` with
`WL_EVENT_READABLE`. Same pattern.

But there is a subtle issue: if the libseat socket's write buffer
fills up (e.g., the daemon is slow to read the ack), the compositor
needs to write more data and the fd becomes writable. Ishizue does
not listen for `EPOLLOUT`, so the compositor will not wake up to
retry the write. In practice this is rare because the libseat
protocol messages are small, but it can happen under heavy load.

wlroots uses `wl_event_loop_add_fd` which by default only listens
for `WL_EVENT_READABLE`. So wlroots has the same issue. Aquamarine
[13, line 520] uses `SPollFD` which I believe also only listens for
readable. So this is a common pattern, not an Ishizue-specific bug.

### 12.10. libinput suspend/resume on VT switch

As noted in section 11, Ishizue does not call
`libinput_suspend`/`libinput_resume` on session events. wlroots
[14, lines 185-189] and Aquamarine [13, lines 82-83, 90-91] do.

Without `libinput_suspend`, evdev fds stay open across VT switch.
With libseat, the session layer revokes the fds (via
`seat_deactivate_device`), so libinput's view of them becomes
stale. On resume, libinput does not know to re-open them. Input
stops working after a VT switch.

Fix: add a listener on `ISZ_EVENT_SESSION_*` that calls
`libinput_suspend` on inactive and `libinput_resume` on active.

### 12.11. Stale page-flip state on resume

Aquamarine's `restoreAfterVT` [15, lines 398-414] has a comment:

> During S3 suspend the display hardware powers off, so any pending
> page-flip completion events are lost. The handlePF() callback
> that normally clears these flags will never fire. Without this
> reset, commitState() rejects every frame with "Cannot commit when
> a page-flip is awaiting" and scheduleFrame() returns early,
> leaving outputs permanently black after resume.

Ishizue has the same issue. The `COMMITTING` state in the DRM
backend is set when a commit is queued and cleared when the
page-flip event arrives. If the page-flip event never arrives
(because the VT switch happened mid-commit, or because the hardware
powered off), the state stays `COMMITTING` forever and no further
commits are accepted.

This is documented in `doc/research/drm-backend-audit.md` as a
known bug. The fix is to clear the `COMMITTING` state on
`SESSION_ACTIVE` (in `isz_drm_on_session_active`) before
re-committing.

### 12.12. Connector rescan on resume

wlroots [2, line 105] calls `scan_drm_connectors(drm)` on resume.
Aquamarine [15, line 417] calls `recheckOutputs()` on resume. Both
re-enumerate connectors in case the display configuration changed
while the VT was away (e.g., user plugged in a monitor while on
another VT).

Ishizue's `isz_drm_on_session_active` (`isz_drm.c:1128-1145`) does
NOT call `isz_drm_rescan_connectors`. It only re-acquires master.
So if a monitor is plugged in while on another VT, Ishizue will
not notice until the next hotplug event (which may never come if
the kernel already sent it while we were away).

Fix: call `isz_drm_rescan_connectors(st)` in
`isz_drm_on_session_active` after re-acquiring master.

### 12.13. Re-commit of connector state on resume

wlroots [2, lines 107-122] re-commits each connector's state on
resume:

```c
struct wlr_drm_connector *conn;
wl_list_for_each(conn, &drm->outputs, link) {
        struct wlr_output_mode *mode = NULL;
        uint32_t committed = WLR_OUTPUT_STATE_ENABLED;
        if (conn->output.enabled && conn->output.current_mode != NULL) {
                committed |= WLR_OUTPUT_STATE_MODE;
                mode = conn->output.current_mode;
        }
        struct wlr_output_state state = {
                .committed = committed,
                .enabled = mode != NULL,
                .mode_type = WLR_OUTPUT_STATE_MODE_FIXED,
                .mode = mode,
        };
        drm_connector_commit_state(conn, &state);
}
```

Aquamarine's `restoreAfterVT` [15, lines 423-470] does the same:
walks each connector, builds a commit state, calls
`commitState`.

Ishizue's `isz_drm_on_session_active` does NOT re-commit any
state. It only re-acquires master. The CRTC was disabled by the
new VT owner (or by the kernel on switch), so the screen stays
black after switch back until something else triggers a commit.

Fix: walk each output and re-commit its last state in
`isz_drm_on_session_active`.

### 12.14. libseat dispatch on init

Ishizue's `isz_drm_init` (`isz_drm.c:756-764`) opens the libseat
seat but does NOT dispatch it:

```c
st->seat = libseat_open_seat(&drm_seat_listener, st);
if (st->seat) {
    /* Don't dispatch here. enable_seat would fire before drm_fd
     * is set, and the callback's guard would return early but
     * leave the seat un-acknowledged. */
}
```

The comment is wrong. `enable_seat` does not need to be acked (only
`disable_seat` does). wlroots [1, lines 103-108] dispatches once
after `libseat_open_seat` to drain any pending `enable_seat`:

```c
// We may have received enable_seat immediately after the open_seat result,
// so, dispatch once without timeout to speed up activation.
if (libseat_dispatch(session->seat_handle, 0) == -1) {
        wlr_log_errno(WLR_ERROR, "libseat dispatch failed");
        goto error_dispatch;
}
```

Without this dispatch, `enable_seat` may sit in the socket buffer
until the next event loop tick. The compositor starts with
`session->active = false` (the default) and may render a frame
before `enable_seat` fires, which would fail because master is not
yet acquired (in the libseat path, master is acquired by the daemon
before `enable_seat` is sent, so this is less of an issue, but
`session->active` is still false).

Aquamarine [13, line 321] also dispatches:

```cpp
// dispatch any already pending events
session->dispatchPendingEventsAsync();
```

Fix: call `libseat_dispatch(st->seat, 0)` once after
`libseat_open_seat` returns. The `enable_seat` callback's guard
should be fixed to not assume `drm_fd` is set; instead, set
`session_active = true` and let the first `isz_drm_commit` call
handle the case where master is not yet acquired.

### 12.15. Summary of differences

| Topic | wlroots | Aquamarine | Weston (direct) | Xorg | Ishizue |
|---|---|---|---|---|---|
| Calls drmSetMaster at init | No | No | Yes (implicit via open) | Yes (via DDX) | Yes (explicit) |
| Calls drmDropMaster on VT switch | No | No | Yes | Yes (via DDX) | Yes (redundant with libseat) |
| VT signal handler | signalfd (via wl_event_loop) | signalfd (via SPollFD) | signalfd (via wl_event_loop) | signal() + flag | sigaction + flag |
| VT signal choice | N/A (libseat) | N/A (libseat) | SIGRTMIN for both | SIGUSR1 for both | SIGUSR1/SIGUSR2 (matches seatd) |
| KDSETMODE KD_GRAPHICS | N/A | N/A | Yes | Yes | No |
| KDSKBMODE K_OFF | N/A | N/A | Yes | Yes | No |
| VT_ACTIVATE on init | N/A | N/A | Yes | Yes | No |
| Re-commits state on resume | Yes | Yes | Yes (via session_signal) | Yes (via DDX) | No |
| Rescans connectors on resume | Yes | Yes | Yes (via session_signal) | Yes | No |
| libinput_suspend on disable | Yes | Yes | N/A (uses libinput directly) | N/A | No |
| Clears stale page-flip on resume | Yes (implicit) | Yes (explicit) | Yes (implicit) | Yes (via DDX) | No |

The "No" entries in the Ishizue column are the bugs. Items 12.1,
12.3, 12.10, 12.11, 12.12, and 12.13 are the highest priority
because they cause visible symptoms (crash, black screen, no
input, no VT switch). Items 12.5, 12.6, 12.7, and 12.14 are
correctness issues that happen to work today but are fragile.
Items 12.2, 12.4, 12.8, and 12.9 are latent issues that will bite
under specific conditions (display manager launch, heavy load,
wrapper process sending signals).

## References

[1] wlroots `backend/session/session.c`, swaywm mirror on GitHub,
    fetched 2026-07-22.
    https://raw.githubusercontent.com/swaywm/wlroots/master/backend/session/session.c

[2] wlroots `backend/drm/backend.c`, swaywm mirror on GitHub,
    fetched 2026-07-22.
    https://raw.githubusercontent.com/swaywm/wlroots/master/backend/drm/backend.c

[3] wlroots `backend/drm/drm.c` (legacy single-file version),
    swaywm mirror on GitHub, fetched 2026-07-22.
    https://raw.githubusercontent.com/swaywm/wlroots/master/backend/drm/drm.c

[4] Weston `libweston/launcher-direct.c`, intel/external-weston
    mirror on GitHub, fetched 2026-07-22.
    https://raw.githubusercontent.com/intel/external-weston/master/libweston/launcher-direct.c

[5] Weston `libweston/launcher-logind.c`, intel/external-weston
    mirror on GitHub, fetched 2026-07-22.
    https://raw.githubusercontent.com/intel/external-weston/master/libweston/launcher-logind.c

[6] Sway `sway/server.c`, swaywm/sway on GitHub, fetched
    2026-07-22.
    https://raw.githubusercontent.com/swaywm/sway/master/sway/server.c

[7] Linux kernel, `Documentation/gpu/drm-uapi.rst`, "Primary Nodes,
    DRM Master and Authentication" section.
    https://www.kernel.org/doc/html/latest/gpu/drm-uapi.html

[8] seatd `seatd/seat.c`, kennylevinsen/seatd on GitHub, fetched
    2026-07-22.
    https://raw.githubusercontent.com/kennylevinsen/seatd/master/seatd/seat.c

[9] libseat public API header, kennylevinsen/seatd on GitHub,
    fetched 2026-07-22.
    https://raw.githubusercontent.com/kennylevinsen/seatd/master/include/libseat.h

[10] seatd `common/terminal.c`, kennylevinsen/seatd on GitHub,
     fetched 2026-07-22.
     https://raw.githubusercontent.com/kennylevinsen/seatd/master/common/terminal.c

[11] seatd `seatd/server.c`, kennylevinsen/seatd on GitHub,
     fetched 2026-07-22.
     https://raw.githubusercontent.com/kennylevinsen/seatd/master/seatd/server.c

[12] Aquamarine README, hyprwm/aquamarine on GitHub.
     https://github.com/hyprwm/aquamarine

[13] Aquamarine `src/backend/Session.cpp`, hyprwm/aquamarine on
     GitHub, fetched 2026-07-22.
     https://raw.githubusercontent.com/hyprwm/aquamarine/main/src/backend/Session.cpp

[14] wlroots `backend/libinput/backend.c`, swaywm mirror on GitHub,
     fetched 2026-07-22.
     https://raw.githubusercontent.com/swaywm/wlroots/master/backend/libinput/backend.c

[15] Aquamarine `src/backend/drm/DRM.cpp`, hyprwm/aquamarine on
     GitHub, fetched 2026-07-22.
     https://raw.githubusercontent.com/hyprwm/aquamarine/main/src/backend/drm/DRM.cpp

[16] libdrm `xf86drm.c`, robclark/libdrm mirror on GitHub, fetched
     2026-07-22. `drmSetMaster` at line 2634, `drmDropMaster` at
     line 2639.
     https://raw.githubusercontent.com/robclark/libdrm/master/xf86drm.c

[17] `ioctl_vt(2)` Linux man page, man-pages 6.18, 2026-02-08.
     https://man7.org/linux/man-pages/man2/ioctl_vt.2.html

[18] Linux kernel `drivers/tty/vt/vt_ioctl.c`, `VT_RELDISP` case
     at line 869, fetched 2026-07-22.
     https://raw.githubusercontent.com/torvalds/linux/master/drivers/tty/vt/vt_ioctl.c

[19] libseat `libseat/libseat.c`, kennylevinsen/seatd on GitHub,
     fetched 2026-07-22.
     https://raw.githubusercontent.com/kennylevinsen/seatd/master/libseat/libseat.c

[20] libseat seatd backend `libseat/backend/seatd.c`,
     kennylevinsen/seatd on GitHub, fetched 2026-07-22.
     https://raw.githubusercontent.com/kennylevinsen/seatd/master/libseat/backend/seatd.c

[21] Xorg `hw/xfree86/os-support/shared/VTsw_usl.c`, sulmone/X11
     mirror on GitHub (xorg-server 1.12.2), fetched 2026-07-22.
     https://raw.githubusercontent.com/sulmone/X11/master/xorg-server-1.12.2/hw/xfree86/os-support/shared/VTsw_usl.c

[22] Xorg `hw/xfree86/os-support/linux/lnx_init.c`, sulmone/X11
     mirror on GitHub (xorg-server 1.12.2), fetched 2026-07-22.
     https://raw.githubusercontent.com/sulmone/X11/master/xorg-server-1.12.2/hw/xfree86/os-support/linux/lnx_init.c
