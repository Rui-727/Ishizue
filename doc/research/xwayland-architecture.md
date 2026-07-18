# Xwayland architecture deep-dive

Research notes for Ishizue's X11 compatibility bridge (SPEC §13). The bridge
is a separate process that connects to Ishizue as an ordinary client over
the native wire protocol, listens on `/tmp/.X11-unix/X<display>`, and
translates X11 requests into Ishizue surface/buffer/input operations. The
closest existing analog is Xwayland, which does the same job for Wayland
compositors. This document reverse-engineers Xwayland's architecture from
its source code and protocol specs so we can decide what to copy and what
to do differently.

Sources are cited inline as `[N]` and listed at the end. The Xwayland
source references point at the `mirror/xserver` GitHub mirror of the
freedesktop `xorg/xserver` repository, branch `master`.

## 1. History and provenance

Xwayland is the Xorg X server with a different backend. It lives in the
same source tree as Xorg (`xorg/xserver`), shares the dix (device-
independent X), mi (machine-independent rendering), GLX, render, composite,
randr, xkb, and glamor code, and replaces only the `hw/xfree86` driver
and KMS backend with `hw/xwayland`. The Wayland book puts it bluntly:
"Xwayland is a complete X11 server, just like Xorg is, but instead of
driving the displays and opening input devices, it acts as a Wayland
client." [1]

Kristian Høgsberg started Wayland in 2008 [12] while at Red Hat, and the
Xwayland backend was the practical answer to the X11 compatibility
question from the start: rather than write a new X11 translator inside
every compositor, ship the existing X server with a Wayland output path
and let compositors treat it as a regular client. The code originally
lived in a separate branch and was merged into the main `xserver` tree
around 2014 (the `xwayland-screen.c` copyright header reads "Copyright
2011-2014 Intel Corporation"). Since then it is built from the same
source as Xorg, just with a different `hw/` backend selected at build
time. The standalone `xorg-xwayland` distribution package (Arch version
24.1.13 at time of writing [2]) is the same code, packaged separately.

Why it exists: rewriting X11 inside a compositor is intractable. The
Wayland book again: "the gigantic implementation effort needed to
support X11 makes it intractable to just write X11 support directly in
a Wayland compositor. The implementation would be nothing short of a
real X11 server." [1] Reusing the Xorg server gives compositors full
X11 protocol coverage for free, at the cost of one extra process per
session.

## 2. Process model

Xwayland is a separate process spawned by the Wayland compositor. It is
not a library. The compositor launches `Xwayland` with a small set of
command-line arguments that hand over pre-bound file descriptors, plus
the standard environment variables `WAYLAND_DISPLAY` and
`XDG_RUNTIME_DIR` so the new process can connect back to the compositor
as a Wayland client.

The argument set, taken verbatim from `ddxUseMsg` in
`hw/xwayland/xwayland.c` [4]:

```
-rootless              run rootless, requires wm support
-fullscreen            run fullscreen when rootful
-geometry WxH          set Xwayland window size when rootful
-host-grab             disable host keyboard shortcuts when rootful
-wm fd                 create X client for wm on given fd
-initfd fd             add given fd as a listen socket for initialization clients
-listenfd fd           add given fd as a listen socket
-listen fd             deprecated, use "-listenfd" instead
-shm                   use shared memory for passing buffers
-eglstream             use eglstream backend for nvidia GPUs
```

The fd arguments are the important part. The compositor creates the
listening X11 socket itself (binds `/tmp/.X11-unix/X<n>`), creates a
socketpair for the XWM, optionally a socketpair for "initialization"
clients, and passes them in via `-listenfd`, `-wm`, and `-initfd`.
`ddxProcessArgument` in `xwayland.c` [4] parses them into
`listen_fds[]`, `wm_fd`, and `init_fd`.

`InitOutput` in `xwayland.c` [4] then decides how to wire them up:

```c
if (wm_fd >= 0 || init_fd >= 0) {
    if (wm_fd >= 0)
        TimerSet(NULL, 0, 1, add_client_fd, NULL);
    if (init_fd >= 0)
        ListenOnOpenFD(init_fd, FALSE);
    AddCallback(&SelectionCallback, wm_selection_callback, NULL);
}
else if (listen_fd_count > 0) {
    listen_on_fds();
}
```

`add_client_fd` calls `AddClientOnOpenFD(wm_fd)`, which adds the
compositor as an X11 client over the wm fd. The compositor, in its XWM
role, then acquires the `WM_S0` selection on the X server.
`wm_selection_callback` [4] fires when that selection is set and only
then calls `listen_on_fds()`, which starts accepting regular X clients.
This is the actual "Xwayland is ready for clients" gate: the XWM must
be in place first.

Readiness has two layers:

1. **Process readiness**: the compositor wants to know Xwayland has a
   display bound and is accepting the XWM. This is signaled via the
   `-displayfd fd` argument, handled in `os/utils.c` (parsed into the
   static `displayfd` variable) and written by `NotifyParentProcess` in
   `os/connection.c` [9]:

   ```c
   void NotifyParentProcess(void) {
       if (displayfd >= 0) {
           write(displayfd, display, strlen(display));
           write(displayfd, "\n", 1);
           close(displayfd);
           displayfd = -1;
       }
       if (RunFromSmartParent) {
           if (ParentProcess > 1)
               kill(ParentProcess, SIGUSR1);
       }
       if (RunFromSigStopParent)
           raise(SIGSTOP);
       sd_notify(0, "READY=1");
   }
   ```

   `NotifyParentProcess` is called from `dix/main.c` [10] right after
   `CreateConnectionBlock()` and before `Dispatch()`. The compositor
   watches its end of the displayfd socketpair in its event loop; when
   it becomes readable, it reads the display number (e.g. `:20\n`) and
   knows Xwayland is up. The legacy `SIGUSR1` path (`-sharevts` /
   `-noListenAll` from the Xorg `init` script era) is still supported
   for backwards compatibility but is not what modern compositors use.
   The `sd_notify(0, "READY=1")` line lets Xwayland be supervised by
   systemd as a service unit if the compositor happens to be a systemd
   user service.

2. **WM readiness**: the compositor (as XWM) has acquired `WM_S0`,
   after which Xwayland starts accepting X client connections on the
   listen fds. This is gated by `wm_selection_callback` above.

The wlroots `wlr_xwayland_server` API formalizes both: it exposes a
`server_start` signal (process spawned) and a `server_ready` signal
(displayfd fired), and compositors that want lazy startup use
`wlr_xwayland_create(..., lazy=true)` so the Xwayland process is not
forked until a client connects to the listen socket [11].

## 3. Rootful vs rootless

The Wayland book [1] defines the two modes:

- **Rootful**: the entire X11 screen is one Wayland surface. X11 clients
  see a normal root window and can run their own X11 window manager
  inside it. Foreign windows do not integrate with the native desktop.
  Useful for sandboxes, emulators, and "X11 desktop in a window" tools
  like `gamescope`'s rootful mode.
- **Rootless**: each X11 top-level window is a separate Wayland surface.
  X11 windows stack and mix freely with native windows. The Wayland
  compositor must act as the X11 window manager; no other X11 WM can
  run. This is harder to implement but is the mode desktop compositors
  use by default.

The man page [2] says Xwayland "usually runs rootless so that X clients
integrate seamlessly with the rest of the Wayland desktop." Rootful is
"mainly for testing purposes" when launched by a user, though recent
work (rootful part 1/2 blog series by Olivier Fourdan, covered by
Phoronix [21]) has made rootful useful for sandboxed X11 sessions
inside `gamescope` and similar.

The `-rootless` flag is parsed in `xwl_screen_init` (`xwayland-screen.c`
[5]). In rootless mode `xwl_screen->root_clip_mode = ROOT_CLIP_INPUT_ONLY`,
which means the root window exists for input routing and ICCCM
bookkeeping but has no backing pixmap; redirected (composited) windows
are the only thing rendered. In rootful mode the root window gets a
real backing pixmap (`ROOT_CLIP_FULL`), and the whole root is one
`wl_surface` committed to the compositor as a single toplevel.

Recommendation for Ishizue: default to **rootless** for the same reason
every desktop Wayland compositor does. Rootful is a niche feature for
sandboxes and can be added later as a `-rootful` flag on the bridge
that creates one Ishizue surface sized to the root and never creates
per-window surfaces. SPEC §1 says Ishizue is mechanism-only and the
Architect owns all window-management policy, which already matches the
rootless Xwayland model where the compositor is the WM.

## 4. Glamor and the buffer path

Glamor is Xorg's GL-accelerated 2D rendering layer. It implements the
X11 Rendering extension (and most of fb, the software fallback) on top
of OpenGL or GLES, drawing into pixmaps backed by GL textures or
renderbuffers. In Xwayland, glamor is what turns X11 drawing requests
into GPU buffers the compositor can composite.

The pipeline, traced from `xwl_window_post_damage` in
`xwayland-window.c` [6]:

```c
region = DamageRegion(window_get_damage(xwl_window->window));
pixmap = xwl_window_buffers_get_pixmap(xwl_window, region);

if (xwl_screen->glamor)
    buffer = xwl_glamor_pixmap_get_wl_buffer(pixmap);
else
    buffer = xwl_shm_pixmap_get_wl_buffer(pixmap);

if (xwl_screen->glamor)
    xwl_glamor_post_damage(xwl_window, pixmap, region);

wl_surface_attach(xwl_window->surface, buffer, 0, 0);
/* ... wl_surface_damage calls ... */
xwl_window_create_frame_callback(xwl_window);
DamageEmpty(window_get_damage(xwl_window->window));
```

The `xwl_glamor_pixmap_get_wl_buffer` call [7] dispatches through an
EGL backend vtable:

```c
struct wl_buffer *
xwl_glamor_pixmap_get_wl_buffer(PixmapPtr pixmap) {
    struct xwl_screen *xwl_screen = xwl_screen_get(pixmap->drawable.pScreen);
    if (xwl_screen->egl_backend->get_wl_buffer_for_pixmap)
        return xwl_screen->egl_backend->get_wl_buffer_for_pixmap(pixmap);
    return NULL;
}
```

The EGL backend is selected at startup. As of 2018 the GBM-specific
code was split out of `xwayland-glamor.c` into
`xwayland-glamor-gbm.c`, leaving a backend-agnostic core that can host
either the GBM backend (most drivers) or the EGLStream backend
(NVIDIA). See the Lyude Paul patch on the xorg-devel list [18] for the
history.

For the GBM backend, the flow per pixmap is:

1. Glamor renders the X11 client's drawing into a GL texture backed by
   a `gbm_bo` (allocated via `gbm_bo_create_with_modifiers2`).
2. `gbm_bo_get_fd_for_plane` exports the bo as a dma-buf fd per plane.
3. `zwp_linux_dmabuf_v1_create_params` builds a `wl_buffer` from the
   fd + format + modifier + per-plane offset/stride tuple, using
   modifier and format lists advertised by the compositor via
   `zwp_linux_dmabuf_feedback_v1` (the `xwl_dmabuf_feedback_*`
   listeners in `xwayland-glamor.c` [7] handle the feedback tranches
   and pick formats/modifiers that match the compositor's main device).
4. The `wl_buffer` is attached to the window's `wl_surface` in the next
   `xwl_window_post_damage` call.

If glamor is disabled (`-shm` or `XWAYLAND_NO_GLAMOR`), the bridge
falls back to `wl_shm`: pixmaps are CPU-side, and `xwl_shm_pixmap_get_wl_buffer`
exports a `wl_shm` buffer. This is the slow path, used only for
debugging or on systems without GL.

The `-shm` flag [2] forces this path. The `-glamor gl|es|off` flag
selects between desktop GL, GLES, and no glamor.

### Ishizue equivalent

SPEC §7 already has the building blocks: `isz_surface_attach_buffer`
accepts a dma-buf fd with format/modifier/stride per plane, exactly
the parameters Xwayland hands to `zwp_linux_dmabuf_v1_params`. The
Ishizue bridge should:

- Render X11 client drawing into a `gbm_bo` via glamor (or, on the
  N4100 reference hardware in SPEC §14, via the i965/iris Mesa driver
  using EGL + GBM from the render node).
- Export the bo as a dma-buf fd, build an `isz_buffer_desc` matching
  §7.1, and call `isz_surface_attach_buffer`.
- Use the existing `ISZ_MSG_BUFFER_RELEASE` event (§7.5) for
  synchronization, equivalent to `wl_buffer.release`.

A pure-CPU fallback (render into `malloc`'d memory, attach as
`ISZ_BUFFER_KIND_SHM`, per §11 fallback matrix) is the equivalent of
Xwayland's `-shm` mode and should be available for testing on
headless.

## 5. Damage tracking across the boundary

Xwayland uses the X11 Damage extension to learn what changed in an X11
window, then translates that to `wl_surface_damage` (or
`wl_surface_damage_buffer`) on the corresponding Wayland surface.

The translation is in `xwl_window_post_damage` [6], shown in part in
the previous section. The damage-rect emission logic is what is most
relevant to Ishizue:

```c
if (RegionNumRects(region) > 256) {
    box = RegionExtents(region);
    xwl_surface_damage(xwl_screen, xwl_window->surface,
                       box->x1 + borderWidth, box->y1 + borderWidth,
                       box->x2 - box->x1, box->y2 - box->y1);
} else {
    box = RegionRects(region);
    for (i = 0; i < RegionNumRects(region); i++, box++) {
        xwl_surface_damage(xwl_screen, xwl_window->surface,
                           box->x1 + borderWidth, box->y1 + borderWidth,
                           box->x2 - box->x1, box->y2 - box->y1);
    }
}
```

1. **Per-rect damage with a fallback to the bounding box at 256 rects.**
   The comment says "Arbitrary limit to try to avoid flooding the
   Wayland connection. If we flood it too much anyway, this could abort
   in libwayland-client." The number is hardcoded. SPEC §7.9 mandates
   list-of-rects damage for the same reason Xwayland does it: a single
   bounding box forces a full-region recomposite on any multi-region
   update. Ishizue should adopt the same per-rect list semantics. The
   256-rect threshold is a reasonable default and could be made
   configurable.

2. **Coordinate space.** `xwl_surface_damage` in `xwayland-screen.c`
   [5] prefers `wl_surface_damage_buffer` (buffer coordinates) over
   the older `wl_surface_damage` (surface coordinates) when the
   compositor supports version 4+ of `wl_surface`:

   ```c
   void xwl_surface_damage(struct xwl_screen *xwl_screen,
                           struct wl_surface *surface,
                           int32_t x, int32_t y, int32_t w, int32_t h) {
       if (wl_surface_get_version(surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
           wl_surface_damage_buffer(surface, x, y, w, h);
       else
           wl_surface_damage(surface, x, y, w, h);
   }
   ```

   Buffer-coordinate damage is correct when the buffer is scaled
   relative to the surface (HiDPI with `wp_viewport` set); surface-
   coordinate damage would force the compositor to scale the rects.
   SPEC §7.9 already uses surface-local coordinates for damage, with
   the convention "inclusive of left/top, exclusive of right/bottom."
   This matches the Wayland convention exactly, so no translation is
   needed.

3. **Coalescing across X11 paint requests.** X11 clients can issue
   many small paint operations per frame; the X server's Damage
   extension accumulates them into a region. Xwayland reads the region
   once per frame callback, attaches one buffer, emits the (possibly
   coalesced) damage rects, and calls `DamageEmpty` to reset. The
   frame callback (`xwl_window_create_frame_callback`) is what
   throttles this to display refresh and prevents the bridge from
   flooding the compositor with buffers it cannot display. Ishizue's
   §7.3 frame scheduling already provides this primitive; the bridge
   should not attach a new buffer until the previous frame's
   `ISZ_EVENT_FRAME_PRESENTED` arrives, mirroring the Wayland frame
   callback.

For deeper background on damage tracking (frame damage vs. buffer
damage, buffer age), see emersion's writeup [13].

## 6. Input forwarding

Xwayland is a Wayland client. It binds `wl_seat`, receives
`wl_keyboard`, `wl_pointer`, and `wl_touch` events from the compositor,
and translates them into X11 core + XInput events that it then
delivers to whatever X11 window has focus.

Keyboard focus on the Wayland side arrives via `wl_keyboard.enter` /
`wl_keyboard.leave`. The handlers in `xwayland-input.c` [8]:

```c
static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial,
                                  struct wl_surface *surface,
                                  struct wl_array *keys) {
    struct xwl_seat *xwl_seat = data;
    if (surface != NULL && !is_surface_from_xwl_window(surface))
        return;
    xwl_seat->xwl_screen->serial = serial;
    xwl_seat->keyboard_focus = surface;
    wl_array_copy(&xwl_seat->keys, keys);
    wl_array_for_each(k, &xwl_seat->keys)
        QueueKeyboardEvents(xwl_seat->keyboard, EnterNotify, *k + 8);
    maybe_fake_grab_devices(xwl_seat);
}
```

The `is_surface_from_xwl_window` check is important: the compositor
tells Xwayland via Wayland that keyboard focus moved to a particular
`wl_surface`, and Xwayland synthesizes X11 `EnterNotify` events for
the corresponding X11 window. Pointer events (`wl_pointer.motion`,
`button`, `axis`) similarly translate to `MotionNotify`,
`ButtonPress`/`ButtonRelease`, and button-scroll events.

The X11 side of focus is owned by the compositor in its XWM role. The
XWM calls `XSetInputFocus` on Xwayland (over the wm_fd) to set the X11
keyboard focus to the X11 window that corresponds to the focused
`wl_surface`. This is a separate channel from the Wayland keyboard
focus: the Wayland focus is what generates EnterNotify/LeaveNotify and
gates key delivery, while the X11 input focus is what XGetInputFocus
returns and what clients that grab the keyboard actively see. The
compositor must keep both in sync. The wlroots API exposes this as the
ICCCM input model enum `wlr_xwayland_icccm_input_model` [11]:
`NONE`, `PASSIVE`, `LOCAL`, `GLOBAL`, picking which X11 focus policy
the XWM uses per surface based on the client's `WM_TAKE_FOCUS`
WM_PROTOCOLS hint.

The man page's `-noTouchPointerEmulation` flag [2] lets the compositor
disable Xwayland's built-in touch-to-pointer emulation, so the
compositor can implement its own.

### Ishizue equivalent

SPEC §9's `isz_seat_set_keyboard_focus(seat, surf)` is the compositor
side of the contract: the Architect calls it to set focus on an
Ishizue surface. The bridge receives `ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED`
(SPEC §9) and must translate that into both:

1. **Wayland-style keyboard focus**: deliver X11 `EnterNotify` to the
   X11 window mapped to the focused Ishizue surface, and start
   delivering KeyPress/KeyRelease to it. This is the same flow as
   `keyboard_handle_enter` above.
2. **X11 input focus**: the bridge, acting as a privileged Ishizue
   client, must track which X11 window currently has X11 input focus
   (set via `SetInputFocus` requests that the bridge receives from...
   itself, since there is no separate XWM process). The bridge is both
   the X server and the XWM, because SPEC §1 puts all WM policy in the
   Architect and the bridge is just a translator.

This is one of the few places where the Xwayland model (two processes:
Xwayland + XWM in compositor) does not map directly to Ishizue (one
process: the bridge doing both jobs). The bridge needs an internal
"XWM" thread or coroutine that maintains the X11 focus state in
response to Ishizue focus events.

## 7. Selection and clipboard

X11 has two selections: `PRIMARY` (the selection, set by mouse select,
pasted by middle-click) and `CLIPBOARD` (the clipboard, set by Ctrl+C,
pasted by Ctrl+V). Both are ICCCM protocols built on top of the core
`SetSelectionOwner` request. Wayland's core protocol has only
`wl_data_device.selection` (the clipboard); `PRIMARY` is added by the
`zwp_primary_selection_v1` protocol [20], which mirrors the clipboard
API.

The bridging happens in the compositor's XWM, not in Xwayland itself.
Xwayland is just an X server; the compositor connects to it as a
special X11 client and watches selection ownership via XFixes
(`XFixesSelectionNotify`). When an X11 client takes ownership of
`CLIPBOARD`, the XWM creates a `wl_data_source` on the Wayland side
and offers the X11 targets (translated to MIME types). When a Wayland
client requests the data, the XWM does the ICCCM dance: sends an
`XConvertSelection` to the X11 owner, reads the property the owner
writes, pipes it to the Wayland `wl_data_source.send` fd. The reverse
direction (Wayland source, X11 requestor) is symmetric: the XWM sets
itself as the X11 `CLIPBOARD` owner and, on `XSelectionRequest`,
reads from the `wl_data_offer` fd and writes the X11 property.

The mechanics are described in detail in the xeechou post [15], which
calls the result "ugly, long and hard to maintain." The hard parts
are:

- **INCR protocol.** When the data does not fit in a single property
  change, the ICCCM INCR protocol splits it across multiple
  `PropertyNotify` events. The XWM has to drive both an X11
  PropertyNotify-driven state machine and a Wayland pipe fd, watching
  for both to be ready simultaneously.
- **MIME type translation.** X11 targets are atoms (`UTF8_STRING`,
  `TEXT`, `STRING`, `image/png`, `text/uri-list`, etc.). Wayland uses
  MIME strings. The mapping is mostly 1:1 for the `text/*` and
  `image/*` cases but has edge cases (`TARGETS`, `TIMESTAMP`,
  `MULTIPLE`).
- **Primary vs clipboard.** KWin's approach [16] is to run a small
  helper binary that uses Qt's X11 clipboard code on one side and
  KWayland on the other, which is simpler than re-implementing ICCCM.
  Most other compositors implement it directly in the XWM.

A key observation from Martin Flöser [16]: "the X11 clipboard is part
of ICCCM and not a core feature of the X-Server. It's a communication
protocol between clients." This is why the bridge in the XWM does the
work, not Xwayland itself.

### Ishizue equivalent

SPEC §6.8 already defines a clipboard model: DMA-BUF for images,
memfd for text, transferred as fds via SCM_RIGHTS, tagged with a MIME
type. The library does not parse or convert. This is the Wayland
model, restated.

The Ishizue bridge, in its XWM role, needs to:

- Track X11 selection ownership via XFixes on the X11 side.
- Translate `CLIPBOARD` ownership to `isz_clipboard_set_owner` with
  the offered MIME types.
- Translate `PRIMARY` ownership to whatever Ishizue defines for
  primary selection. SPEC §6.8 only describes the clipboard; the
  primary selection is not explicitly defined. The bridge should push
  the SPEC to add a primary-selection API, or treat PRIMARY as a
  second clipboard channel (with its own owner/mime list) under a
  different API name.
- On `ISZ_EVENT_CLIPBOARD_REQUEST` (SPEC §6.8), drive the ICCCM
  `XConvertSelection` dance against the X11 owner and write to the
  provided fd. INCR must be handled.
- Architect policy (SPEC §6.8) is implemented via
  `ISZ_EVENT_CLIPBOARD_REQUEST` listener, which is exactly where
  content-type filtering lives.

## 8. Drag and drop

Xdnd is the de facto X11 drag-and-drop protocol, built on the X11
selection mechanism (it uses `XdndSelection` and a set of atoms
`XdndEnter`, `XdndPosition`, `XdndDrop`, `XdndLeave`, `XdndFinished`).
Wayland's core protocol has `wl_data_device.start_drag`,
`wl_data_device.motion`, `wl_data_device.drop`, etc. Both use the same
fd-based data transfer as the clipboard.

The XWM translates between them, similar to the clipboard case. When
an X11 client starts a drag (`XdndEnter` on the X root), the XWM
creates a `wl_data_source` and calls `wl_data_device.start_drag` on
the Wayland side, using the surface under the X11 pointer as the drag
origin. Wayland `wl_data_device.motion` events are translated back to
`XdndPosition` on the appropriate X11 window; `wl_data_device.drop`
becomes `XdndDrop`.

Cross-protocol DnD (X11 client dragging onto a native window and vice
versa) is where it gets ugly. The emersion post [14] describes the
Wayland-side mechanics; the Fedora bug tracker [17] and the niri
discussion [22] document the X11-to-Wayland and Wayland-to-X11 edge
cases that have historically broken:

- The X11 drag origin surface and the Wayland drag icon surface must
  be the same `wl_surface`, but the compositor decides what is under
  the pointer, and an X11 client's idea of "the window I dragged
  over" (driven by `XdndPosition` replies) does not always match the
  compositor's idea of "the surface under the pointer" (driven by
  `wl_data_device.motion`).
- Wayland `wl_data_source` cancel does not map cleanly to Xdnd
  `XdndLeave`, especially when the drop happens outside any accepting
  surface.
- Java's AWT has had repeated bugs here [23]; KDE's kwayland-server
  has a separate fix for a crash on X11-to-Wayland DnD [24].

### Ishizue equivalent

SPEC §6.9 defines drag-and-drop with `drag_start`, `drag_motion`,
`drag_accept`/`drag_reject`, `drag_drop`, using the same fd-passing
mechanism as the clipboard. The bridge should translate Xdnd to this
API. The known-broken edge cases in Xwayland should be designed
around from day one: have a single source of truth for "what surface
is under the pointer" on the Ishizue side, and do not let the X11
client's `XdndPosition`-based notion of the target window diverge
from it.

## 9. Window management

In a normal X11 session, the X window manager reparents each top-level
client window into a frame window, draws decorations into the frame,
and handles interactive resize/move through the frame. Xwayland in
rootless mode does not do this, because the Wayland compositor draws
decorations and handles interactive resize directly.

The Wayland book [1] describes the architecture:

- Two asynchronous channels exist between Xwayland and the compositor:
  the Wayland protocol (Xwayland as Wayland client) and the X11
  protocol (compositor-as-XWM as X11 client over the wm fd).
- The XWM is a normal X11 WM from Xwayland's perspective: it
  subscribes to `MapRequest`, `ConfigureRequest`, `DestroyNotify`,
  property changes on top-level windows, etc. It reparents top-level
  windows into frame windows of its own (or, more commonly in modern
  XWMs, it does not reparent at all and instead redirects the window
  via `XCompositeNameWindowPixmap`, then draws decorations as
  separate Wayland surfaces around it).
- The XWM-to-WWM (Wayland Window Manager) bridge is what makes the
  X11 window appear as a Wayland toplevel: it tracks the X11 window's
  geometry, maps it to a `wl_surface` (or, with `xwayland-shell-v1`,
  to a `xwayland_surface_v1`), and handles stacking, focus, and
  workspace placement.

The XWM must be carefully written to avoid deadlocks. The book [1]
warns: "It is often nearly impossible to prove that synchronous or
blocking X11 calls from XWM cannot cause a deadlock, and therefore it
is strongly recommended to make all X11 communications asynchronous.
All Wayland communications are already asynchronous by design." Two
processes both blocking on each other over two separate sockets is a
classic deadlock pattern; the XWM has to use xcb's async API and
dispatch X11 replies in its main loop.

Window identification (the wl_surface <-> X11 window mapping) is the
core problem. The book [1] describes the legacy mechanism: Xwayland
sends an `XClientMessageEvent` of type atom `WL_SURFACE_ID` on the X11
window, carrying the `wl_surface`'s id as the first 32-bit data
element. The compositor's XWM reads this and associates the X11
window with the Wayland surface. The order is not guaranteed: the
`wl_surface` creation request and the `WL_SURFACE_ID` client message
can arrive in either order, so the XWM has to buffer one until the
other arrives. This race is what `xwayland-shell-v1` was written to
fix.

Interactive resize has its own synchronization issue. Vlad
Zahorodnii's post [17] describes the `_NET_WM_SYNC_REQUEST` protocol
and why it does not work out of the box under Xwayland: the X11 frame
synchronization protocol assumes the compositor grabs window contents
via `XCompositeNameWindowPixmap()`, but Wayland compositors get
contents via `wl_buffer` attach on the `wl_surface`, so the XSync
counter update and the new-size buffer attach are not synchronized.
KWin's fix was to add a separate "wait for the new-size buffer before
unblocking updates" check in the XWM.

### Ishizue equivalent

SPEC §1 puts all WM policy in the Architect. The Ishizue bridge acts
as both the X11 server and the XWM (since there is no separate
compositor process for the bridge to delegate to). The bridge should:

- Receive X11 `MapWindow` requests for top-level windows and create
  Ishizue surfaces for them.
- Expose window properties (title, app_id, geometry, decorations
  hint, modal state) via `ISZ_MSG_SURFACE_SET_*` requests so the
  Architect can apply policy.
- Accept `ISZ_EVENT_*` from the Architect (configure, focus, close,
  minimize, maximize, fullscreen, restack) and translate them to the
  corresponding X11 messages (`ConfigureNotify`, `XSetInputFocus`,
  `WM_DELETE_WINDOW` ClientMessage, etc.).
- Never reparent: the Architect draws decorations, not the bridge.
- Avoid the `_NET_WM_SYNC_REQUEST` problem by tying resize
  acknowledgment to `ISZ_EVENT_FRAME_PRESENTED` on the new size,
  which is the same fix KWin arrived at.

## 10. The xwayland-shell-v1 protocol

`xwayland-shell-v1` [3] is a Wayland protocol extension added to
wayland-protocols (staging) in 2022 by Joshua Ashton. It formalizes
the wl_surface <-> X11 window association that was previously done
via the `WL_SURFACE_ID` client message.

The protocol description from the XML [3]:

> Protocol for associating X11 windows to wl_surfaces. This protocol
> adds a xwayland_surface role which allows an Xwayland server to
> associate an X11 window to a wl_surface. Before this protocol, this
> would be done via the Xwayland server providing the wl_surface's
> resource id via the a client message with the WL_SURFACE_ID atom on
> the X window. This was problematic as a race could occur if the
> wl_surface associated with a WL_SURFACE_ID for a window was
> destroyed before the client message was processed by the compositor
> and another surface (or other object) had taken its id due to
> recycling.

The protocol has two interfaces:

- `xwayland_shell_v1`: a singleton global. The compositor advertises
  it; only Xwayland is allowed to bind it. `get_xwayland_surface(id,
  surface)` creates a `xwayland_surface_v1` for a given `wl_surface`.
- `xwayland_surface_v1`: the role object. `set_serial(serial_lo,
  serial_hi)` associates the surface with an X11 window identified by
  a 64-bit monotonic serial. The same serial is sent to the
  compositor's XWM via an `XClientMessageEvent` of type
  `WL_SURFACE_SERIAL` on the X11 window. The compositor matches them.

The serial is the key part. Wayland object ids are recycled, so a
`wl_surface` id alone is not enough to identify a window
unambiguously across time. The serial is a 64-bit monotonic value
Xwayland generates per window, never reused, and is matched on both
sides. This closes the race.

The protocol is bound only by Xwayland; the compositor is supposed to
hide it from other clients in `wl_registry` [3]. As of late 2025,
every major compositor implements it (Cage 0.2.0, COSMIC 1.0-beta.8,
GameScope 3.15.14, Hyprland 0.52.1, KWin 6.6, Labwc 0.9.2, Mutter
49.2, niri 25.11, Sway 1.11, Weston 14.0.2, etc., per the compositor
support table on the protocol page [3]).

### Ishizue equivalent

Ishizue's bridge is in the same process as the XWM, so the
wl_surface <-> X11 window association does not cross a process
boundary and does not need a wire protocol to synchronize. The
bridge maintains an in-memory `x11_window -> isz_surface` map. There
is no race to close because there is no second process generating
ids that could be recycled out from under the bridge.

That said, Ishizue should still expose a `surface_serial` or
equivalent on `ISZ_MSG_SURFACE_CREATE` so that the Architect can
distinguish "this surface is a fresh X11 window" from "this surface
id was recycled." SPEC §6.4 does not currently define this. It is a
minor gap; the bridge can paper over it by reusing the X11 window id
(32-bit) as the serial, since X11 window ids within a single X
server are also recycled but Xwayland does not recycle them while a
window is mapped.

## 11. Multi-Xwayland instances

A single Xwayland instance per session has been the default for a
decade, and the Wayland book [1] says so explicitly: "A Wayland
compositor usually spawns only one Xwayland instance. This is because
many X11 applications assume they can communicate with other X11
applications through the X server, and this requires a shared X
server instance."

The downside of the single-instance model is that all X11 clients
share a server and can attack each other through it: any X11 client
can read the keyboard state of any other, snoop on the clipboard,
inject events, etc. This is the X11 security model, which Xwayland
inherits. Sandboxing X11 clients from each other requires running
separate Xwayland instances per client (or per sandbox), which breaks
inter-client X11 communication but is the only way to get isolation.

The `-initfd` flag [2] was added for the on-demand / per-application
use case. The man page describes it as aimed at "Wayland servers
which run Xwayland on-demand, to be able to spawn specific X clients
which need to complete before other regular X clients can connect to
Xwayland, like xrdb." In practice it lets the compositor do early
X11 setup (load X resources, set root window properties) before
regular X clients can connect.

wlroots split its API to make multi-instance easier. The
`wlr_xwayland_server` type [11] manages the Xwayland process (fork,
exec, parse displayfd, expose ready signal), while `wlr_xwayland`
manages the XWM + protocol on top of a server. The
`wlr_xwayland_create_with_server(display, compositor, server)` API
lets a compositor attach an XWM to an existing server, so a
compositor can run multiple servers and one shared XWM, or one
server and multiple XWMs, depending on the isolation model it wants.

Real-world deployments:

- `gamescope` runs Xwayland rootful and uses it as a sandboxed X11
  session for a single game.
- Flatpak and bwrap-based sandboxes [25] typically give the sandboxed
  app its own Xwayland socket (a fresh listen fd) but do not
  necessarily spawn a new Xwayland process; they rely on the
  compositor's Xwayland with a separate auth cookie.
- COSMIC and other newer compositors have experimented with
  per-app Xwayland instances for sandboxing.

### Ishizue equivalent

SPEC §13 says the bridge is "a separate process, not part of the
library." It does not say "the only bridge." Ishizue should support
multiple bridge instances per server from day one, because the
cost is low and the use case (per-app sandboxing) is real. The
allowlist (SPEC §6.3) already keys off binary path, so multiple
instances of the same bridge binary work without changes.

What Ishizue should not do is try to invent an X11 session
manager on top: each bridge instance owns its own X display number,
its own `/tmp/.X11-unix/X<n>` socket, and its own X11 client set.
Inter-bridge X11 communication (one X11 app talking to another X11
app on a different bridge) is out of scope and should be left to
the Architect if it ever matters.

## 12. Known limitations and pain points

A non-exhaustive list of things Xwayland gets wrong or that bug
users, with implications for Ishizue:

- **Stale GL contexts on VT switch.** Xwayland does not own DRM
  master (the Wayland compositor does), so it depends on the
  compositor's render node access. On VT switch away and back, GL
  contexts inside Xwayland can go stale if the compositor does not
  restore them correctly. SPEC §3 already mandates DRM master
  handling with `drmSetMaster`/`drmDropMaster` on session
  active/inactive events, which is the compositor's responsibility
  in Wayland; the Ishizue bridge just uses the render node and
  never holds master, so it inherits this behavior.

- **Fullscreen quirks.** X11 fullscreen apps expect to be the only
  thing on screen and to switch the display mode. Under Xwayland,
  the compositor decides what fullscreen means (typically a
  `xdg_toplevel.set_fullscreen` that the XWM translates from the
  X11 `_NET_WM_STATE_FULLSCREEN` property). Mode switching is
  emulated via `wp_viewport` scaling, which can introduce latency
  or scaling artifacts. The man page's `-force-xrandr-emulation`
  flag [2] is a workaround. Ishizue should expose RANDR emulation
  through the existing output model (SPEC §6.5) and let the
  Architect decide policy.

- **XInput2 translation losses.** Xwayland translates Wayland
  `wl_touch` and `zwp_tablet_v2` events to XInput2 events, but
  some XInput2 features (e.g. per-device modulation, raw events
  with full state, multitouch gesture translation) do not map
  cleanly. Touch pointer emulation (`-noTouchPointerEmulation`
  flag [2]) is one escape hatch. SPEC §9 should explicitly define
  what the bridge translates and what it does not, so the
  Architect can fill gaps with its own input policy.

- **Pointer barriers and edge translation.** X11 has
  `XFixesCreatePointerBarrier` for edge barriers; Wayland has
  `zwp_pointer_constraints_v1`. Cross-monitor edge translation in
  X11 (the pointer warps from one screen edge to the next) is
  fundamentally at odds with Wayland's per-surface coordinate
  model. This is a long-standing source of bugs in mutter and KWin.
  Ishizue's per-output coordinate model (SPEC §6.5) has the same
  issue and the bridge should declare pointer barriers unsupported
  rather than implement them partially.

- **Idle-inhibit weirdness.** The X11 screensaver protocol
  (`XScreenSaverSuspend`) does not work under rootless Xwayland;
  xwl_screen_init disables the screensaver extension entirely in
  rootless mode [5]. Wayland has `zwp_idle_inhibit_manager_v1`
  instead. The bridge should translate X11
  `XScreenSaverSuspend(True)` to `isz_surface_set_idle_inhibit`
  (or whatever Ishizue exposes; SPEC does not currently define
  this) on the focused surface.

- **VRR passthrough.** Variable refresh rate under Xwayland has
  historically been broken [26] because the compositor controls
  the display mode and the X11 client cannot directly request a
  mode change. The Ishizue bridge should pass through
  `_NET_WM_BYPASS_COMPOSITOR` and similar hints as surface
  properties on the Ishizue side and let the Architect apply
  VRR policy.

- **Resize glitches.** The `_NET_WM_SYNC_REQUEST` issue described
  in section 9 is a real, user-visible bug that KWin fixed in 6.3
  [17]. Ishizue's design should tie configure acknowledgment to
  frame presentation from the start, so the same bug does not have
  to be fixed later.

## 13. What Ishizue should copy and what it should do differently

Copy:

- **The process model.** A separate process, spawned by the
  Architect, connected via a Unix socket, listening on
  `/tmp/.X11-unix/X<n>`. This is already what SPEC §13 mandates and
  what the existing `x11bridge/` scaffold does.
- **The fd-based readiness mechanism.** The Ishizue bridge should
  write its display number to a readiness fd handed to it by the
  Architect, equivalent to Xwayland's `-displayfd`. The existing
  scaffold uses an env var (`ISZ_X11_DISPLAY`); this is OK for v1
  but should migrate to an fd for parity with how Xwayland does it
  and to avoid the race where the Architect reads the env var
  before the bridge has actually bound the socket.
- **Damage as a list of rects, with a 256-rect fallback to the
  bounding box.** This is exactly what SPEC §7.9 calls for, and
  Xwayland's empirical threshold is a good default.
- **Buffer-coordinate damage** (the `wl_surface_damage_buffer`
  path) when the surface is scaled. SPEC §7.9 specifies
  surface-local coordinates; the bridge should expose both or
  document which one it uses, because HiDPI X11 apps with
  `wp_viewport` scaling will get the wrong damage otherwise.
- **The glamor path.** Render X11 client drawing into a `gbm_bo`,
  export as dma-buf, attach via `isz_surface_attach_buffer`. This
  is the same flow Xwayland uses and the SPEC §7.1 API is already
  shaped for it.
- **Rootless by default.** Desktop compositors all do this. Add
  rootful later for sandbox use cases.
- **The XWM-in-the-bridge pattern.** Since the Ishizue bridge is
  both the X server and the XWM, the bridge must have an internal
  XWM module that drives X11 focus, stacking, and configure state
  in response to Ishizue events. This is unavoidable.

Do differently:

- **Skip xwayland-shell-v1.** Ishizue does not need a wire protocol
  for surface <-> window association because the bridge and the
  XWM are the same process. Keep an in-memory map. Expose the X11
  window id as a surface property for the Architect's use, but do
  not invent a protocol for it.
- **Define primary selection in SPEC.** SPEC §6.8 only defines the
  clipboard. X11 has both PRIMARY and CLIPBOARD, and bridging both
  requires either two APIs or a parameterized one. Add a primary-
  selection API (mirroring `zwp_primary_selection_v1`) before the
  bridge gets to the selection work.
- **Tie configure acknowledgment to frame presentation.** Do not
  repeat the `_NET_WM_SYNC_REQUEST` mistake. The bridge should
  send a ConfigureNotify to the X11 client, wait for the next
  `isz_surface_attach_buffer` with the new size, and only then
  tell the Architect the resize is complete.
- **Design for multiple bridge instances from day one.** The
  allowlist already supports it. Document the multi-instance
  pattern in the SPEC so the Architect can spawn one bridge per
  sandboxed app if it wants to.
- **Do not implement pointer barriers, raw XInput2 multitouch, or
  mode switching.** Declare them unsupported in v1. They are
  long-term bug sources in Xwayland and Ishizue does not need
  them for the v1 target hardware class (SPEC §3, §14).
- **Single source of truth for "surface under the pointer."**
  Drive both X11 `MotionNotify` routing and Wayland-style
  `wl_pointer.enter` from the same Ishizue pointer event. This
  avoids the cross-protocol DnD bugs described in section 8.

## References

- [1] https://wayland.freedesktop.org/docs/book/Xwayland.html - Wayland Book, "X11 Application Support" chapter
- [2] https://man.archlinux.org/man/Xwayland.1.en - Xwayland(1) man page (Arch Linux)
- [3] https://wayland.app/protocols/xwayland-shell-v1 - xwayland-shell-v1 Wayland protocol (Wayland Explorer)
- [4] https://github.com/mirror/xserver/blob/master/hw/xwayland/xwayland.c - xwayland.c: `ddxProcessArgument`, `ddxUseMsg`, `InitOutput`, `wm_selection_callback`
- [5] https://github.com/mirror/xserver/blob/master/hw/xwayland/xwayland-screen.c - xwayland-screen.c: `xwl_screen_init`, `xwl_surface_damage`
- [6] https://github.com/mirror/xserver/blob/master/hw/xwayland/xwayland-window.c - xwayland-window.c: `xwl_window_post_damage`, `xwl_window_set_allow_commits`
- [7] https://github.com/mirror/xserver/blob/master/hw/xwayland/xwayland-glamor.c - xwayland-glamor.c: `xwl_glamor_pixmap_get_wl_buffer`, `xwl_dmabuf_feedback_*` listeners
- [8] https://github.com/mirror/xserver/blob/master/hw/xwayland/xwayland-input.c - xwayland-input.c: `keyboard_handle_enter`/`keyboard_handle_leave`, `xwl_seat` focus state
- [9] https://github.com/mirror/xserver/blob/master/os/connection.c - os/connection.c: `NotifyParentProcess` (writes display number to `-displayfd`, raises SIGUSR1, sd_notify)
- [10] https://github.com/mirror/xserver/blob/master/dix/main.c - dix/main.c: caller of `NotifyParentProcess` after `CreateConnectionBlock`
- [11] https://wlroots.pages.freedesktop.org/wlroots/wlr/xwayland/xwayland.h.html - wlroots `wlr_xwayland`, `wlr_xwayland_server`, `wlr_xwayland_icccm_input_model` API docs
- [12] https://en.wikipedia.org/wiki/Wayland_(protocol) - Wikipedia: Wayland protocol history (Kristian Høgsberg, 2008)
- [13] https://emersion.fr/blog/2019/intro-to-damage-tracking - emersion, "Introduction to damage tracking"
- [14] https://emersion.fr/blog/2020/wayland-clipboard-drag-and-drop - emersion, "Wayland clipboard and drag & drop"
- [15] https://xeechou.net/posts/xwayland-selection - xeechou, "Xwayland Clipboard" (ICCCM selection bridging walkthrough)
- [16] https://blog.martin-graesslin.com/blog/2016/07/synchronizing-the-x11-and-wayland-clipboard - Martin Gräßlin (Flöser), "Synchronizing the X11 and Wayland clipboard"
- [17] https://blog.vladzahorodnii.com/2024/10/28/improving-xwayland-window-resizing - Vlad Zahorodnii, "Improving Xwayland window resizing" (`_NET_WM_SYNC_REQUEST` under Xwayland)
- [18] https://lists.x.org/archives/xorg-devel/2018-March/056229.html - Lyude Paul, "[PATCH xserver 1/3] xwayland: Decouple GBM from glamor" (xorg-devel, Mar 2018)
- [19] https://lists.freedesktop.org/archives/systemd-commits/2015-March/008224.html - systemd v219-stable changelog (Xwayland readiness hooks reference)
- [20] https://wayland.app/protocols/primary-selection-unstable-v1 - `zwp_primary_selection_v1` Wayland protocol
- [21] https://www.phoronix.com/news/XWayland-Rootful-Useful - Phoronix, "XWayland's Rootful Mode Is Becoming More Useful"
- [22] https://github.com/YaLTeR/niri/discussions/2566 - niri discussion, "Drag-and-Drop Between Wayland and XWayland Applications"
- [23] https://bugs.openjdk.org/browse/JDK-8280994 - OpenJDK, "[XWayland] Drag and Drop does not work in java -> wayland app direction"
- [24] https://invent.kde.org/plasma/kwayland-server/-/merge_requests/106 - KDE kwayland-server MR, "Fix crash on drag and drop from xwayland to wayland clients"
- [25] https://sloonz.github.io/posts/sandboxing-2 - sloonz, "Sandboxing Applications with Bubblewrap: Desktop Applications"
- [26] https://github.com/swaywm/sway/issues/7370 - swaywm/sway#7370, "Adaptive sync / variable refresh broken when fullscreen"
