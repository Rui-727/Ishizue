# tinywm and tinywl architecture, and what tinyisz should steal

This document reads tinywm and tinywl source line by line, then compares them
with tinyisz. The goal is concrete: list patterns tinyisz should copy, patterns
it should reject, and features it is missing.

tinyisz is the in-tree minimal WM that drives the Ishizue library. It already
tiles, handles keybindings, and spawns the X11 bridge. tinywm is the smallest
useful X11 WM. tinywl is the smallest useful wlroots compositor. Both are
reference points for what "minimal" means in this problem space.

## tinywm: overview

tinywm is a window manager written by Nick Welch in 2005, with a refresh in
2011 [1][3]. The C source is 51 lines including the license header and blank
lines [1]. A Python port exists in the same repository at 36 lines, which is
useful as a second view of the same algorithm [1]. The README on GitHub
describes it as "ridiculously tiny" and "implemented in nearly as few lines of
C as possible, without being obfuscated or entirely useless" [2]. The Hacker
News thread from 2018 calls it "a minimal template to follow if one wants to
start writing a window manager in one's language of choice" [4].

The whole C file fits in one screen. Reproduced here in full because every
later section refers back to it [1]:

```c
/* TinyWM is written by Nick Welch <nick@incise.org> in 2005 & 2011.
 *
 * This software is in the public domain
 * and is provided AS IS, with NO WARRANTY. */

#include <X11/Xlib.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

int main(void)
{
    Display * dpy;
    XWindowAttributes attr;
    XButtonEvent start;
    XEvent ev;

    if(!(dpy = XOpenDisplay(0x0))) return 1;

    XGrabKey(dpy, XKeysymToKeycode(dpy, XStringToKeysym("F1")), Mod1Mask,
            DefaultRootWindow(dpy), True, GrabModeAsync, GrabModeAsync);
    XGrabButton(dpy, 1, Mod1Mask, DefaultRootWindow(dpy), True,
            ButtonPressMask|ButtonReleaseMask|PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, 3, Mod1Mask, DefaultRootWindow(dpy), True,
            ButtonPressMask|ButtonReleaseMask|PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);

    start.subwindow = None;
    for(;;)
    {
        XNextEvent(dpy, &ev);
        if(ev.type == KeyPress && ev.xkey.subwindow != None)
            XRaiseWindow(dpy, ev.xkey.subwindow);
        else if(ev.type == ButtonPress && ev.xbutton.subwindow != None)
        {
            XGetWindowAttributes(dpy, ev.xbutton.subwindow, &attr);
            start = ev.xbutton;
        }
        else if(ev.type == MotionNotify && start.subwindow != None)
        {
            int xdiff = ev.xbutton.x_root - start.x_root;
            int ydiff = ev.xbutton.y_root - start.y_root;
            XMoveResizeWindow(dpy, start.subwindow,
                attr.x + (start.button==1 ? xdiff : 0),
                attr.y + (start.button==1 ? ydiff : 0),
                MAX(1, attr.width + (start.button==3 ? xdiff : 0)),
                MAX(1, attr.height + (start.button==3 ? ydiff : 0)));
        }
        else if(ev.type == ButtonRelease)
            start.subwindow = None;
    }
}
```

Section by section:

- Lines 1 to 4: public-domain dedication. No license boilerplate, no warranty.
- Line 6: single include. Xlib pulls in everything tinywm needs.
- Line 8: a `MAX` macro. The only piece of "library" code in the file.
- Lines 10 to 15: locals. `dpy` is the X connection. `attr` holds the
  pre-drag geometry of the window being moved or resized. `start` is a
  `XButtonEvent` reused as the drag origin marker. `ev` is the current event.
- Line 17: open the default display, exit on failure. There is no error
  message; the return code 1 is the only signal.
- Lines 19 to 24: grab one key (Alt+F1) and two buttons (Alt+Button1 and
  Alt+Button3) on the root window. These grabs are the entire keybinding
  and shortcut system.
- Line 26: initialize `start.subwindow` to `None`. This field is the flag
  that says "a drag is in progress".
- Lines 27 to 49: the event loop. `XNextEvent` blocks. The body dispatches
  on `ev.type` to one of four cases.

Initialization is the 6-line block from line 17 to line 26. It opens the
display, registers three passive grabs on the root window, and primes the
drag-state variable. There is no EWMH init, no reparenting, no cursor
change, no error handler, no atexit cleanup. The X server's defaults take
care of focus (sloppy focus is the X default) and window decoration (none
is added).

The window manager does not create any windows of its own. It does not
reparent client windows into frames. It does not draw borders. It does
not set input focus. It only reacts to the three grabbed inputs.

## tinywm: event loop

The loop is `for (;;) { XNextEvent(dpy, &ev); ... }` [1]. `XNextEvent`
blocks until the X server queues an event the client has selected for.
tinywm selects for events implicitly through the passive grabs: a grabbed
key or button generates `KeyPress`/`ButtonPress` events delivered to the
grabbing client, and the `PointerMotionMask` argument in `XGrabButton`
asks the server to also deliver `MotionNotify` events while the grab is
active [1].

Four event types are handled:

- `KeyPress` with a non-`None` `subwindow`. This is Alt+F1 over a client
  window. Action: `XRaiseWindow` on the window under the pointer [1].
  The `subwindow` field of `XKeyEvent` is the child window of the root
  that the pointer was over when the key went down, supplied by the X
  server as part of the event. tinywm does not need to do a
  `XQueryPointer`.
- `ButtonPress` with a non-`None` `subwindow`. Alt+Button1 or
  Alt+Button3 over a client window. Action: cache the window's current
  geometry in `attr` and copy the event into `start` [1]. The drag is
  now armed.
- `MotionNotify` while `start.subwindow` is not `None`. Action: compute
  `xdiff` and `ydiff` against the press point, then call
  `XMoveResizeWindow` with a new geometry derived from `attr` plus the
  deltas [1]. Button 1 maps to position deltas, button 3 to size deltas.
  The `MAX(1, ...)` guards keep width and height above zero.
- `ButtonRelease`. Action: clear `start.subwindow` to `None` [1]. The
  drag is disarmed.

Other events go to the implicit `else` and are dropped. There is no
`MappingNotify` handler to refresh grabs when the keymap changes, no
`UnmapNotify` handler to forget windows, no `MapRequest` handler to
decide whether to allow a window on screen. tinywm lets the X server
handle every other event according to its defaults.

The `MotionNotify` path deserves a closer look because it shows how
tinywm gets away with no drag abstraction:

```c
else if(ev.type == MotionNotify && start.subwindow != None)
{
    int xdiff = ev.xbutton.x_root - start.x_root;
    int ydiff = ev.xbutton.y_root - start.y_root;
    XMoveResizeWindow(dpy, start.subwindow,
        attr.x + (start.button==1 ? xdiff : 0),
        attr.y + (start.button==1 ? ydiff : 0),
        MAX(1, attr.width + (start.button==3 ? xdiff : 0)),
        MAX(1, attr.height + (start.button==3 ? ydiff : 0)));
}
```

Every motion event triggers a synchronous `XMoveResizeWindow` round
trip. tinywm does not compress motion events, does not use
`XSendEvent` to coalesce, and does not batch. On a slow X connection
this would lag; on a local socket it is fast enough. The simplicity
of the approach is the lesson: a single `XMoveResizeWindow` call per
motion event is sufficient for the dragging feel users expect.

The cast of `ev.xbutton` inside a `MotionNotify` branch is legal
because `XButtonEvent` and `XMotionEvent` are layout-compatible
prefixes of `XAnyEvent` and share the `x_root`/`y_root`/`state`/`subwindow`
fields at the same offsets. tinywm relies on that ABI quirk to avoid a
union access through `ev.xmotion`.

## tinywm: window management

tinywm tracks zero windows. There is no window list, no focus stack, no
z-order list. The state of the WM is exactly three local variables in
`main`: `dpy`, `attr`, `start` [1]. Of those, only `start.subwindow` is
meaningful between events, and it is only meaningful between a button
press and the matching release.

Windows are identified by X window IDs that arrive inside events.
`ev.xkey.subwindow` and `ev.xbutton.subwindow` are the only window
references tinywm ever holds. When an event arrives, tinywm asks the X
server for the current geometry with `XGetWindowAttributes`, uses it for
the duration of the drag, and discards it on release. The X server is
the system of record.

Focus is delegated to the X server entirely. tinywm never calls
`XSetInputFocus`. The default focus policy on most X servers is "sloppy
focus", where focus follows the pointer between windows but sticks when
the pointer leaves the root window. tinywm inherits whatever the user
has configured.

Raising is done with `XRaiseWindow` on Alt+F1 [1]. `XRaiseWindow` is
the Xlib wrapper around a `ConfigureWindow` request with
`stack_mode = Above`. There is no restack logic, no transient handling,
no focus-then-raise pairing. Alt+F1 raises the window currently under
the pointer; that is the entire stacking policy.

tinywm is purely floating. There is no tiling, no maximization, no
snap, no fullscreen. Windows keep whatever size and position the client
requested, modified only by Alt+drag. This is the simplest possible
window management policy: clients place themselves, the WM only moves
them on user request.

The lesson here is that a window manager does not need a window list to
be useful. The X server already maintains the global window tree, and
for a floating WM that tree is sufficient state. A tiling WM cannot
get away with this because it has to recompute geometry for every
window when one is added or removed, but tinywm shows that the line
between "window manager" and "X client that handles three shortcuts"
is very thin.

## tinywm: keybindings

tinywm has exactly three bindings [1]:

| Input | Action |
|---|---|
| Alt+F1 with pointer over a window | `XRaiseWindow` on that window |
| Alt+Button1 press over a window | Begin move drag |
| Alt+Button3 press over a window | Begin resize drag |

The modifier is `Mod1Mask`, which is Alt on a typical X server. There
is no Super binding, no Shift binding, no chord. The key is `F1`
converted from a keysym to a keycode via `XKeysymToKeycode(dpy,
XStringToKeysym("F1"))` [1]. The indirection through `XStringToKeysym`
is the only piece of keymap-aware code in the file.

Bindings are installed with `XGrabKey` and `XGrabButton`. These functions
register "passive grabs" on the root window. A passive grab tells the
X server: when this key or button goes down with this modifier state,
freeze further event delivery and route events to the grabbing client
until the grab is released [1]. The arguments to both calls are
identical in pattern:

- The display and the keycode or button number.
- The modifier mask.
- The grab window (the root).
- `owner_events = True`, which means the grab still delivers events to
  the window the pointer happens to be over (so `subwindow` is
  populated).
- `pointer_mode` and `keyboard_mode` of `GrabModeAsync`, meaning the X
  server does not freeze other clients while the grab is active.
- For buttons, an event mask of `ButtonPressMask|ButtonReleaseMask|
  PointerMotionMask` so the grabber receives motion events too.
- `confine_to = None` and `cursor = None`, so the cursor is free to
  move anywhere and uses the default cursor image.

The mapping from press to action lives in the event loop, not in a
table. `KeyPress` is one branch, `ButtonPress` is another, and the
`start.button` field at drag time selects between move and resize
inside the `MotionNotify` branch [1]. There is no keybinding
abstraction, no `Binding` struct, no lookup function. Adding a fourth
binding would mean adding another `XGrabKey`/`XGrabButton` call and
another `else if` branch.

The Python port makes this even more visible [1]:

```python
dpy.screen().root.grab_key(dpy.keysym_to_keycode(XK.string_to_keysym("F1")), X.Mod1Mask, 1,
        X.GrabModeAsync, X.GrabModeAsync)
dpy.screen().root.grab_button(1, X.Mod1Mask, 1, X.ButtonPressMask|X.ButtonReleaseMask|X.PointerMotionMask,
        X.GrabModeAsync, X.GrabModeAsync, X.NONE, X.NONE)
dpy.screen().root.grab_button(3, X.Mod1Mask, 1, X.ButtonPressMask|X.ButtonReleaseMask|X.PointerMotionMask,
        X.GrabModeAsync, X.GrabModeAsync, X.NONE, X.NONE)
```

Same calls, same arguments, same three bindings. The Python version
uses `configure(stack_mode=X.Above)` for raise and `configure(x=, y=,
width=, height=)` for move/resize, which are the Xlib requests under
`XRaiseWindow` and `XMoveResizeWindow`.

## tinywl: overview

tinywl is a Wayland compositor shipped inside the wlroots repository, in
the `tinywl/` subdirectory [6][7][8]. The README opens with: "This is
the 'minimum viable product' Wayland compositor based on wlroots. It
aims to implement a Wayland compositor in the fewest lines of code
possible, while still supporting a reasonable set of features" [7]. The
README then says "Reading this code is the best starting point for
anyone looking to build their own Wayland compositor based on wlroots"
[7].

The C source is 972 lines, including comments and blank lines [6]. That
is roughly 19 times the size of tinywm. The size difference is not
padding; it reflects how much more work a Wayland compositor has to do
than an X11 WM. The X server handles input routing, focus policy,
window tree maintenance, and damage tracking. A Wayland compositor has
to do all of that itself, with wlroots providing the primitives.

What tinywl demonstrates, from the README and source [6][7]:

- A single-output, single-seat Wayland session.
- xdg-shell support for application windows (toplevels and popups).
- Keyboard and pointer input through `wlr_seat` and `wlr_cursor`.
- Interactive move and resize triggered by client-side decorations.
- Fullscreen rendering at the output refresh rate, no damage tracking.
- Two keybindings: Alt+Escape to quit, Alt+F1 to cycle focus.
- A `-s` startup command, run via `/bin/sh -c`.

What tinywl omits, also from the README [7]:

- HiDPI support (only partial; the scale factor is applied in
  `render_surface` but the cursor manager is loaded at scale 1 only).
- Configuration of any kind, including output layout.
- Any protocol other than xdg-shell. No layer-shell, no Xwayland, no
  screencopy, no primary selection.
- Damage tracking. The whole output is repainted every frame.

The README points at the wlroots wiki for damage tracking and other
features [7][12]. The blog posts referenced in tinywl.c itself are
Drew DeVault's "Input handling in wlroots" and "Wayland shells" articles
[9][10].

How tinywl differs from sway: sway is a full tiling WM with i3-style
keybindings, configuration files, multi-output layouts, Xwayland,
layer-shell, screencopy, IPC, and damage tracking. tinywl is none of
that. tinywl is a floating WM with two shortcuts, no config, no damage
tracking, and only xdg-shell. sway reads tinywl as a tutorial; tinywl
reads sway as a feature checklist. A common community recommendation
for new wlroots compositors is to fork tinywl and grow from there
[11].

## tinywl: initialization

Initialization runs in `main` from line 814 to line 949 [6]. The
sequence, in order:

1. `wlr_log_init(WLR_DEBUG, NULL)` so wlroots internal messages reach
   stderr [6].
2. Parse `-s` with `getopt`. The startup command is stored in
   `startup_cmd` [6].
3. `server.wl_display = wl_display_create()` [6]. This is the
   libwayland-server entry point. It allocates the display state, the
   event loop, and the global registry, but does not yet open a socket.
4. `server.backend = wlr_backend_autocreate(server.wl_display)` [6].
   `wlr_backend_autocreate` picks a backend based on environment. If
   `WAYLAND_DISPLAY` or `DISPLAY` is set, it creates a Wayland or X11
   backend that renders into a window of the parent session. If
   neither is set, it creates a libseat + DRM backend and opens the
   first available DRM device. The point is that tinywl does not have
   to know which one it will get.
5. `server.renderer = wlr_backend_get_renderer(server.backend)` [6].
   The renderer is created by the backend during `autocreate` and
   exposed here. tinywl does not call `wlr_gles2_renderer_create`
   directly; it accepts whatever the backend provides. For DRM this is
   GLES2 over the GPU named by the DRM device. For the Wayland and
   X11 backends it is GLES2 over the parent's GL stack.
6. `wlr_renderer_init_wl_display(server.renderer, server.wl_display)`
   [6]. This publishes the `wl_shm` global and the renderer's
   supported buffer formats so clients know what they can submit.
7. `wlr_compositor_create(server.wl_display, server.renderer)` [6].
   Publishes the `wl_compositor` global so clients can create
   `wl_surface` objects. This is the step that makes tinywl a real
   Wayland server rather than just a libwayland program.
8. `wlr_data_device_manager_create(server.wl_display)` [6]. Publishes
   `wl_data_device_manager`, which is the clipboard protocol.
9. `server.output_layout = wlr_output_layout_create()` [6]. The
   output layout is wlroots' 2D coordinate system for placing outputs.
   tinywl uses `add_auto` later so each new output goes to the right
   of the previous one.
10. `wl_list_init(&server.outputs)` and register the `new_output`
    listener on `server.backend->events.new_output` [6]. When the
    backend enumerates a display, `server_new_output` runs.
11. `wl_list_init(&server.views)` and create the xdg-shell with
    `wlr_xdg_shell_create(server.wl_display)`. Register
    `server_new_xdg_surface` on `xdg_shell->events.new_surface` [6].
    This is the equivalent of tinywm's passive grabs: it tells wlroots
    "tell me whenever a client creates a toplevel".
12. `wlr_cursor_create()`, attach the output layout, create the
    xcursor manager at scale 1 with size 24 [6]. The cursor is a
    logical pointer that wlroots moves across the output layout.
    Input devices attach to it and emit aggregated motion events.
13. Wire five cursor signal listeners: motion, motion_absolute,
    button, axis, frame [6].
14. `wl_list_init(&server.keyboards)` and register `server_new_input`
    on `backend->events.new_input` [6]. This catches keyboards and
    pointers as they appear.
15. `wlr_seat_create(server.wl_display, "seat0")` [6]. The seat is
    the Wayland abstraction for a user position. tinywl has one seat
    named `seat0`.
16. Register the seat's `request_set_cursor` and
    `request_set_selection` listeners [6]. Clients ask the seat to
    change the cursor image or the clipboard; tinywl approves all
    cursor requests from the focused client and all selection
    requests unconditionally.
17. `wl_display_add_socket_auto(server.wl_display)` [6]. Opens a
    `wayland-0` (or next free) Unix socket and publishes it. tinywl
    holds the socket name to set `WAYLAND_DISPLAY` for the startup
    command later.
18. `wlr_backend_start(server.backend)` [6]. This is the point of no
    return. The backend takes over the DRM master, enumerates
    outputs, opens evdev devices through libinput, and starts the
    frame clock. If this fails, tinywl tears down and exits.
19. `setenv("WAYLAND_DISPLAY", socket, true)` and fork the startup
    command via `execl("/bin/sh", "/bin/sh", "-c", startup_cmd,
    NULL)` if one was supplied [6].
20. `wl_display_run(server.wl_display)` [6]. Enters the event loop.
    See the next section.

There is no explicit "open the DRM device" step in tinywl. The DRM fd
is opened by the backend during `wlr_backend_autocreate` if no parent
session is detected, and the libseat integration inside wlroots handles
DRM master acquisition. tinywl never sees the fd. The lesson is that
the backend abstraction hides the platform; a new compositor does not
need DRM code.

## tinywl: event loop

The loop is a single call: `wl_display_run(server.wl_display)` [6].
`wl_display_run` is libwayland's blocking dispatch loop. It calls
`wl_event_loop_dispatch` on the display's internal `wl_event_loop`,
which `poll(2)`s the registered fds and fires their callbacks. The
loop returns when something calls `wl_display_terminate`, which
tinywl does in `handle_keybinding` on Alt+Escape [6].

The fds in the loop, by source:

- The Wayland socket from `wl_display_add_socket_auto`. Reads on this
  fd produce client connection events and protocol messages.
- The libinput fd from the backend. Reads produce input device events.
- The DRM fd, polled for vblank and page-flip completion.
- Any timer fds wlroots installs, for example the keyboard repeat
  timer.

None of these are dispatched manually by tinywl. They are dispatched
inside `wl_display_run`. tinywl's job is to register callbacks
(`wl_signal_add` in wlroots terms) before the loop starts so that
libwayland and wlroots will call into tinywl at the right moments.

The callback pattern is uniform. Every signal gets a `wl_listener`
with a `.notify` function pointer, registered with `wl_signal_add`.
The notify function takes `(struct wl_listener *, void *data)`,
recovers the tinywl object with `wl_container_of`, and runs the
handler. Example from the keyboard path [6]:

```c
static void keyboard_handle_key(
                struct wl_listener *listener, void *data) {
    struct tinywl_keyboard *keyboard =
            wl_container_of(listener, keyboard, key);
    struct tinywl_server *server = keyboard->server;
    struct wlr_event_keyboard_key *event = data;
    struct wlr_seat *seat = server->seat;

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(
                    keyboard->device->keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);
    if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            for (int i = 0; i < nsyms; i++) {
                    handled = handle_keybinding(server, syms[i]);
            }
    }

    if (!handled) {
            wlr_seat_set_keyboard(seat, keyboard->device);
            wlr_seat_keyboard_notify_key(seat, event->time_msec,
                    event->keycode, event->state);
    }
}
```

The pattern is: try the compositor bindings first. If a binding
matches, mark `handled = true` and the event is consumed. If nothing
matched, forward the event to the focused client via the seat. This
is the Wayland equivalent of tinywm's passive grabs, but implemented
in software: wlroots delivers every key event to the compositor, the
compositor decides who gets it.

Output frame callbacks work the same way. Each `tinywl_output` has a
`frame` listener registered on `wlr_output->events.frame` [6]. When
the DRM backend signals vblank, wlroots fires the frame signal and
`output_frame` runs. `output_frame` does the rendering and commits
the output, scheduling the next vblank wait. The loop is closed
inside wlroots; tinywl only reacts.

Wayland clients are handled by libwayland. When a client connects to
the socket, libwayland accepts it and adds the client fd to the
event loop. When the client sends a protocol message, libwayland
parses it, dispatches it to the resource's handler (which wlroots
installed when it created the global), and wlroots fires a signal
like `new_xdg_surface`. tinywl never reads the socket directly.

## tinywl: window management

tinywl tracks toplevels as a linked list of `struct tinywl_view`
[6]. The list lives at `server.views` and is initialized with
`wl_list_init`. Each view holds:

```c
struct tinywl_view {
    struct wl_list link;
    struct tinywl_server *server;
    struct wlr_xdg_surface *xdg_surface;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    bool mapped;
    int x, y;
};
```

The list is ordered front-to-back: the head is the most recently
focused view, the tail is the bottom of the stack [6]. This is the
stacking order, and it is also the search order for `desktop_view_at`.

A new toplevel arrives as a `new_surface` signal from the xdg-shell
[6]. `server_new_xdg_surface` allocates a `tinywl_view`, wires five
listeners (map, unmap, destroy, request_move, request_resize), and
prepends the view to `server.views`. At this point the view is
allocated but not yet visible; it has `mapped = false`.

`xdg_surface_map` runs when the client has committed its first
buffer [6]. It sets `mapped = true` and calls `focus_view` on the
new view. `xdg_surface_unmap` sets `mapped = false` and does not
remove the view from the list; the view sticks around in case the
client maps it again. `xdg_surface_destroy` runs when the
`wlr_xdg_surface` is gone for good, removes the view from the list,
and frees it.

The map/unmap split is a Wayland detail that tinywm never sees. In
X11, a window either exists or it does not. In Wayland, an
`xdg_surface` can be configured, mapped, unmapped, remapped, and
finally destroyed, all within the lifetime of one `wl_surface`.
tinywl mirrors that with a `mapped` flag that rendering and
focus ignore when false.

Focus is implemented in `focus_view` [6]:

```c
static void focus_view(struct tinywl_view *view, struct wlr_surface *surface) {
    if (view == NULL) {
            return;
    }
    struct tinywl_server *server = view->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface == surface) {
            return;
    }
    if (prev_surface) {
            struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(
                                seat->keyboard_state.focused_surface);
            wlr_xdg_toplevel_set_activated(previous, false);
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);
    wlr_xdg_toplevel_set_activated(view->xdg_surface, true);
    wlr_seat_keyboard_notify_enter(seat, view->xdg_surface->surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}
```

Three things happen on focus. First, the previously focused
toplevel is deactivated via `wlr_xdg_toplevel_set_activated(...,
false)`, which tells the client to drop its active appearance
(caret, active-state border, and so on). Second, the view is moved
to the head of `server.views`, which makes it the top of the
stacking order. Third, the seat is notified that keyboard focus
entered the new surface, which causes libwayland to send a
`wl_keyboard.enter` event to the new client and a `wl_keyboard.leave`
to the old one. The keycodes and modifiers are sent along so the
new client knows the current modifier state.

Cycle focus lives in `handle_keybinding` under `XKB_KEY_F1` [6].
The current view is the head of `server.views`. The next view is
`current_view->link.next`. `focus_view(next_view, ...)` moves
`next_view` to the head. Then the previous head is moved to the
tail with `wl_list_remove` + `wl_list_insert(server->views.prev,
...)`, which rotates the list by one. The result is a round-robin
focus cycle.

The view's `x, y` are tinywl-local layout coordinates, not
output-local. They are updated by `process_cursor_move` and
`process_cursor_resize` during interactive drags, and read by
`render_surface` to translate surface-local coordinates to
output-local coordinates for rendering [6]. tinywl does not
support a non-interactive move; the only way a view moves is
through a pointer drag initiated by `begin_interactive`.

`begin_interactive` is called from `xdg_toplevel_request_move`
and `xdg_toplevel_request_resize` [6]. These signals fire when a
client asks the compositor to start an interactive move or
resize, typically because the user dragged a client-side
decoration. tinywl checks that the requesting view is the one
with pointer focus, then sets `server->grabbed_view`,
`server->cursor_mode` to `TINYWL_CURSOR_MOVE` or
`TINYWL_CURSOR_RESIZE`, and caches the grab point. Subsequent
cursor motion events go to `process_cursor_move` or
`process_cursor_resize` until a button release resets the mode
to `TINYWL_CURSOR_PASSTHROUGH`.

## tinywl: rendering

Rendering is per-output, driven by the frame signal [6]. The
handler is `output_frame`:

```c
static void output_frame(struct wl_listener *listener, void *data) {
    struct tinywl_output *output =
            wl_container_of(listener, output, frame);
    struct wlr_renderer *renderer = output->server->renderer;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!wlr_output_attach_render(output->wlr_output, NULL)) {
            return;
    }
    int width, height;
    wlr_output_effective_resolution(output->wlr_output, &width, &height);
    wlr_renderer_begin(renderer, width, height);

    float color[4] = {0.3, 0.3, 0.3, 1.0};
    wlr_renderer_clear(renderer, color);

    struct tinywl_view *view;
    wl_list_for_each_reverse(view, &output->server->views, link) {
            if (!view->mapped) {
                    continue;
            }
            struct render_data rdata = {
                    .output = output->wlr_output,
                    .view = view,
                    .renderer = renderer,
                    .when = &now,
            };
            wlr_xdg_surface_for_each_surface(view->xdg_surface,
                            render_surface, &rdata);
    }

    wlr_output_render_software_cursors(output->wlr_output, NULL);

    wlr_renderer_end(renderer);
    wlr_output_commit(output->wlr_output);
}
```

The frame sequence is:

1. `wlr_output_attach_render` makes the renderer's GL context
   current on the output's back buffer.
2. `wlr_output_effective_resolution` returns the size after
   transform (a rotated output swaps width and height).
3. `wlr_renderer_begin` calls `glViewport` and sets up GL state.
4. `wlr_renderer_clear` paints a solid grey background.
5. Walk `server.views` in reverse (back-to-front) and call
   `wlr_xdg_surface_for_each_surface` on each mapped view. That
   wlroots helper invokes `render_surface` for the toplevel and
   every popup, passing the layout position down through
   `render_data`.
6. `wlr_output_render_software_cursors` paints the cursor if the
   hardware cursor plane is not available. On DRM with a working
   hardware cursor this is a no-op.
7. `wlr_renderer_end` flushes GL state.
8. `wlr_output_commit` queues a page flip. The next vblank will
   show the new frame and fire the next `frame` signal.

`render_surface` does the per-surface work [6]. It calls
`wlr_surface_get_texture` to get the GPU texture the client
committed, computes a box from the surface position plus the view
position plus the output-local translation from
`wlr_output_layout_output_coords`, scales by `output->scale` for
HiDPI, builds a 3x3 projection matrix with
`wlr_matrix_project_box`, and calls
`wlr_render_texture_with_matrix`. Then it calls
`wlr_surface_send_frame_done(surface, &now)` to tell the client
"the frame you committed has been shown, you may commit the next
one".

Damage tracking is absent. The README calls this out explicitly
[7]: "Damage tracking, which tracks which parts of the screen are
changing and minimizes redraws accordingly." tinywl repaints
every pixel of every output on every vblank. The cost is constant
GPU traffic at the refresh rate, regardless of whether anything
changed. sway and other production compositors use
`wlr_output_damage` to skip frames where nothing is dirty.

Cursor handling is split. `wlr_cursor` tracks the logical cursor
position across the output layout and emits motion signals. The
xcursor manager supplies the cursor image. The actual hardware
cursor on DRM is programmed by wlroots when
`wlr_cursor_set_surface` or `wlr_xcursor_manager_set_cursor_image`
is called. tinywl calls the latter in `process_cursor_motion` when
no view is under the pointer, to show the default `left_ptr`
cursor [6]. Client-supplied cursor surfaces are accepted in
`seat_request_cursor` after a check that the requesting client
holds pointer focus [6].

## tinywl: keybindings

tinywl has exactly two keybindings [6][7]:

| Input | Action |
|---|---|
| Alt+Escape | `wl_display_terminate`, quit the compositor |
| Alt+F1 | Cycle focus to the next view |

The modifier is `WLR_MODIFIER_ALT`, not Super. The task brief
mentioned Super+Return and Super+Escape, but the upstream source
uses Alt and has no spawn binding. The README confirms: "TinyWL
supports the following keybindings: Alt+Escape: Terminate the
compositor. Alt+F1: Cycle between windows" [7]. tinywl relies on
the `-s` flag at startup for spawning programs, since there is no
in-session spawn binding.

The binding dispatch is in `keyboard_handle_key` and
`handle_keybinding` [6]. The keyboard handler does this:

- Convert the libinput keycode to an xkb keycode with `+8`.
- Resolve the keycode to a list of keysyms through
  `xkb_state_key_get_syms` on the keyboard's xkb state.
- Read the current modifier mask with
  `wlr_keyboard_get_modifiers`.
- If Alt is held and the key was pressed, call
  `handle_keybinding` for each keysym. If any call returns true,
  the event is considered handled.
- If unhandled, forward the event to the focused client through
  `wlr_seat_keyboard_notify_key`.

`handle_keybinding` is a `switch` on the keysym [6]. Two cases.
`XKB_KEY_Escape` calls `wl_display_terminate(server->wl_display)`.
`XKB_KEY_F1` cycles focus as described in the previous section.
The default case returns `false` so the event falls through to
client delivery.

The keyboard setup in `server_new_keyboard` deserves a closer look
[6]:

```c
struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

wlr_keyboard_set_keymap(device->keyboard, keymap);
xkb_keymap_unref(keymap);
xkb_context_unref(context);
wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);
```

`xkb_keymap_new_from_names(context, NULL, ...)` builds a keymap
from the environment's defaults: layout "us", no variant, no
options. tinywl does not read XKB_* environment variables or a
config file. Repeat is hardcoded to 25 Hz with a 600 ms delay.
Every keyboard added to the seat gets the same keymap.

The Wayland model has no passive grabs. wlroots delivers every
key event to the compositor first. The compositor decides
whether to consume the event (a binding) or forward it to the
client. This is more flexible than X11's grab model but requires
the compositor to do its own modifier tracking. tinywl reads
the modifier mask out of the wlr_keyboard on every key press
rather than tracking state across events, which is simpler and
avoids bugs around modifier-state desync.

## Comparison: tinywm vs tinywl vs tinyisz

| Dimension | tinywm | tinywl | tinyisz |
|---|---|---|---|
| Lines of code (production .c) | 51 [1] | 972 [6] | 698 across 4 .c files [14] |
| Protocol | X11 | Wayland (xdg-shell) | Ishizue UDS protocol [13] |
| Tiling vs floating | Floating, no policy | Floating, interactive move/resize | Master/stack tiling [14] |
| Keybindings | Alt+F1 raise, Alt+Btn1 move, Alt+Btn3 resize [1] | Alt+Esc quit, Alt+F1 cycle [6][7] | Super+Enter spawn, Super+J/K cycle, Super+H/L adjust master, Super+1..9 focus, Super+Shift+Q close, Super+Shift+Esc quit [14] |
| Window tracking | None; X server is system of record | Linked list of `tinywl_view`, front-to-back stacking order [6] | Fixed array of 32 `tinyisz_window` with focused index [14] |
| Focus policy | Inherited from X server (sloppy by default) | Click-to-focus via `focus_view` on button press; keyboard focus through `wlr_seat` [6] | Click-to-focus via focus-follows-mouse in pointer motion handler; focus through `isz_seat_set_keyboard_focus` [14] |
| Rendering approach | Not rendered by the WM; X server composites | Full repaint every frame, no damage tracking [6][7] | Deferred to library + bridge; tinyisz sets zpos and commits [14] |
| Input handling | Passive grabs on root window [1] | wlroots signal listeners on seat and cursor [6] | Ishizue event listeners registered with `isz_add_listener` [14] |
| Error handling | Return 1 from main on `XOpenDisplay` failure; no error messages | `wlr_log_init(WLR_DEBUG, NULL)`, log messages on backend start failure, teardown on failure paths [6] | `isz_set_log_callback` sink forwarding to stderr, error strings on every failure path, explicit teardown order [14] |
| Config file | None | None (`-s` startup command only) | None; CLI flags only |
| Multi-output | Yes (root window covers all screens) | Yes (`wlr_output_layout`, `add_auto`) | Partial; first output only, multi-output tracked but unused for tiling [14] |
| Spawn mechanism | Not in WM (clients launched externally) | `fork` + `execl("/bin/sh", "-c", ...)` at startup, no in-session spawn | `fork` + `execvp("xterm", ...)` on Super+Enter, with `DISPLAY` set to the bridge's X11 socket [14] |
| Process model | Single process, the X server is separate | Single process, libwayland in-process | Single process plus a forked X11 bridge subprocess [14] |
| What it teaches | A WM does not need a window list or a focus policy to be useful; the X server can be the system of record | A compositor has to do everything itself: input routing, focus, stacking, rendering, damage. The library only provides primitives | A WM built on a non-Wayland custom protocol can match tinywl's structure (event listeners, view list, layout pass) while keeping tinywm-style minimalism in line count |

The line-count comparison needs context. tinywm is 51 lines because
the X server does the heavy lifting. tinywl is 972 lines because
Wayland compositors are expected to do everything except buffer
management and DRM modesetting. tinyisz is 698 lines because the
Ishizue library already owns buffer management, DRM, the wire
protocol, and the bridge process; tinyisz is pure policy.

## Lessons for tinyisz

### Lesson 1: tinyisz is already more capable than both references

tinyisz supports tiling, master/stack layout, focus cycling, master
ratio adjustment, index focus, focus-follows-mouse, in-session
spawn, and a clean shutdown sequence [14]. tinywm has none of that.
tinywl has none of the tiling. The minimalism of tinywm and tinywl
is a teaching aid, not a target. tinyisz should not regress in
features to match them; it should borrow their patterns.

### Lesson 2: copy tinywl's signal-listener pattern, not tinywm's passive grabs

tinywl registers one `wl_listener` per signal with a `.notify`
function pointer and uses `wl_container_of` to recover the
context [6]. tinyisz already follows this pattern with
`isz_add_listener` [14], so the structural match is good. The piece
tinyisz should copy is tinywl's discipline about listener
lifetimes: every `wl_signal_add` is paired with a `wl_list_remove`
in the destroy path. tinyisz's `tinyisz_ctx_destroy` does not
unregister listeners, it only destroys surfaces [14]. Because the
library outlives the context in some test paths, stale listeners
could fire. Add explicit unregister calls in `tinyisz_ctx_destroy`
for each listener that `tinyisz.c` registers.

### Lesson 3: borrow tinywl's focus_view pattern of deactivate-then-activate

tinywl's `focus_view` does three things on every focus change [6]:
deactivate the previous toplevel, restack the new one to the head,
and notify the seat of keyboard enter. tinyisz's
`tinyisz_wins_focus_index` only flips a `focused` bool [14]. The
deactivate step is the missing piece: when a window loses focus in
tinyisz, nothing tells the underlying client. For the current
bridge that is fine because the bridge does not surface focus state
to X11 clients, but it will matter when real Ishizue clients
arrive. The seat-enter pattern (sending the current modifiers and
keycodes to the newly focused client) is also missing in tinyisz
and should be added when the API exposes it.

### Lesson 4: tinyisz should adopt tinywl's mapping flag pattern

Wayland surfaces have a map/unmap lifecycle that is more granular
than create/destroy. tinywl models this with a `bool mapped` field
on each view and skips rendering and focus for unmapped views [6].
tinyisz currently treats client connect as "create a window" and
client disconnect as "destroy the focused window" [14], which is a
stand-in for a missing surface-create event. When the public API
gains a surface-create event, tinyisz should add a `mapped` flag
to `tinyisz_window` so a surface can exist in the list without
being rendered. This matches the tinywl pattern and avoids having
to remove and re-add views during transient unmap.

### Lesson 5: adopt tinywl's damage-tracking gap as a documented limitation, do not try to fix it yet

tinywl omits damage tracking and documents the omission in the
README [7]. tinyisz does not do damage tracking either, because
rendering is delegated to the bridge [14]. This is acceptable for
v1. The lesson is to make the omission explicit in the README,
the way tinywl does, so users do not file bugs expecting partial
repaint. tinyisz's README already lists limitations [14]; add a
line that says rendering is full-repaint per frame and damage
tracking will arrive when the library exposes damage regions.

### Lesson 6: tinywm's lesson is "do not over-engineer the window list"

tinywm has no window list at all [1]. tinywl has a linked list
with manual `wl_list_remove` and `wl_list_insert` calls [6].
tinyisz has a fixed array of 32 [14]. The fixed array is simpler
than the linked list and good enough for a tiling WM, but the
hardcoded `TINYISZ_MAX_WINDOWS = 32` will bite at some point. The
intermediate option is a dynamically grown array (realloc on add,
compact on remove), which keeps the index-based layout code
untouched. Do not switch to a linked list; the index is load-bearing
in `tinyisz_layout_apply` because the master is always index 0 [14].

### Lesson 7: copy tinywl's keybinding dispatch shape but keep tinyisz's richer table

tinywl's `handle_keybinding` is a `switch` on the keysym [6]. tinyisz's
`handle_key` is also a `switch` on the keycode, with a nested
`switch` for Shift-modified bindings [14]. The shape is already
right. The piece to copy from tinywl is the "try bindings first,
fall through to client delivery" pattern. tinyisz currently
consumes every Super-modified keypress and never forwards
non-binding keypresses to the client [14]. That works only because
the bridge is the only client and it consumes its input from the
X11 socket, not from tinyisz. When real Ishizue clients arrive,
`handle_key` needs an `if (!handled) isz_seat_keyboard_notify_key(...)`
branch like tinywl's.

### Lesson 8: do not copy tinywm's reliance on `subwindow` and event field aliasing

tinywm reads `ev.xbutton.x_root` from inside a `MotionNotify` branch
[1]. This works because `XButtonEvent` and `XMotionEvent` share a
layout-compatible prefix, but it is fragile and undocumented.
tinyisz uses typed accessors (`isz_event_get_keyboard_key`,
`isz_event_get_pointer_motion`) [14], which is the right call.
Stay with the typed accessor pattern.

### Lesson 9: copy tinywl's logging discipline

tinywl calls `wlr_log_init(WLR_DEBUG, NULL)` at startup [6] and
uses `wlr_log` throughout. tinyisz already wires
`isz_set_log_callback(tinyisz_log_fn, NULL)` [14] and forwards
library messages to stderr. The piece to add is using the log
callback from tinyisz's own code paths, not just the library's.
Right now tinyisz uses bare `fprintf(stderr, ...)` for its own
messages [14]. Routing those through the same log callback would
unify the output format and make log level filtering work
consistently.

### Lesson 10: tinyisz already has features both references lack

Document them. tinyisz has VT switch pause/resume through
`ISZ_EVENT_SESSION_ACTIVE`/`INACTIVE` [14]; tinywm and tinywl do
not handle VT switching at all (tinywl relies on wlroots' backend
to do it). tinyisz has a forked bridge subprocess with a clean
kill-and-reap sequence [14]; tinywl only forks the startup
command and does not reap it. tinyisz has master/stack tiling
with adjustable ratio [14]; neither reference tiles. These are
not gaps to fill, they are advantages to keep.

## References

[1] tinywm source, including tinywm.c and tinywm.py.
https://raw.githubusercontent.com/mackstann/tinywm/master/tinywm.c
and
https://raw.githubusercontent.com/mackstann/tinywm/master/tinywm.py

[2] mackstann/tinywm GitHub repository.
https://github.com/mackstann/tinywm

[3] tinywm homepage by Nick Welch.
http://incise.org/tinywm.html

[4] Hacker News discussion of tinywm, August 2018.
https://news.ycombinator.com/item?id=17765446

[5] Reddit r/C_Programming discussion of tinywm.
https://www.reddit.com/r/C_Programming/comments/d57rt0/tinywm_an_x11_window_manager_in_40_lines_of

[6] tinywl source, tinywl.c.
https://raw.githubusercontent.com/swaywm/wlroots/master/tinywl/tinywl.c

[7] tinywl README.
https://github.com/swaywm/wlroots/blob/master/tinywl/README.md

[8] tinywl directory in wlroots.
https://github.com/swaywm/wlroots/tree/master/tinywl

[9] Drew DeVault, "Input handling in wlroots" (referenced in
tinywl.c comments).
https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html

[10] Drew DeVault, "Wayland shells" (referenced in tinywl.c
comments).
https://drewdevault.com/2018/07/29/Wayland-shells.html

[11] inclem.net, "Thoughts on writing a wayland window manager
with wlroots".
https://inclem.net/2021/04/17/wayland/writing_a_wayland_compositor_with_wlroots/

[12] wlroots wiki, "Getting started".
https://github.com/swaywm/wlroots/wiki/Getting-started

[13] Ishizue specification, local file.
/home/z/my-project/repos/Ishizue/SPEC.md

[14] tinyisz source, local files.
/home/z/my-project/repos/Ishizue/tinyisz/tinyisz.c
/home/z/my-project/repos/Ishizue/tinyisz/input.c
/home/z/my-project/repos/Ishizue/tinyisz/window.c
/home/z/my-project/repos/Ishizue/tinyisz/layout.c
/home/z/my-project/repos/Ishizue/tinyisz/README.md
