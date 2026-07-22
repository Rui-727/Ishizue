# DRM master and VT switching in Wayland compositors (W13-RESEARCH)

The Ishizue DRM backend has a critical bug: when `tinyisz` runs with the
DRM backend, the user cannot switch TTYs. `tinyisz` holds DRM master and
the VT switch hangs. The libseat session fd is in the epoll set but the
disable flow does not complete. The user got stuck on a TTY unable to
switch away.

This document reverse-engineers how wlroots, Aquamarine (Hyprland), and
weston actually handle DRM master acquisition, release, and VT switch
acknowledgment, then maps the differences onto our code. Sources are
cited inline as `[N]` and listed at the end.

The short version: wlroots and Aquamarine do NOT call `drmSetMaster` or
`drmDropMaster` directly. The libseat backend (seatd daemon, builtin
in-process server, or logind) owns the master and the VT. The
compositor's `disable_seat` callback only acknowledges the libseat
disable event; it does NOT drive the VT switch. Our code calls
`drmSetMaster` and `drmDropMaster` directly, which races with libseat
and can leave the VT in a state where the switch hangs.

## 1. How wlroots opens the DRM device and acquires master

wlroots splits device opening and master management across two files:
`backend/session/session.c` (libseat integration) and
`backend/drm/backend.c` (DRM backend). The compositor never calls
`drmSetMaster` itself [1][2].

`wlr_session_create` in `backend/session/session.c` calls
`libseat_session_init`, which calls `libseat_open_seat(&seat_listener,
session)` [2]. The seat listener is two functions:

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

`backend/session/session.c` lines 18-27 [2].

After `libseat_open_seat` returns, the compositor calls
`wlr_session_find_gpus` which calls `wlr_session_open_file(session,
path)` for each candidate. `wlr_session_open_file` calls
`libseat_open_device(session->seat_handle, path, &fd)` and returns a
`struct wlr_device` wrapping the fd [2]. The fd is the DRM device fd
opened by the seatd daemon (or granted by logind). The compositor does
NOT call `drmSetMaster` on this fd.

`wlr_drm_backend_create` in `backend/drm/backend.c` receives the
`struct wlr_device *dev` and stores `drm->fd = dev->fd` [1]. It then
calls `check_drm_features(drm)` and `init_drm_resources(drm)`, which
query DRM caps and enumerate KMS resources. Neither function calls
`drmSetMaster`. The fd already has master privileges because the seatd
daemon opened it with master (or logind granted it via `TakeControl`).

`backend/drm/backend.c` also registers a listener on
`session->events.active` whose handler is `handle_session_active` [1]:

```c
static void handle_session_active(struct wl_listener *listener, void *data) {
    struct wlr_drm_backend *drm =
        wl_container_of(listener, drm, session_active);
    struct wlr_session *session = drm->session;

    if (session->active) {
        wlr_log(WLR_INFO, "DRM fd resumed");
        scan_drm_connectors(drm);
        /* re-commit state on each connector */
    } else {
        wlr_log(WLR_INFO, "DRM fd paused");
    }
}
```

`backend/drm/backend.c` lines 95-120 [1].

The "DRM fd paused" branch does NOTHING. It logs and returns. There is
no `drmDropMaster` call. There is no `VT_RELDISP` call. The session
layer (libseat) already handled both.

This is the key pattern: the compositor treats the DRM fd as a
capability granted by the session layer. The session layer acquires and
releases master. The compositor only re-commits KMS state on resume.

## 2. How wlroots handles VT switching

When the user presses Ctrl+Alt+F3, the kernel checks the current VT's
mode. If the VT is in `VT_PROCESS` mode (set by some process via
`VT_SETMODE`), the kernel sends the `relsig` signal to the process that
called `VT_SETMODE`, stores the target VT number in `vc->vt_newvt`, and
WAITS for that process to call `VT_RELDISP(1)` before actually
switching [3][4]. If the VT is in `VT_AUTO` mode, the kernel switches
immediately without asking.

Under wlroots, the process that called `VT_SETMODE` is the seatd daemon
(out-of-process backend), the in-process seatd server (builtin backend),
or logind (logind backend). NOT the compositor. The compositor never
calls `VT_SETMODE`.

The seatd daemon's signal handler runs in its own process. The handler
calls `seat_vt_release(seat)` in `seatd/seat.c` [5]:

```c
int seat_vt_release(struct seat *seat) {
    if (!seat->vt_bound) {
        log_debug("VT release request on non VT-bound seat, ignoring");
        return -1;
    }
    seat_update_vt(seat);

    log_debug("Releasing VT");
    if (seat->active_client != NULL) {
        seat_disable_client(seat->active_client);
    }

    vt_ack(seat, true);
    seat->cur_vt = -1;
    return 0;
}
```

`seatd/seat.c` lines 740-755 [5].

`seat_disable_client` deactivates all client devices and sends
`SERVER_DISABLE_SEAT` to the compositor. `vt_ack(seat, true)` calls
`terminal_ack_release` which calls `ioctl(fd, VT_RELDISP, 1)` [6]. The
order matters: the daemon sends `SERVER_DISABLE_SEAT` to the compositor
FIRST, then acks the kernel. The kernel then switches the VT.

The compositor receives `SERVER_DISABLE_SEAT` via its libseat fd. The
libseat seatd backend's `dispatch` function calls `execute_events`
which calls the compositor's `disable_seat` callback [7]. The callback
sets `session->active = false`, emits `session->events.active` (which
fires `handle_session_active` in the DRM backend, which logs "DRM fd
paused"), and calls `libseat_disable_seat(session->seat_handle)`.

`libseat_disable_seat` sends `CLIENT_DISABLE_SEAT` to the daemon and
blocks waiting for `SERVER_SEAT_DISABLED` [7]. The daemon processes
`CLIENT_DISABLE_SEAT` by calling `seat_ack_disable_client`, which
sets the client state to `CLIENT_DISABLED` and sends `SERVER_SEAT_DISABLED`
back [5]. `libseat_disable_seat` returns.

The VT switch has ALREADY happened at this point (the daemon acked the
kernel before the compositor even received the disable event). The
compositor's job in the `disable_seat` callback is just to acknowledge
the libseat protocol so the daemon can mark the client as disabled and
proceed with cleanup. The compositor does NOT drive the VT switch.

## 3. The libseat seat_listener pattern

The libseat API is documented in `include/libseat.h` [8]. The
`disable_seat` callback documentation is explicit:

> The seat has been disabled. This event signals that the application
> is going to lose its seat access. The event *must* be acknowledged
> with libseat_disable_seat shortly after receiving this event.
>
> If the recepient fails to acknowledge the event in time, seat devices
> may be forcibly revoked by the seat provider.

`include/libseat.h` lines 22-31 [8].

The order is fixed by the protocol:

1. libseat backend (seatd, logind, or builtin) detects the VT switch.
2. libseat backend calls the compositor's `disable_seat` callback.
3. The compositor does its cleanup (pause rendering, drop any resources
   that conflict with the new VT owner).
4. The compositor calls `libseat_disable_seat(seat)` to acknowledge.
5. libseat backend completes its side of the protocol (seatd marks the
   client as `CLIENT_DISABLED`; logind sends `PauseDeviceComplete` if
   it hasn't already).

The wlroots pattern matches this exactly [2]. So does the Aquamarine
pattern [9]:

```cpp
static void libseatDisableSeat(struct libseat* seat, void* data) {
    auto PSESSION    = (Aquamarine::CSession*)data;
    PSESSION->active = false;
    if (PSESSION->libinputHandle)
        libinput_suspend(PSESSION->libinputHandle);
    PSESSION->events.changeActive.emit();
    libseat_disable_seat(PSESSION->libseatHandle);
}
```

`src/backend/Session.cpp` lines 87-94 [9].

Neither wlroots nor Aquamarine calls `drmDropMaster` from the
`disable_seat` callback. Neither calls `drmSetMaster` from the
`enable_seat` callback. Master management is the session layer's job.

The ORDER inside the callback matters: emit the "active changed" signal
BEFORE calling `libseat_disable_seat`. Downstream listeners (the DRM
backend's `handle_session_active`) need to run before the
acknowledgment so they can pause rendering and release KMS state.
Calling `libseat_disable_seat` last matches the documentation's
"shortly after receiving this event" wording [8].

Calling `libseat_disable_seat` from inside the `disable_seat` callback
looks re-entrant, and it is. The libseat seatd backend's
`libseat_disable_seat` implementation sends `CLIENT_DISABLE_SEAT` and
calls `read_until_response` which blocks on `poll_connection(backend,
-1)` [7]. This blocks the calling thread until the daemon responds. The
daemon is responsive (it already acked the kernel and is back in its
poller loop), so the block is short. Both wlroots and Aquamarine rely
on this. The builtin backend's daemon is a forked child running its own
poller, so it also responds quickly.

## 4. How the libseat fd is polled

`libseat_get_fd` returns a pollable fd. For the seatd backend it is the
socket to the daemon [7]. For the logind backend it is the sd-bus fd
[10]. For the builtin backend it is one end of a socketpair to the
in-process server [7].

wlroots adds this fd to the wayland event loop in
`libseat_session_init` [2]:

```c
session->libseat_event = wl_event_loop_add_fd(event_loop,
    libseat_get_fd(session->seat_handle),
    WL_EVENT_READABLE, libseat_event, session);
```

`backend/session/session.c` lines 81-83 [2].

The callback is `libseat_event` [2]:

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

`backend/session/session.c` lines 29-35 [2].

Three things to note. First, the callback calls
`libseat_dispatch(session->seat_handle, 0)` with timeout 0
(non-blocking). Second, on error it terminates the display. Third,
there is exactly ONE libseat seat per process (`session->seat_handle`),
and that one seat is used for both session events AND device opening
(`libseat_open_device`).

Aquamarine follows the same pattern. `CSession::pollFDs` returns the
libseat fd plus the udev and libinput fds, and the compositor's event
loop dispatches `libseat_dispatch(libseatHandle, 0)` when the libseat
fd is readable [9]:

```cpp
std::vector<SPollFD> Aquamarine::CSession::pollFDs() {
    return {
        makeShared<SPollFD>(libseat_get_fd(libseatHandle),
            [this](){ dispatchLibseatEvents(); }),
        /* udev, libinput */
    };
}
```

`src/backend/Session.cpp` lines 519-525 [9].

The libseat fd is the ONLY way the compositor learns about VT switches.
libseat has no internal event source that runs on its own. The
compositor must poll the fd and call `libseat_dispatch` when it is
readable. If the compositor never dispatches, `enable_seat` and
`disable_seat` callbacks never fire, and the compositor's state goes
stale. The VT switch itself still proceeds (the daemon acked the
kernel independently), but the compositor doesn't know it happened.

wlroots also calls `libseat_dispatch(session->seat_handle, 0)` ONCE at
the end of `libseat_session_init` [2]:

```c
// We may have received enable_seat immediately after the open_seat
// result, so, dispatch once without timeout to speed up activation.
if (libseat_dispatch(session->seat_handle, 0) == -1) {
    wlr_log_errno(WLR_ERROR, "libseat dispatch failed");
    goto error_dispatch;
}
```

`backend/session/session.c` lines 91-95 [2].

This is important. `libseat_open_seat` internally calls
`execute_events` at the end of `_open_seat` [7], which fires
`enable_seat` if the daemon already activated the client. But the
compositor's `enable_seat` callback needs `session` to be fully
initialized (the `seat_handle`, the event source, etc.) before it can
safely run. Dispatching once at the end of init catches any events that
queued during `libseat_open_seat` and fires the callback now that
everything is set up.

## 5. The VT switch acknowledgment

What actually acknowledges the VT switch to the kernel is `VT_RELDISP`
with argument 1, issued on the VT's tty fd by the process that called
`VT_SETMODE`. The kernel's `vt_reldisp` function in
`drivers/tty/vt/vt_ioctl.c` handles it [4]:

```c
static int vt_reldisp(struct vc_data *vc, unsigned int swtch)
{
    int newvt, ret;

    if (vc->vt_mode.mode != VT_PROCESS)
        return -EINVAL;

    /* Switched-to response */
    if (vc->vt_newvt < 0) {
         /* If it's just an ACK, ignore it */
        return swtch == VT_ACKACQ ? 0 : -EINVAL;
    }

    /* Switching-from response */
    if (swtch == 0) {
        /* Switch disallowed, so forget we were trying to do it. */
        vc->vt_newvt = -1;
        return 0;
    }

    /* The current vt has been released, so complete the switch. */
    newvt = vc->vt_newvt;
    vc->vt_newvt = -1;
    ret = vc_allocate(newvt);
    if (ret)
        return ret;

    complete_change_console(vc_cons[newvt].d);

    return 0;
}
```

`drivers/tty/vt/vt_ioctl.c` lines 553-585 [4].

`swtch == 1` means "release acknowledged, proceed with the switch".
`swtch == 0` means "release denied, cancel the switch". `swtch ==
VT_ACKACQ` (2) means "acquire acknowledged" (used after the new VT's
process receives `acqsig`).

The kernel's `change_console` function (the front-end of a VT switch)
shows the wait [4]:

```c
void change_console(struct vc_data *new_vc)
{
    /* ... */
    vc = vc_cons[fg_console].d;
    if (vc->vt_mode.mode == VT_PROCESS) {
        vc->vt_newvt = new_vc->vc_num;
        if (kill_pid(vc->vt_pid, vc->vt_mode.relsig, 1) == 0) {
            /*
             * It worked. Mark the vt to switch to and
             * return. The process needs to send us a
             * VT_RELDISP ioctl to complete the switch.
             */
            return;
        }
        /* The controlling process has died, so we revert back to
         * normal operation. */
        reset_vc(vc);
    }
    /* ... fall through to complete_change_console for VT_AUTO ... */
}
```

`drivers/tty/vt/vt_ioctl.c` lines 1196-1245 [4].

Two things to note. First, if the VT is in `VT_PROCESS` mode and the
controlling process (`vc->vt_pid`) is alive, the kernel sends `relsig`
and RETURNS without switching. The switch only completes when the
process calls `VT_RELDISP(1)`. If the process never calls it, the
switch hangs forever.

Second, if `kill_pid` fails (the process is dead), the kernel calls
`reset_vc(vc)` which resets the VT to `VT_AUTO` mode and proceeds with
the switch [4]. This is the kernel's own recovery for dead VT
controllers. It does NOT always work reliably; the comment in the
kernel source says "the worst thing that can happen is: we send a
signal to a process, it dies, and the switch gets 'lost' waiting for a
response" [4].

Under seatd, the daemon calls `VT_RELDISP(1)` via
`terminal_ack_release` in `common/terminal.c` [6]:

```c
int terminal_ack_release(int fd) {
    log_debug("Acking VT release");
    if (ioctl(fd, VT_RELDISP, 1) == -1) {
        log_errorf("Could not ack VT release: %s", strerror(errno));
        return -1;
    }
    return 0;
}
```

`common/terminal.c` lines 223-231 [6].

Under logind, the compositor does NOT call `VT_RELDISP` at all. logind
handles the VT switch itself when it receives `PauseDeviceComplete`
from the compositor (or when it forces the pause). The libseat logind
backend sends `PauseDeviceComplete` automatically inside
`handle_pause_device` [10]:

```c
if (pause) {
    set_active(session, false);  // fires disable_seat callback
    ret = sd_bus_call_method(session->bus, "org.freedesktop.login1",
        session->path, "org.freedesktop.login1.Session",
        "PauseDeviceComplete", ret_error, NULL, "uu", major, minor);
}
```

`libseat/backend/logind.c` lines 386-401 [10].

Under logind, `libseat_disable_seat` is a NO-OP [10]:

```c
static int disable_seat(struct libseat *base) {
    (void)base;
    return 0;
}
```

`libseat/backend/logind.c` lines 215-218 [10].

This is because the acknowledgment already happened via
`PauseDeviceComplete`. The compositor's `disable_seat` callback still
runs (for cleanup), and it still calls `libseat_disable_seat` (because
the pattern is uniform across backends), but the call does nothing
under logind.

So the answer to "what acknowledges the VT switch" depends on the
backend:

- seatd (out-of-process or builtin): the daemon calls `VT_RELDISP(1)`
  on the tty fd. The compositor's `libseat_disable_seat` call is a
  protocol acknowledgment, not a kernel acknowledgment.
- logind: logind calls `VT_RELDISP(1)` (or its equivalent) when it
  receives `PauseDeviceComplete`. The compositor's
  `libseat_disable_seat` is a no-op.

The compositor NEVER calls `VT_RELDISP` directly when using libseat.

## 6. How weston does it differently

weston has its own session management with three backends:
`launcher-direct.c` (no session manager, weston handles VT itself),
`launcher-logind.c` (direct DBus to logind, no libseat), and
`launcher-libseat.c` (libseat). The direct launcher is the instructive
one because it shows the full VT handling without a session manager
abstraction [11].

`setup_tty` in `src/launcher-direct.c` [11]:

```c
mode.mode = VT_PROCESS;
mode.relsig = SIGRTMIN;
mode.acqsig = SIGRTMIN;
if (ioctl(launcher->tty, VT_SETMODE, &mode) < 0) {
    weston_log("failed to take control of vt handling\n");
    goto err_close;
}

loop = wl_display_get_event_loop(launcher->compositor->wl_display);
launcher->vt_source = wl_event_loop_add_signal(loop, SIGRTMIN,
                                               vt_handler, launcher);
```

`src/launcher-direct.c` lines 191-200 [11].

weston uses `SIGRTMIN` for BOTH `relsig` and `acqsig`. The signal
handler distinguishes release from acquire by checking
`compositor->session_active` [11]:

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

`src/launcher-direct.c` lines 76-92 [11].

This is the direct pattern. The order on release is:

1. Set `session_active = 0`.
2. Emit `session_signal` (downstream listeners pause rendering).
3. Call `drmDropMaster(launcher->drm_fd)`.
4. Call `ioctl(launcher->tty, VT_RELDISP, 1)`.

Step 4 is what unblocks the kernel. Without it, the switch hangs. Step
3 must happen BEFORE step 4 because once the kernel switches, the new
VT's process needs to be able to acquire master.

On acquire (switch back):

1. Call `ioctl(launcher->tty, VT_RELDISP, VT_ACKACQ)`.
2. Call `drmSetMaster(launcher->drm_fd)`.
3. Set `session_active = 1`.
4. Emit `session_signal`.

weston's direct launcher ALSO restores `VT_AUTO` mode on teardown [11]:

```c
/*
 * VT_AUTO, so we don't risk switching to a VT with another
 * ... process controlling it.
 */
drmDropMaster(launcher->drm_fd);

mode.mode = VT_AUTO;
if (ioctl(launcher->tty, VT_SETMODE, &mode) < 0)
    /* ... */
```

`src/launcher-direct.c` lines 259-268 [11].

This is important. If weston crashes without restoring `VT_AUTO`, the
next process that tries to switch VTs hangs because the kernel is still
in `VT_PROCESS` mode waiting for the dead weston to ack. Restoring
`VT_AUTO` on teardown prevents this.

weston's logind launcher does NOT call `VT_SETMODE` or `VT_RELDISP`
directly. It talks DBus to logind: `TakeControl` to acquire the
session, `ReleaseControl` to release it, `TakeDevice`/`ReleaseDevice`
for DRM and evdev, and `PauseDeviceComplete` to acknowledge device
pauses [12]. logind handles the VT switch internally.

The weston logind launcher's `device_paused` handler [12]:

```c
static void
device_paused(struct launcher_logind *wl, DBusMessage *m)
{
    /* parse major, minor, type */
    if (!strcmp(type, "pause"))
        launcher_logind_pause_device_complete(wl, major, minor);

    if (wl->sync_drm && major == DRM_MAJOR)
        launcher_logind_set_active(wl, false);
}
```

`src/launcher-logind.c` lines 449-460 [12].

`launcher_logind_pause_device_complete` sends the `PauseDeviceComplete`
DBus method to logind [12]. This is the acknowledgment. logind then
proceeds with the VT switch.

## 7. How Hyprland does it

Hyprland dropped wlroots in favor of its own Aquamarine backend
library. Aquamarine's session handling is in `src/backend/Session.cpp`
[9]. It uses libseat directly, following the same pattern as wlroots.

`CSession`'s libseat integration is a near-exact copy of wlroots's
`wlr_session` [9]:

- One libseat seat per process (`session->libseatHandle`).
- `libseatEnableSeat` callback sets `active = true`, resumes libinput,
  emits `changeActive`.
- `libseatDisableSeat` callback sets `active = false`, suspends
  libinput, emits `changeActive`, calls `libseat_disable_seat`.
- The libseat fd is in `CSession::pollFDs` and dispatches via
  `libseat_dispatch(libseatHandle, 0)`.
- `CSession::switchVT(vt)` calls `libseat_switch_session(libseatHandle,
  vt)`.

`src/backend/Session.cpp` lines 79-99, 501-504 [9].

Hyprland does NOT add any custom VT handling on top of Aquamarine. It
relies entirely on libseat for VT switching. There is no
`drmSetMaster`/`drmDropMaster` call in the Hyprland compositor or in
Aquamarine's session layer.

The takeaway: every modern Wayland compositor that uses libseat
(wlroots, Aquamarine, sway, Hyprland, cage, labwc, river, wayfire,
niri) follows the SAME pattern. They do NOT call `drmSetMaster` or
`drmDropMaster` directly. They do NOT call `VT_SETMODE` or
`VT_RELDISP` directly. The libseat backend owns all of that.

## 8. The specific bug in our code

Our Ishizue DRM backend in `src/backend/isz_drm.c` deviates from the
wlroots/Aquamarine pattern in several ways. Listing each deviation
with file:line references.

### Bug 1: We call drmSetMaster directly

`src/backend/isz_drm.c` line 644:

```c
if (drmSetMaster(fd) != 0) {
    /* fail fast with ISZ_ERR_DRM_MASTER */
}
```

Under seatd, the daemon already holds master on the device fd it gave
us via `libseat_open_device`. Our `drmSetMaster` call is either
redundant (succeeds silently because the fd already has master) or
conflicts with the daemon. Under logind, logind granted us master via
`TakeControl`, so `drmSetMaster` is redundant. Under builtin, the
in-process server holds master.

wlroots and Aquamarine do NOT call `drmSetMaster`. They trust the
session layer to grant master. Calling `drmSetMaster` ourselves is
harmless when it succeeds (the fd already has master) but it papers
over the case where libseat failed to grant master. If
`libseat_open_device` returned a fd without master (e.g., because
libseat fell back to a non-seat-aware path), our `drmSetMaster` would
succeed if we are root and fail otherwise. Either way, we are bypassing
the session layer's master management.

### Bug 2: We call drmDropMaster directly in the disable_seat callback

`src/backend/isz_drm.c` lines 566-579:

```c
static void drm_disable_seat(struct libseat *seat, void *userdata) {
    (void)seat;
    struct isz_drm_state *st = userdata;
    if (!st || !st->seat || st->drm_fd < 0) return;
    st->session_active = false;
    isz_log_internal(ISZ_LOG_INFO, "drm: VT switch away, dropping master");
    if (drmDropMaster(st->drm_fd) != 0) {
        isz_log_internal(ISZ_LOG_WARN, "drm: drmDropMaster failed: %s",
                         strerror(errno));
    }
    libseat_disable_seat(st->seat);
}
```

Under seatd, the daemon drops master on its side when it receives
`CLIENT_DISABLE_SEAT`. Our `drmDropMaster` call either fails (we are
not master; the daemon is) or races with the daemon. Under logind,
logind drops master when it receives `PauseDeviceComplete`. Our
`drmDropMaster` is redundant.

The wlroots pattern is: emit the active signal (downstream listeners
pause), then call `libseat_disable_seat`. No `drmDropMaster`. Aquamarine
is the same. Our code adds a `drmDropMaster` call that does not belong
in the libseat path.

### Bug 3: The disable_seat callback can return without acknowledging

`src/backend/isz_drm.c` line 569:

```c
if (!st || !st->seat || st->drm_fd < 0) return;
```

If `st->drm_fd < 0` when the callback fires, we return WITHOUT calling
`libseat_disable_seat(st->seat)`. The libseat protocol requires the
callback to acknowledge with `libseat_disable_seat` "shortly after
receiving this event" [8]. If we don't acknowledge, the daemon
eventually force-revokes our devices, but during the grace period the
VT switch may be incomplete or the next `enable_seat` may not fire
correctly.

`st->drm_fd` is set at line 640 in `isz_drm_init`, AFTER
`libseat_open_seat` at line 621. If `enable_seat` fires during
`libseat_open_seat` (which it does, per the seatd backend's
`_open_seat` calling `execute_events` at the end [7]), our
`drm_enable_seat` callback guards on `drm_fd < 0` and returns early.
`enable_seat` does not need acknowledgment, so this is fine.

But if `disable_seat` fires before `drm_fd` is set (race during init,
or a VT switch during startup), we return early without acknowledging.
The daemon waits for our ack. The next VT switch may hang.

The comment at lines 623-627 acknowledges this risk: "Don't dispatch
here. enable_seat would fire before drm_fd is set, and the callback's
guard would return early but leave the seat un-acknowledged." The
comment conflates `enable_seat` (which doesn't need ack) with
`disable_seat` (which does). The real risk is `disable_seat` firing
during init.

The fix is to dispatch once at the end of init (like wlroots does at
`backend/session/session.c` line 91 [2]) AND to make the
`disable_seat` callback always acknowledge, even if `drm_fd` is
invalid.

### Bug 4: We do not dispatch libseat at the end of init

`src/backend/isz_drm.c` lines 621-628:

```c
st->seat = libseat_open_seat(&drm_seat_listener, st);
if (st->seat) {
    /* Don't dispatch here. enable_seat would fire before drm_fd
     * is set, and the callback's guard would return early but
     * leave the seat un-acknowledged. The first
     * isz_drm_read_events call drains the seat after init is
     * complete and drm_fd is valid. */
}
```

The comment says "don't dispatch here" but `libseat_open_seat`
internally calls `execute_events` [7], so `enable_seat` ALREADY fires
during `libseat_open_seat`. The comment's stated reason is wrong.

What we should do is dispatch AFTER `drm_fd` is set, like wlroots does
[2]. This catches any events that queued during `libseat_open_seat`
and fires the callbacks now that `drm_fd` is valid. The current code
relies on the first `isz_drm_read_events` call (triggered by a DRM
page-flip event or a libseat fd event) to drain the seat. If no event
arrives for a long time, the seat stays undrained.

### Bug 5: We have a second libseat seat that is never initialized

`src/input/isz_session.c` lines 55-78 declares `isz_session_init`
which calls `libseat_open_seat(&seat_listener, st)` on
`st->session` (the input_state's seat). But `isz_session_init` is
NEVER called from anywhere in the codebase (verified with grep). The
only caller is itself.

`isz_session_dispatch` in `src/input/isz_session.c` lines 80-89 drains
`st->session`:

```c
void isz_session_dispatch(isz_server *srv) {
    struct isz_input_state *st = isz_server_get_input_state(srv);
    if (!st || !st->session)
        return;
    while (libseat_dispatch(st->session, 0) > 0) {
        /* keep draining */
    }
}
```

Since `st->session` is always NULL (isz_session_init is never called),
this function is always a no-op.

The dispatch loop in `src/isz_lifecycle.c` line 357 calls
`isz_session_dispatch(srv)` when the ISZ_FD_SEAT epoll case fires:

```c
case ISZ_FD_SEAT:
    /* libseat session fd: drain the session so
     * enable_seat / disable_seat fire. ... */
    isz_session_dispatch(srv);
    if (srv->backend) {
        (void)isz_backend_read_events(srv->backend);
    }
    break;
```

`isz_session_dispatch` is a no-op. The actual drain happens in the
next call: `isz_backend_read_events` calls `isz_drm_read_events` which
calls `libseat_dispatch(st->seat, 0)` on the DRM backend's seat
(`drm_state->seat`). This works, but the comment in the dispatch case
is misleading: it says "drain the session" referring to the input
seat, but the input seat doesn't exist.

The fix is to remove the dead `isz_session_init` and
`isz_session_dispatch` code from `isz_session.c`, remove the
`isz_session_dispatch(srv)` call from `isz_lifecycle.c` line 357
(keep only `isz_backend_read_events`), and update the comment.

### Bug 6: We do not handle the direct-open fallback for VT switching

`src/backend/isz_drm.c` lines 614-628 and 631:

```c
#ifdef ISHIZUE_HAVE_LIBSEAT
    st->seat = libseat_open_seat(&drm_seat_listener, st);
    if (st->seat) {
        /* ... */
    }
#endif

    int fd = open_primary_drm_node(st);
```

If `libseat_open_seat` fails (returns NULL), we fall through to
`open_primary_drm_node` which opens the DRM device directly (via
`open(path, O_RDWR | O_CLOEXEC)`). We then call `drmSetMaster(fd)` at
line 644.

In this direct-open mode, we do NOT call `VT_SETMODE`. The kernel's VT
mode is whatever it was before we started. Two scenarios:

1. The kernel is in `VT_AUTO` mode (no previous session set
   `VT_PROCESS`). The kernel switches VTs immediately on Ctrl+Alt+F3.
   No hang. But we still hold DRM master, so the new VT's process
   cannot modeset. The user sees a text console on the new VT but
   cannot start another compositor. When they switch back, our state is
   stale (we never got a disable event).

2. The kernel is in `VT_PROCESS` mode (leftover from a previous seatd
   that crashed without restoring `VT_AUTO`, or from an X server that
   crashed). The kernel sends `relsig` to the dead process. The switch
   HANGS. The user is stuck.

Scenario 2 is the most likely cause of the reported "stuck on TTY
unable to switch away" bug. The fix is either:

- Fail fast if `libseat_open_seat` fails. Do not fall through to
  direct open. Require the user to run a seatd daemon or logind
  session.
- OR implement weston-style direct VT handling: open `/dev/tty<current>`,
  call `VT_SETMODE` with `VT_PROCESS` and `SIGRTMIN`, install a
  `SIGRTMIN` handler that does `drmDropMaster` + `VT_RELDISP(1)` on
  release and `VT_RELDISP(VT_ACKACQ)` + `drmSetMaster` on acquire.
  Restore `VT_AUTO` on teardown.

The first option is simpler and matches what wlroots does (wlroots
requires a session backend; it does not fall back to direct open).

### Bug 7: We do not restore VT_AUTO on teardown

`src/backend/isz_drm.c` lines 781-831 (`isz_drm_destroy`):

```c
if (st->is_master && st->drm_fd >= 0)
    (void)drmDropMaster(st->drm_fd);

if (st->drm_fd >= 0) {
    close(st->drm_fd);
    st->drm_fd = -1;
}

#ifdef ISHIZUE_HAVE_LIBSEAT
    if (st->seat) {
        libseat_close_seat(st->seat);
        st->seat = NULL;
    }
#endif
```

We drop master and close the fd. Under seatd, `libseat_close_seat`
tells the daemon to release the session, and the daemon restores
`VT_AUTO` on its side. Under direct open, we leave the VT in whatever
state it was in. If a previous process set `VT_PROCESS` and we
inherited it, we leave it set when we exit. The next compositor to
start on that VT inherits `VT_PROCESS` with a dead `vt_pid` (us), and
VT switches hang.

weston's direct launcher restores `VT_AUTO` on teardown [11]. We do
not.

### Bug 8: We do not check libseat_dispatch return value for errors

`src/backend/isz_drm.c` lines 770-776:

```c
if (st->seat) {
    while (libseat_dispatch(st->seat, 0) > 0) {
        /* keep draining */
    }
}
```

`libseat_dispatch` returns -1 on error [7][8]. We treat -1 the same as
0 (exit the loop). wlroots terminates the display on
`libseat_dispatch` error [2]. Aquamarine logs an error [9]. We
silently continue.

If the libseat connection breaks (daemon crashed, socket closed), the
next `libseat_dispatch` returns -1. We exit the loop and continue
processing DRM events. The seat is now in an error state; future
`libseat_dispatch` calls will also return -1. The compositor keeps
running but can no longer receive VT switch events. The next VT switch
hangs because the daemon (which set `VT_PROCESS`) is dead and no one
acks.

## 9. Concrete fix

Based on the working patterns, here is what we need to change. Listed
by file and function.

### Fix 1: Stop calling drmSetMaster directly

File: `src/backend/isz_drm.c`, function `isz_drm_init`, lines 643-657.

Remove the `drmSetMaster(fd)` call and the `st->is_master = true`
assignment. Trust the libseat backend to grant master. If
`drmSetMaster` is needed for the direct-open fallback (Fix 6), gate it
behind `if (!st->seat)` so it only runs in direct-open mode.

The `ISZ_ERR_DRM_MASTER` error path can stay as a fallback for the
direct-open case. In the libseat case, if the fd doesn't have master,
the first atomic commit will fail with `EACCES` or `EPERM` and we
surface the error there.

### Fix 2: Stop calling drmDropMaster in the disable_seat callback

File: `src/backend/isz_drm.c`, function `drm_disable_seat`, lines
566-579.

Remove the `drmDropMaster(st->drm_fd)` call. The libseat backend
handles master drop. Keep the `libseat_disable_seat(st->seat)` call.
The callback becomes:

```c
static void drm_disable_seat(struct libseat *seat, void *userdata) {
    (void)seat;
    struct isz_drm_state *st = userdata;
    if (!st || !st->seat) return;
    st->session_active = false;
    isz_log_internal(ISZ_LOG_INFO, "drm: VT switch away");
    /* emit ISZ_EVENT_SESSION_INACTIVE so downstream listeners pause */
    isz_event e = { .type = ISZ_EVENT_SESSION_INACTIVE };
    if (st->srv) isz_server_emit_event(st->srv, &e);
    libseat_disable_seat(st->seat);
}
```

The `drm_enable_seat` callback similarly drops its `drmSetMaster` call
and just emits `ISZ_EVENT_SESSION_ACTIVE`:

```c
static void drm_enable_seat(struct libseat *seat, void *userdata) {
    (void)seat;
    struct isz_drm_state *st = userdata;
    if (!st || !st->seat) return;
    st->session_active = true;
    isz_log_internal(ISZ_LOG_INFO, "drm: VT switch back");
    isz_event e = { .type = ISZ_EVENT_SESSION_ACTIVE };
    if (st->srv) isz_server_emit_event(st->srv, &e);
}
```

The `isz_drm_on_session_active` and `isz_drm_on_session_inactive`
functions in `src/backend/isz_drm.c` lines 960-990 currently call
`drmDropMaster` and `drmSetMaster`. They should be removed or
converted to no-ops, since the libseat callbacks now handle the state
transition directly. The listener shims at lines 1013-1025 can be
removed from `isz_init` registration at `src/isz_lifecycle.c` lines
253-258.

### Fix 3: Make disable_seat always acknowledge

File: `src/backend/isz_drm.c`, function `drm_disable_seat`.

Remove the `st->drm_fd < 0` guard. The callback must always call
`libseat_disable_seat(st->seat)` regardless of `drm_fd` state. The
guard was there because the old callback called `drmDropMaster`, which
needs a valid fd. With Fix 2, the callback no longer touches `drm_fd`,
so the guard is unnecessary.

### Fix 4: Dispatch libseat once at the end of init

File: `src/backend/isz_drm.c`, function `isz_drm_init`, after line 734
(after the `isz_log_internal` info line, before `return ISZ_OK`).

Add:

```c
#ifdef ISHIZUE_HAVE_LIBSEAT
    if (st->seat) {
        if (libseat_dispatch(st->seat, 0) == -1) {
            isz_log_internal(ISZ_LOG_ERROR,
                             "drm init: libseat_dispatch failed: %s",
                             strerror(errno));
            /* non-fatal; the seat will retry on the next epoll event */
        }
    }
#endif
```

This catches any events that queued during `libseat_open_seat` and
fires the callbacks now that `drm_fd` is valid. It matches the wlroots
pattern at `backend/session/session.c` lines 91-95 [2].

### Fix 5: Remove the dead input_state->session code

Files: `src/input/isz_session.c`, `src/input/isz_seat_internal.h`,
`src/isz_lifecycle.c`.

Delete `isz_session_init` and `isz_session_dispatch` from
`isz_session.c`. Delete their declarations from
`isz_seat_internal.h`. Remove the `isz_session_dispatch(srv)` call
from `isz_lifecycle.c` line 357 (inside the `ISZ_FD_SEAT` case) and
line 375 (the unconditional call at the end of `isz_dispatch`). The
DRM backend's `isz_drm_read_events` already drains the seat.

The `ISZ_FD_SEAT` case in `isz_lifecycle.c` becomes:

```c
case ISZ_FD_SEAT:
    if (srv->backend) {
        (void)isz_backend_read_events(srv->backend);
    }
    break;
```

### Fix 6: Handle libseat_open_seat failure correctly

File: `src/backend/isz_drm.c`, function `isz_drm_init`, lines 614-628.

Two options.

Option A (simpler, matches wlroots): fail fast if
`libseat_open_seat` fails. Remove the direct-open fallback. Return
`ISZ_ERR_FEATURE_UNAVAIL` with a clear log message explaining that
libseat is required. The user must run a seatd daemon or logind
session.

Option B (more compatible, matches weston's direct launcher):
implement direct VT handling when libseat is unavailable. Add a
`struct isz_drm_vt_state` with the tty fd, a `SIGRTMIN` handler, and
the `vt_handler` logic from weston's `launcher-direct.c` [11]. Open
`/dev/tty<current_vt>`, call `VT_SETMODE` with `VT_PROCESS` and
`SIGRTMIN`, install the handler. On release: `drmDropMaster` +
`VT_RELDISP(1)`. On acquire: `VT_RELDISP(VT_ACKACQ)` +
`drmSetMaster`. Restore `VT_AUTO` on teardown.

Option A is the minimum fix for the reported bug. Option B is the
robust fix that lets tinyisz run without a session manager. The task
scope is the VT switch bug, so Option A is sufficient. Option B can be
a follow-up.

### Fix 7: Restore VT_AUTO on teardown (Option B only)

File: `src/backend/isz_drm.c`, function `isz_drm_destroy`.

If we implemented Option B (direct VT handling), call `VT_SETMODE`
with `VT_AUTO` on the tty fd before closing it. This prevents the
"dead vt_pid" hang on the next compositor start. weston does this at
`src/launcher-direct.c` lines 259-268 [11].

If we implemented Option A (fail fast on libseat failure), this fix is
not needed because the libseat backend handles VT mode restoration.

### Fix 8: Check libseat_dispatch errors

File: `src/backend/isz_drm.c`, function `isz_drm_read_events`, lines
770-776.

Change the drain loop to check for errors:

```c
#ifdef ISHIZUE_HAVE_LIBSEAT
    if (st->seat) {
        int rc;
        while ((rc = libseat_dispatch(st->seat, 0)) > 0) {
            /* keep draining */
        }
        if (rc == -1) {
            isz_log_internal(ISZ_LOG_ERROR,
                             "drm: libseat_dispatch failed: %s",
                             strerror(errno));
            /* The session connection is broken. Surface an error so
             * the Architect can decide to restart. */
            isz_backend_set_error(st->backend, ISZ_ERR_SESSION_LOST);
        }
    }
#endif
```

`ISZ_ERR_SESSION_LOST` is a new error code; add it to the error enum
in `include/ishizue/isz_errors.h`. The wlroots pattern is to call
`wl_display_terminate(session->display)` on dispatch error [2], which
is more aggressive. Surfacing an error code lets the Architect decide.

### Summary of changes by file

`src/backend/isz_drm.c`:
- Remove `drmSetMaster` call in `isz_drm_init` (Fix 1).
- Remove `drmDropMaster` call in `drm_disable_seat` (Fix 2).
- Remove `drmSetMaster` call in `drm_enable_seat` (Fix 2).
- Remove `drm_fd < 0` guard in `drm_disable_seat` (Fix 3).
- Add `libseat_dispatch` call at end of `isz_drm_init` (Fix 4).
- Remove or no-op `isz_drm_on_session_active` and
  `isz_drm_on_session_inactive` (Fix 2).
- Handle `libseat_open_seat` failure per Option A or B (Fix 6).
- Check `libseat_dispatch` return value (Fix 8).
- Optionally implement direct VT handling (Fix 6 Option B, Fix 7).

`src/input/isz_session.c`:
- Delete `isz_session_init` and `isz_session_dispatch` (Fix 5).

`src/input/isz_seat_internal.h`:
- Delete declarations for `isz_session_init` and
  `isz_session_dispatch` (Fix 5).

`src/isz_lifecycle.c`:
- Remove `isz_session_dispatch(srv)` from `ISZ_FD_SEAT` case and from
  end of `isz_dispatch` (Fix 5).
- Remove `isz_drm_session_active_listener` and
  `isz_drm_session_inactive_listener` registration (Fix 2).

`include/ishizue/isz_errors.h`:
- Add `ISZ_ERR_SESSION_LOST` (Fix 8).

## 10. Emergency recovery

If a user is stuck on a TTY because a compositor (tinyisz or otherwise)
left the VT in `VT_PROCESS` mode with a dead `vt_pid`, here is how to
recover.

### Method 1: Magic SysRq + k (SAK)

Hold `Alt + SysRq`, press `k`, release. The kernel's Secure Access
Key (SAK) kills all processes on the current virtual console [13][14].
This kills the stuck compositor. The kernel then calls `reset_vc` for
the VT (because the controlling process is gone), which restores
`VT_AUTO` mode [4]. The user can then switch VTs normally.

The SysRq key is also labeled `Print Screen` on many keyboards [13].
Some keyboards require pressing `Alt`, then `SysRq`, then releasing
`SysRq`, then pressing `k`, then releasing everything [13].

SysRq must be enabled. Check with:

```
cat /proc/sys/kernel/sysrq
```

A value of 1 means all SysRq commands are enabled. A value of 0 means
disabled. To enable temporarily:

```
echo 1 | sudo tee /proc/sys/kernel/sysrq
```

To enable persistently, add `kernel.sysrq = 1` to
`/etc/sysctl.d/99-sysrq.conf`.

### Method 2: Magic SysRq + r (unraw) then switch

Hold `Alt + SysRq`, press `r`, release. This turns off keyboard raw
mode and sets it to XLATE [13]. If the compositor put the keyboard in
raw mode (which can interfere with VT switch keys), this restores
normal keyboard handling. Then try `Ctrl + Alt + F3` again.

### Method 3: Magic SysRq + e or + i (kill all processes)

Hold `Alt + SysRq`, press `e` to send `SIGTERM` to all processes
except init [13][14]. Wait a few seconds. If the compositor doesn't
exit, hold `Alt + SysRq`, press `i` to send `SIGKILL` to all processes
except init [13][14]. This forces the compositor to die. The kernel
then resets the VT.

These are more aggressive than SAK (which only kills processes on the
current console). They kill everything, including other TTY sessions.
Use only if SAK doesn't work.

### Method 4: SSH in and kill the compositor

From another machine, SSH into the stuck machine. Find the compositor
process:

```
pgrep -af tinyisz
pgrep -af seatd
```

Kill it:

```
sudo pkill -9 tinyisz
sudo pkill -9 seatd
```

If the compositor was using libseat builtin (forked child seatd
server), killing the parent tinyisz should also kill the child. If
not, kill the child explicitly.

After killing, check the VT mode:

```
sudo cat /sys/class/tty/tty0/active
```

This shows the active VT. To force a switch:

```
sudo chvt 3
```

If `chvt` hangs, the VT is still in `VT_PROCESS` mode with a dead
controller. The kernel should eventually reset it (via the `reset_vc`
path in `change_console` [4]), but it may take a manual trigger. Try
switching to a different VT first:

```
sudo chvt 1
sudo chvt 3
```

### Method 5: SSH in and force VT_SETMODE to VT_AUTO

Write a small C program that opens `/dev/tty<current_vt>` and calls
`ioctl(fd, VT_SETMODE, &mode)` with `mode.mode = VT_AUTO`. This
restores auto switching. The kernel's `vt_ioctl` handler accepts this
from any process with `CAP_SYS_TTY_CONFIG` [4].

Sketch:

```c
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int fd = open("/dev/tty1", O_RDWR);
    struct vt_mode mode = {0};
    mode.mode = VT_AUTO;
    ioctl(fd, VT_SETMODE, &mode);
    close(fd);
    return 0;
}
```

Run as root. Replace `/dev/tty1` with the stuck VT.

### Method 6: Reboot via SysRq + b (last resort)

Hold `Alt + SysRq`, press `s` (sync filesystems), wait for the sync
to complete (the console prints "Emergency Sync complete" or similar).
Then hold `Alt + SysRq`, press `b` to reboot immediately [13][14].
This is the equivalent of pressing the reset button. Unsynced data is
lost.

The full "safe reboot" sequence is `Alt + SysRq + R`, then `E`, then
`I`, then `S`, then `U`, then `B`. Mnemonic: "Raising Elephants Is
So Utterly Boring" [14].

### Prevention

To prevent the bug from recurring:

- Always run tinyisz under a seatd daemon or logind session. Do not
  rely on the direct-open fallback.
- If running tinyisz as root from a TTY without seatd or logind, start
  seatd first: `sudo seatd -g video` (or whatever group owns the DRM
  device). Then run tinyisz as a user in that group.
- If running tinyisz with `LIBSEAT_BACKEND=builtin`, ensure the
  builtin backend's forked child is reaped on exit. If tinyisz is
  killed with SIGKILL, the child may survive and keep `VT_PROCESS`
  mode. Use `SIGTERM` first.
- Enable SysRq permanently (`kernel.sysrq = 1`) so recovery method 1
  is always available.

## References

[1] wlroots `backend/drm/backend.c`, master branch.
https://github.com/swaywm/wlroots/blob/master/backend/drm/backend.c

[2] wlroots `backend/session/session.c`, master branch. This file
contains the libseat integration (the file is named `session.c` but
implements the libseat backend; the libseat-specific listener and
event source are here).
https://github.com/swaywm/wlroots/blob/master/backend/session/session.c

[3] `ioctl_vt(2)` Linux manual page.
https://man7.org/linux/man-pages/man2/ioctl_vt.2.html

[4] Linux kernel `drivers/tty/vt/vt_ioctl.c`, master branch. The
`vt_reldisp`, `change_console`, and `complete_change_console`
functions.
https://github.com/torvalds/linux/blob/master/drivers/tty/vt/vt_ioctl.c

[5] seatd `seatd/seat.c`, master branch. `seat_vt_release`,
`seat_disable_client`, `seat_ack_disable_client`, `seat_activate`.
https://github.com/kennylevinsen/seatd/blob/master/seatd/seat.c

[6] seatd `common/terminal.c`, master branch. `terminal_open`,
`terminal_set_process_switching`, `terminal_ack_release`,
`terminal_ack_acquire`.
https://github.com/kennylevinsen/seatd/blob/master/common/terminal.c

[7] seatd `libseat/backend/seatd.c`, master branch. The seatd and
builtin backend implementation: `_open_seat`, `dispatch`,
`execute_events`, `disable_seat`, `read_until_response`,
`poll_connection`.
https://github.com/kennylevinsen/seatd/blob/master/libseat/backend/seatd.c

[8] seatd `include/libseat.h`, master branch. The public libseat API
documentation for `libseat_open_seat`, `libseat_disable_seat`,
`libseat_dispatch`, `libseat_get_fd`.
https://github.com/kennylevinsen/seatd/blob/master/include/libseat.h

[9] Aquamarine `src/backend/Session.cpp`, main branch. Hyprland's
session layer: `libseatEnableSeat`, `libseatDisableSeat`,
`CSession::pollFDs`, `CSession::dispatchLibseatEvents`,
`CSession::switchVT`.
https://github.com/hyprwm/aquamarine/blob/main/src/backend/Session.cpp

[10] seatd `libseat/backend/logind.c`, master branch. The logind
backend: `disable_seat` (no-op), `handle_pause_device`,
`handle_properties_changed`, `set_active`.
https://github.com/kennylevinsen/seatd/blob/master/libseat/backend/logind.c

[11] weston `src/launcher-direct.c` (ogon-project fork, mirrors the
upstream weston code). `setup_tty`, `vt_handler`, teardown.
https://github.com/ogon-project/weston-ogon/blob/master/src/launcher-direct.c

[12] weston `src/launcher-logind.c` (hardening mirror). `device_paused`,
`device_resumed`, `launcher_logind_pause_device_complete`,
`launcher_logind_set_active`.
https://github.com/hardening/weston/blob/master/src/launcher-logind.c

[13] Linux Magic System Request Key Hacks, kernel documentation.
https://docs.kernel.org/admin-guide/sysrq.html

[14] Magic SysRq key, Wikipedia.
https://en.wikipedia.org/wiki/Magic_SysRq_key
