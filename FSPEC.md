# FSPEC.md - Final scope (out-of-v1 deferrals)

Items the Ishizue X11 bridge and library will NOT implement in v1.
Each entry names the gap, why it's deferred, and what the user gets
instead. This file grows as the project discovers more limits.

## 1. X11 font database

The bridge ships no font metrics file and no XLFD (X Logical Font
Description) pattern matcher. QueryFont returns fixed 6x13 metrics
for every font name. ListFonts returns an empty list. Apps that
need real per-glyph metrics (xterm, xclock, xcalc's text labels)
get wrong sizes and may render text clipped or invisible.

Deferred because: a real font database is either (a) parse X11's
Xserver's fonts.dir / fonts.alias files plus libXfont's XLFD matcher,
or (b) ship a pre-built metrics table for the 5-10 most common
fixed fonts. Option (a) is a multi-thousand-line port. Option (b)
is doable but low-value while the bridge also lacks RENDER.

What v1 ships: plausible fixed metrics. Apps that only need
ascent/descent to size their top-level window work. Apps that draw
text glyph-by-glyph (xterm) do not.

## 2. RENDER extension

The bridge replies "not present" to QueryExtension for RENDER.
Modern X11 apps use RENDER for anti-aliased text (via libXft and
fontconfig), alpha compositing, gradients, and transformations.
Without RENDER, apps fall back to core X11 text rendering, which
needs the font database (see §1). The result: most GTK/Qt/Electron
apps render no text at all.

Deferred because: RENDER is a large extension (~50 requests, ~30
events, pict-formats, glyphsets, trapezoids). Implementing it is
roughly the same scope as the entire current bridge. A real
implementation also needs a real font path (fontconfig + freetype)
to be useful.

What v1 ships: RENDER absent. Apps that probe RENDER and fall back
to core protocol work; apps that hard-require RENDER abort.

## 3. DRI3 + Present extensions

No DRI3, no Present. Hardware-accelerated X11 clients (browsers,
GL apps, video players) cannot get dma-bufs to the compositor. All
rendering is software via PutImage.

Deferred because: DRI3 + Present is the fast path for any modern
X11 client. Implementing it requires the bridge to accept dma-buf
fds from X11 clients, wrap them in Ishizue surfaces via
isz_surface_attach_buffer, and sync presentation to vblank via
isz_commit with the presented event. The library side is ready
(W5-B wired drm_syncobj). The bridge side is a multi-hundred-line
addition that needs a real libdrm-equipped environment to test.

What v1 ships: software rendering only. PutImage works; clients
that need 60fps GL do not.

## 4. Wayland client compatibility

Per SPEC §2: explicitly out of scope. No wl_compositor, no
wl_shm, no xdg-shell. Wayland-native clients (weston-terminal,
foot, alacritty's Wayland backend) cannot connect.

Deferred because: SPEC §2 mandates this. A Wayland bridge would
be a separate process analogous to the X11 bridge, speaking
Wayland on one side and Ishizue's wire protocol on the other.
The library gains no Wayland code.

What v1 ships: no Wayland support. The X11 bridge covers the app
tail via Xwayland-style fallback.

## 5. Nested backend

ISZ_BACKEND_NESTED (SPEC §10) is a stub. Running Ishizue inside
an existing desktop session (X11 or Wayland) is not supported.

Deferred because: the nested backend needs a parent-window handle
and a way to forward Ishizue surfaces to the parent compositor as
native surfaces. On Wayland this means speaking wl_surface; on
X11 this means creating an X11 window and pushing dma-bufs via
DRI3. Both are non-trivial.

What v1 ships: headless and DRM backends only.

## 6. Full X11 ICCCM/EWMH compliance

The bridge implements enough ICCCM to let simple apps (xeyes,
xcalc) run. It does not implement: WM_STATE, WM_PROTOCOLS
handling (WM_DELETE_WINDOW, WM_TAKE_FOCUS, _NET_WM_PING),
_NET_SUPPORTED/_NET_WM_STATE/_NET_WM_DESKTOP EWMH, window
manager-side property initialization, reparenting with frame
windows, focus stealing prevention, or client-list tracking.

Deferred because: full ICCCM is a multi-hundred-page spec. The
bridge is a translator, not a window manager (per W7-A decision
doc). tinyisz owns WM policy; the bridge owns wire translation.

What v1 ships: enough for apps that don't depend on EWMH. Apps
that need _NET_WM_STATE to function (modern GTK/Qt) may misbehave.

## 7. XInput2

No XInput2 extension. Multitouch, tablet pressure, extra axes,
and raw motion events are unavailable. Pointer and keyboard work
via core X11 input only.

Deferred because: XInput2 is a large extension with its own
hierarchy of devices, classes, and event masks. Most desktop apps
don't need it; games and creative tools do.

What v1 ships: core pointer + core keyboard. One of each.

## 8. XKB extension

No XKB extension. Layout switching, modifier mapping beyond the
core 8 modifiers, and per-key remapping are unavailable. The
bridge uses the core keyboard mapping (GetKeyboardMapping) with
a minimal US layout.

Deferred because: XKB is its own state machine, plus a keymap
compiler (libxkbcommon) dependency. The library side has
libxkbcommon support (W1-E) but the bridge doesn't expose it to
X11 clients.

What v1 ships: US QWERTY layout, 8 core modifiers.

## 9. Network transparency

The bridge listens on a Unix domain socket only. No TCP listening.
X11 apps on remote machines cannot connect.

Deferred because: SPEC §6.1 mandates UDS. TCP would require
adding a listen port, handling auth (MIT-MAGIC-COOKIE-1), and
dealing with the security surface. Network transparency is a
legacy X11 feature that most modern deployments don't use.

What v1 ships: local-only.
