# X server implementations comparison

Research note for Ishizue's X11 bridge (SPEC §13). The bridge is a
from-scratch X11 server speaking raw X11 bytes over `/tmp/.X11-unix/X<n>`,
with no libX11 or libXCB dependency. This document surveys the existing
implementations so we can decide what to copy and what to leave behind.

Line counts cited below were taken directly from each project's source
tree on 2026-07-18. They are approximate, count `.c`/`.cpp`/`.h` lines
in the relevant subtree, and are meant for order-of-magnitude
comparison only.

## Xorg (the reference X server)

Xorg is the X.Org Foundation's implementation of the X Window System
core protocol, version 11 [1]. It descends from XFree86 (the
`hw/xfree86` directory name is a direct fossil of that lineage) and was
forked into the freedesktop.org tree after the XFree86 license change
of 2004. The git repository produces several distinct X servers from a
single source tree: `Xorg` (in `hw/xfree86`, the bare-metal server),
`Xwayland` (in `hw/xwayland`), `Xnest`, `Xephyr` (in `hw/kdrive/ephyr`),
`Xvfb`, `XQuartz` (macOS, in `hw/xquartz`), and `XWin` (Cygwin, in
`hw/xwin`) [8].

The architecture is split into two layers:

- **DIX (Device Independent X)** lives under `dix/`. It is the part
  that interacts with clients, runs the main loop, dispatches events,
  and does software rendering of the core graphics primitives. This is
  shared by every X server the tree produces. About 43k lines of C.
- **DDX (Device Dependent X)** lives one directory per server under
  `hw/`. Each directory is one DDX: a backend that talks to whatever
  "hardware" that server targets. `hw/xfree86` talks to real KMS
  hardware, `hw/xwayland` talks to a Wayland compositor, `hw/vfb`
  talks to an in-memory framebuffer, and so on [1] [8] [34].

The bare-metal DDX in `hw/xfree86` (~106k lines) historically owned
both mode-setting and 2D acceleration. Mode-setting moved into the
kernel DRM and is now exposed via the generic `modesetting` DDX, which
was mainlined in xorg-server 1.17 and is now the default for AMD,
Intel, and NVIDIA hardware on Xorg [1] [10]. The legacy per-vendor
`xf86-video-*` packages (`-amdgpu`, `-ati`, `-nouveau`, `-intel`) are
deprecated and the ArchWiki calls them "legacy" outright [10].

For 2D acceleration, Xorg moved through XAA (XFree86 Acceleration
Architecture), EXA, UXA, and SNA, and now ships **Glamor** as the
generic accelerator. Glamor translates X Render primitives into OpenGL
operations and was mainlined in xorg-server 1.16 (2014). Its explicit
goal is to "obsolete and replace all the DDX 2D graphics device
drivers" by routing everything through the existing 3D stack [1] [9].
Glamor is about 15k lines.

Input handling changed in xorg-server 1.16 (libinput wrapper added)
and 1.19 (input thread). Before 1.19, input drivers either polled from
the main loop (slow when the server was busy rendering) or installed a
`SIGIO` handler that interrupted the main thread. The `SIGIO` path
gave responsive cursor movement but required all event-processing
code to be async-signal-safe: no `malloc`, no library calls, and the
server even shipped its own `printf` reimplementation for error
logging. Keith Packard landed a separate input thread in the 1.19
development cycle. That thread owns all input device file descriptors,
reads events continuously, and pushes them onto the queue the main
thread drains. The `xf86BlockSIGIO()` calls became `input_lock()` /
`input_unlock()` [7].

Current status: still maintained on the `main` branch of the xserver
git repo, but only barely. The 21.1 release series ships regression
fixes and security updates; Red Hat Enterprise Linux 10 plans to
remove Xorg entirely, Fedora 43 was cleared to ship GNOME as
Wayland-only, and Ubuntu 25.10 drops the Xorg session for GNOME [8]
[28] [29] [30]. The reason Xorg is no longer the default is not
technical rot of the protocol so much as the design: any X server
running as root with direct hardware access and a 40-year-old
eventually-consistent input model is hard to secure, and the
compositor model in Wayland resolves most of the long-standing
problems (every client can see every window, input is broadcast,
screen scraping is the default) [8]. Xwayland stays installed because
X11 applications still need an X server to talk to; Xorg the
bare-metal session is what distros are removing.

Xorg itself never speaks libX11 or libXCB on the server side. The
server reads the wire protocol directly off the X socket in `dix/`,
dispatches by request opcode, and writes replies and events back as
raw bytes. The same is true for every DDX in `hw/`. This is the same
approach Ishizue's bridge takes.

## Xwayland (cross-reference)

Covered in depth in `W6-A`. Summary for completeness: Xwayland is the
`hw/xwayland` DDX inside the same xserver git tree as Xorg, mainlined
in 1.16 in 2014 [1] [8]. It presents a real X11 socket to clients and
acts as a Wayland client to the host compositor. It can run in
rootless mode (each X11 top-level window becomes a Wayland
`xdg_toplevel`) or rooted (one big Wayland surface holding the whole
X11 screen) [35]. Xwayland is the only X server in the tree still
getting regular releases: each `xwayland-23.x.y` tag is `xserver`
master with `hw/{kdrive,vfb,xfree86,xnest,xquartz,xwin}` removed [8].
It uses DMA-BUF for the GL texture path on the host compositor side
through `linux-dmabuf-v1` [15]. About 15k lines of C in
`hw/xwayland`.

## Xephyr

Xephyr is a kdrive-family X server that targets a window on a
pre-existing X server as its framebuffer [5]. It was written by
Matthew Allum and lives at `hw/kdrive/ephyr/` in the xserver tree,
about 6k lines of C.

The design difference from Xnest is that Xephyr is a real X
server, not a proxy. The host X server window is its framebuffer. It
copies damaged regions to that window via shared-memory XImages
(`MIT-SHM`), which keeps the per-frame cost down to a `XShmPutImage`
per dirty rectangle rather than a round-tripped `CopyArea` per
drawing primitive. Because it is a real server, it can advertise
modern extensions (Composite, Damage, RandR, RENDER) even if the host
X server does not [2] [5].

The trade-off is OpenGL: the in-tree Xephyr does software-only GL
rendering. A fork by Feng Haitao demonstrated hardware-accelerated GL
by piping DRI through to the host, but that work was never merged [2].
Glamor can be enabled in Xephyr, which accelerates 2D RENDER but does
not give you client-side GL [5] [9].

Xephyr also has a visual debug mode that flashes damaged regions,
which is useful for working on compositors and window managers [2]
[5]. It is the standard tool for testing tiling WMs and for running a
second X session inside an existing one without a VT switch.

How it differs from Xwayland: Xwayland targets a Wayland compositor
and uses Wayland surfaces, DMA-BUF, and `xdg_toplevel`. Xephyr targets
an X server and uses an X window plus SHM XImages. Both are "real"
servers (not proxies) and both rely on the parent for output only.
Xwayland is the modern, hardware-accelerated equivalent of the Xephyr
model, with DMA-BUF replacing SHM XImages as the fast path.

## Xnest

Xnest is the older nested X server, written by Davor Matic in 1993
(the copyright header on `hw/xnest/Init.c` says so explicitly). It
lives at `hw/xnest/`, about 3.7k lines of C. Like Xephyr it runs
inside an existing X server, but architecturally it is a proxy: every
X11 operation the nested server receives is translated and forwarded
to the parent X server, where it appears as another client [2] [33].

Because it is a proxy, Xnest is limited to whatever the parent server
supports. No Composite, no Damage, no RandR if the parent does not
have them. It also relies on the parent for font services [33]. The
result is that running a modern toolkit inside Xnest tends to produce
visual artifacts and missing extensions, and Xephyr's README describes
its own purpose as "Xnest but with support for modern extensions like
composite, damage and randr" [5].

What to learn from Xnest's mistakes:

1. **Proxying is the wrong shape.** A nested server that forwards
   every primitive to a parent is bottlenecked on round-trips and
   inherits every limitation of the parent. Drawing into a local
   framebuffer and shipping dirty rects (Xephyr) or surfaces
   (Xwayland) is structurally cheaper.
2. **Don't depend on the parent's extension set.** The nested server
   should advertise what *it* implements, not what the parent
   implements.
3. **Don't share font or selection state implicitly.** Xnest's font
   dependency on the parent made it useless once server-side fonts
   stopped being the norm.

## kdrive

KDrive (also called TinyX) is a small X server implementation written
by Keith Packard for low-memory environments. A KDrive server with
RENDER support but without scalable fonts compiled to under 700 KB of
text on Linux/x86 [6]. The original targets were embedded devices and
thin clients. Eric Anholt later extended KDrive's 2D acceleration in
the work reported in "High Performance X Servers in the Kdrive
Architecture" at USENIX 2004, which is also where KAA (KDrive
Acceleration Architecture) was introduced; KAA later mutated into EXA
in the mainline server [6] [31].

Architecturally KDrive is interesting because it is the only small,
self-contained X server implementation in the xserver tree. A KDrive
server does not require any configuration files and works even
without on-disk fonts. All configuration is compile-time plus
command-line flags. Each KDrive server is a single binary linked
against a small os layer, a small framebuffer layer (`fb/`, shared
with Xorg), and one display/input driver. Standard KDrive variants
included `Xfbdev` (Linux fbdev, unaccelerated), `Xvesa` (x86 VESA
BIOS), and `Xephyr` (window in a host X server) [6].

In the current xserver tree KDrive lives at `hw/kdrive/` with about
11k lines total: ~5k in `hw/kdrive/src/` (the shared kdrive core) and
~6k in `hw/kdrive/ephyr/` (the Xephyr variant). The other KDrive
backends (`Xfbdev`, `Xvesa`, `Xsdl`) were removed over the years as
their target platforms either moved to DRM/KMS or became irrelevant.
KDrive is not built by default in modern Xorg releases; the X11Libre
fork has done recent work getting the kdrive keyboard driver working
with the threaded input model [36]. The design is still relevant
because it shows how small a real X server can be.

What is worth borrowing from KDrive:

- A single DDX is a few thousand lines of C, not 100k. The Xorg
  `hw/xfree86` DDX is huge because it carries decades of legacy driver
  ABI; a from-scratch DDX only needs the pieces its target hardware
  (or pseudo-hardware) requires.
- Compile-time configuration instead of runtime config files. SPEC
  §4 already does this with build-time resource limits.
- One binary per backend. SPEC §10 has the same shape: `ISZ_BACKEND_DRM`,
  `ISZ_BACKEND_HEADLESS`, and `ISZ_BACKEND_NESTED` are picked at
  `isz_init()` time.
- The os layer is shared, the DDX is pluggable. Ishizue's
  `isz_backend_ops` is the same pattern at a different layer.

## X11rdp and xorgxrdp

X11rdp was an X server that rendered to the RDP protocol instead of a
local display, used by xrdp for Linux remote desktop sessions [3]
[11] [32]. It was a fork of the Xorg source tree with a custom DDX
that shipped RDP bitmaps to the xrdp daemon instead of blitting to a
real framebuffer. It was deprecated in 2019 because maintaining a
full Xorg fork was too expensive [3].

xorgxrdp is the replacement. It is not a fork: it is "a collection of
modules to be used with a pre-existing X.Org install to make the X
server act like X11rdp" [12]. The trick is that Xorg's DDX already
supports loadable driver modules, so xorgxrdp ships an
`xrdp_drv.so` video driver module and an input driver module that
plug into a stock `Xorg` binary. The xrdp daemon launches
`Xorg :10 -config xrdp/xorg.conf`, which loads the xorgxrdp modules
at startup. The result is functionally equivalent to the old X11rdp
fork but with no Xorg fork to maintain [12].

The rendering path differs from a normal Xorg session in one place:
instead of `glamor` or `modesetting` driving a KMS CRTC, the
`xrdp_drv.so` driver captures the damaged regions of the root
pixmap and ships them to the xrdp daemon over a Unix socket, which
then encodes them as RDP surface commands. The DDX still owns a real
in-memory framebuffer; what changes is where the framebuffer gets
sent. Color depth is fixed at 24bpp internally and xrdp translates
for the client; RDP clients can disconnect and reconnect to the same
session with different color depths [12].

xorgxrdp is about 14.6k lines of C across `module/`. The xrdp daemon
itself is about 24k lines and is independent of any X server.

The design lesson for Ishizue: when the output is a remote protocol
rather than a CRTC, the clean split is "DDX writes to a framebuffer,
something else ships the framebuffer over the wire." The xorgxrdp
module pattern is a textbook example of why the loadable-DDX ABI
exists at all.

## Xvnc

Xvnc is a VNC server that acts as an X server with a virtual display.
It listens for VNC viewers on TCP port 5900 plus the display number
and speaks the RFB (Remote Framebuffer) protocol [4] [13] [14]. RFB
is a thin rectangle-streaming protocol: the server maintains a
framebuffer, tracks which rectangles changed since the last send, and
ships those rectangles to connected viewers. There is no display
hardware involved; the "display" is a chunk of memory.

There are several Xvnc forks in the wild. TightVNC ships `Xtightvnc`,
TigerVNC ships `Xvnc`, RealVNC ships its own variant. They all share
the same origin (the original AT&T VNC codebase from the late 1990s)
and the same architecture: a custom X server that writes its root
pixmap into a host-side buffer and runs an RFB encoder thread per
connected viewer.

TigerVNC's Xvnc is implemented as a patchset on top of the Xorg
source tree. The TigerVNC repo carries `unix/xserver120.patch` and
`unix/xserver21.patch` for the two supported Xorg versions, plus a
`unix/xserver/hw/vnc/` directory with the actual VNC DDX. The TigerVNC
`unix/` subtree is about 17k lines of C++, and the shared `common/`
RFB library is another 17k. This is structurally the same as the
pre-2019 X11rdp approach (a fork of Xorg with an extra DDX), and it
has the same maintenance cost: every Xorg point release requires the
patch to be rebased.

Xvnc is slow because RFB ships pixels rather than drawing commands.
The protocol is inherently bandwidth-hungry on anything but an idle
desktop, and the encoders (Tight, ZRLE) only help so much. It is
still widely used because it works everywhere: any VNC client on any
OS can connect, the server has no GPU dependencies, and the model of
"a desktop that survives disconnects" maps cleanly onto virtual
displays.

## Weston's RDP, VNC, and PipeWire backends

Weston is the reference Wayland compositor. It is not an X server and
does not speak X11. It is included here because its remote-desktop
backends are a clean example of how a compositor can expose its output
via a remote protocol without becoming a remote-desktop server in its
own right.

Since Weston 13.0 (December 2023) the backend layer supports loading
multiple backends simultaneously, and the RDP, VNC, and PipeWire
backends can run as secondary backends alongside the native DRM
backend [15] [16] [17]. The release notes give the example:

    $ weston -B drm,rdp

This runs Weston with the DRM backend driving the local display and
the RDP backend simultaneously streaming the same scene graph to RDP
clients. The implementation required a rework of Weston's internal
scene-graph and damage tracking so that multiple backends can consume
the same composited output [15].

LOC sizes: `libweston/backend-rdp/` is about 4.8k lines of C,
`libweston/backend-vnc/` is about 1.4k, `libweston/backend-pipewire/`
is about 1.5k. These are small because they only do the
"encode composited output as RDP/VNC/PipeWire and ship it" job; the
compositing itself is done by the shared renderer.

The relevance for Ishizue's §10 nested backend (post-v1) is that
"backend" in the libweston sense already means exactly what Ishizue's
`isz_backend_ops` means: an output sink. A future Ishizue RDP, VNC, or
PipeWire backend would fit the existing interface with no changes to
the rest of the library, exactly as Weston's RDP backend fits next to
the DRM backend. The Weston implementation is the reference for how
small such a backend can be.

## Nested compositors (Mutter, KWin, Miriway)

Nested compositors are Wayland compositors that run as clients of
another compositor. The pattern is the same: the nested
compositor connects to the parent over the Wayland socket, creates a
single `wl_surface` (or one per output), renders its entire desktop
into that surface, and reads input from the parent. The Wayland book
notes that nesting is feasible because the protocol is asynchronous
[24].

**Mutter nested.** Mutter, GNOME's compositor, has a "nested backend"
that creates a Wayland surface in the parent compositor and uses it
for output. According to Jonas Ådahl on GNOME Discourse, "the nested
backend is really only meant for debugging, and can't be manipulated
to be an isolated containerized DE running on some other host
compositor. [...] It's using part of the X11 backend, and doesn't
support a bunch of other things, e.g. virtual monitors, remote
control, etc." [21]. GNOME 50 removed Mutter's X11 backend entirely,
which makes the nested backend Wayland-only going forward.

**KWin nested.** KWin has been able to run nested since at least 2013.
Martin Grässlin's blog post from May 2013 describes the mechanism:
"KWin creates a connection to Wayland, creates a Wayland surface and
uses it for OpenGL output. KWin also gets input from Wayland and
passes it to the windows" [23]. The KDE wiki notes that nested KWin
picks its parent based on environment variables: `DISPLAY` triggers
the X11 parent backend, `WAYLAND_DISPLAY` triggers the Wayland parent
backend [22]. The X11 path uses the wlroots-style "be an X client"
approach rather than a Wayland-only nested path.

**Miriway.** Miriway is a small (~800 lines of code) Wayland
compositor built on Canonical's Mir library [19] [20]. It is not
nested by default; it is a host compositor. It is included here
because it demonstrates how thin the WM-on-top-of-a-compositor-library
layer can be when the library handles the hard parts. Miriway
implements floating-window management, workspaces, configurable
shortcuts, and Xwayland integration in under 3k lines of C++ because
Mir owns rendering, input, and the Wayland protocol implementation
[19].

For Ishizue's §10 nested backend the lesson is that nested mode is a
debugging and development feature, not a production feature, in every
compositor that ships it. Mutter explicitly says so. KWin's nested
mode is used for development. Even Gamescope (next section), which is
the closest thing to a production nested compositor, is single-window
by design. The Ishizue nested backend should be sized accordingly:
good enough to run an Architect inside an existing desktop session
for development, not a hardening target.

## GameScope

GameScope is Valve's micro-compositor, originally called
steamcompmgr. Its source lives at `github.com/ValveSoftware/gamescope`
[18] [25]. It is about 54k lines of C++ in `src/` (38.6k `.cpp`,
2.9k `.hpp`, the rest headers and shaders), which is small for a
compositor that does what it does.

GameScope serves two use cases. In the **embedded** use case it runs
directly on DRM/KMS, no parent compositor, and flips game frames to
the screen with as few copies as possible. In the **nested** use case
it runs as a client of an existing X11 or Wayland desktop. The README
explains the embedded path: "It's getting game frames through Wayland
by way of Xwayland, so there's no copy within X itself before it gets
the frame. It can use DRM/KMS to directly flip game frames to the
screen, even when stretching or when notifications are up, removing
another copy. When it does need to composite with the GPU, it does so
with async Vulkan compute" [18].

The architectural shape is:

- **Backends.** `src/Backends/` has one implementation per parent
  surface: DRM (embedded), SDL (nested on X11 or Wayland), Wayland
  (nested on Wayland). The backend owns the output surface and the
  vblank source.
- **Rendering.** `src/rendervulkan.cpp` does Vulkan compositing. There
  is no GL renderer; Vulkan is the only path.
- **X11 layer.** GameScope does not speak X11 itself. It runs an
  internal Xwayland instance and presents an X11 socket to the game.
  The game sees a normal X server; Xwayland translates to Wayland;
  GameScope's `wlserver.cpp` receives the Wayland surfaces and
  composites them.
- **Scaling and filtering.** FSR, NIS, integer scaling, and Reshade
  shaders are all done as Vulkan compute passes in the composite
  pipeline.
- **Input.** `src/LibInputHandler.cpp` reads from libinput on the
  embedded path; on the nested path input comes from the parent
  compositor via Wayland.

The reason GameScope is worth studying for Ishizue is that it is the
closest existing project to what Ishizue is trying to be: a
from-scratch, modern, single-purpose compositor that talks to clients
through one compatibility shim (Xwayland) and to hardware through a
small set of well-defined backends. The differences from Ishizue are
in scope, not in shape. GameScope is single-application by design
[25] and ships its own X11 compatibility (via Xwayland) rather than
implementing X11 directly. Ishizue's bridge is the inverse: it speaks
X11 directly (no Xwayland) and lets the Architect handle everything
else.

The other thing worth copying is the backend boundary. GameScope's
`src/Backends/` directory is the same pattern as Ishizue's
`src/backend/` and Weston's `libweston/backend-*/`: one subdir per
output sink, a shared interface above. SPEC §10 already requires this
shape.

## Wayfire on X11 (wlroots X11 backend)

Wayfire is a Wayland compositor built on wlroots. It does not have its
own X11 backend; the "Wayfire X11 backend" referred to in Wayfire
issue #616 is wlroots's `backend/x11/`, which lets any wlroots-based
compositor run as an X11 client [26]. The wlroots X11 backend is
about 1.9k lines of C across three files:
`backend/x11/backend.c` (746 lines), `backend/x11/output.c` (804
lines), and `backend/x11/input_device.c` (331 lines).

The wlroots X11 backend is the inverse of Xwayland. Where Xwayland is
an X server that talks Wayland to its parent, the wlroots X11 backend
is a Wayland server that talks X11 to its parent. The compositor
creates one X11 window per Wayland output, renders its composited
framebuffer into that window, and reads X11 input events back as
wlroots pointer/keyboard events.

The implementation is built on libxcb and uses a long list of X11
extensions: `xcb/dri3.h`, `xcb/present.h`, `xcb/render.h`,
`xcb/shm.h`, `xcb/xfixes.h`, `xcb/xinput.h`. DRI3 is used to import
client DMA-BUFs as X11 pixmaps, Present is used for vblank-synced
page flips, XInput2 is used for modern input devices [source:
`backend/x11/backend.c` header includes]. Known limitations, called
out in Wayfire issue #2171, are that the X11 backend does not support
shared pixmaps and cannot deliver keycodes above 255 [27]. Both
limitations come from the X11 protocol itself, not from wlroots.

The relevance for Ishizue is mostly negative: the wlroots X11 backend
is exactly the libXCB-based X11 client implementation that Ishizue's
bridge is deliberately not doing. SPEC §13 requires raw X11 bytes
with no libX11/libXCB dependency. The wlroots backend is a useful
reference for which X11 extensions a modern nested implementation
needs (DRI3, Present, XInput2, SHM, Render, Xfixes) but not for the
implementation approach.

## Quick comparison table

LOC counts are approximate, taken from each project's source tree on
2026-07-18. "X11 wire" indicates whether the project parses the X11
protocol itself.

| Implementation | Language | Approx. LOC | X11 wire | Rendering backend | Target use case | Maintenance | License |
|---|---|---|---|---|---|---|---|
| Xorg (`hw/xfree86`) | C | ~106k (DDX only); ~322k full tree | Yes (server side, raw) | modesetting DDX + Glamor (OpenGL) | Bare-metal X11 session | Security/regression fixes only; removed from RHEL 10, Fedora 43, Ubuntu 25.10 GNOME [8] [28] [29] | MIT |
| Xwayland (`hw/xwayland`) | C | ~15k | Yes (server side, raw) | Wayland surfaces + DMA-BUF + Glamor | X11 compat for Wayland compositors | Actively maintained, regular releases [8] | MIT |
| Xephyr (`hw/kdrive/ephyr`) | C | ~6k | Yes (server side, raw) | SHM XImage to a parent X window | Sandboxed/test nested X session | In-tree, builds with `--enable-kdrive`; rare updates | MIT |
| Xnest (`hw/xnest`) | C | ~3.7k | Yes (server side, raw) | X11 proxy to parent server | Legacy nested X (1993) | In-tree, effectively unmaintained | MIT |
| kdrive (`hw/kdrive/`) | C | ~11k (src + ephyr) | Yes (server side, raw) | Per-DDX: fbdev, VESA, host X window | Embedded/thin-client X server | Largely abandoned; X11Libre fork doing minor work [36] | MIT |
| X11rdp | C | n/a (deprecated, replaced by xorgxrdp) | Yes (fork of Xorg) | RDP via xrdp daemon | xrdp backend | Deprecated in 2019 [3] | Apache 2.0 (xrdp) |
| xorgxrdp | C | ~14.6k (module dir) | No (DDX module for stock Xorg) | RDP via xrdp daemon | xrdp backend, replaces X11rdp | Actively maintained [12] | Apache 2.0 |
| Xvnc (TigerVNC) | C++ | ~17k (unix/) + ~17k (common RFB) | Yes (patchset on Xorg) | RFB over TCP, in-memory framebuffer | Remote desktop, survives disconnect | Maintained; rebases on Xorg releases | GPLv2+ |
| Weston `backend-rdp` | C | ~4.8k | No (Wayland compositor backend) | RDP encoding of composited output | Remote access alongside DRM backend | Actively maintained [15] | MIT |
| Weston `backend-vnc` | C | ~1.4k | No (Wayland compositor backend) | RFB encoding of composited output | Remote access alongside DRM backend | Actively maintained [15] | MIT |
| GameScope | C++ | ~54k (src/) | No (uses internal Xwayland) | Vulkan (async compute), DRM/KMS or nested | Steam Deck / gaming session compositor | Actively maintained by Valve [18] [25] | BSD-2-Clause |
| Miriway | C++ | ~2.9k | No (Wayland compositor; uses Xwayland for X11 clients) | Mir renderer | Generic Mir-based desktop shell | Actively maintained [19] [20] | GPLv2+/GPLv3+ |
| Mutter nested | C | part of Mutter | No (Wayland compositor) | Wayland surface in parent compositor | Debugging only [21] | Maintained as part of GNOME | GPLv2+ |
| KWin nested | C++ | part of KWin | No (Wayland compositor; can nest under X11) | Wayland or X11 surface in parent | Debugging only [22] [23] | Maintained as part of KDE | GPLv2+ |
| Wayfire (on X11) | C++ | part of Wayfire; uses wlroots X11 backend | No (Wayland compositor) | Wayland compositor rendered into X11 window via wlroots | Testing [26] [27] | Maintained | GPLv3+ |
| wlroots `backend/x11` | C | ~1.9k | Yes (client side, via libxcb) | X11 window + DRI3 + Present | Run wlroots compositors under X11 | Actively maintained as part of wlroots | MIT |

## What Ishizue's bridge can borrow

Four patterns are worth copying.

1. **kdrive's minimalism.** A real X server does not need to be 300k
   lines. kdrive's `hw/kdrive/src/` is 5k lines and implements a
   working X server. The Ishizue bridge is smaller in scope than
   kdrive (it does not need to be a standalone X server with its own
   fb, only a translator from X11 wire to Ishizue's wire protocol),
   so 5k lines is an upper bound, not a target. The kdrive lesson is
   that compile-time configuration and one-binary-per-backend beat a
   config-file-driven monolith.

2. **Xephyr's nesting approach.** A nested server that draws into a
   local framebuffer and ships dirty rects to a parent is
   structurally cheaper than a proxy that forwards every primitive
   (Xnest). The Ishizue §10 nested backend should follow the Xephyr
   model: own the framebuffer, ship damage to the parent. SPEC §10
   already fixes the API shape (`ISZ_BACKEND_NESTED` with a parent
   window handle) so this is just an implementation note for whoever
   lands the post-v1 nested backend.

3. **Xwayland's DMA-BUF path.** Xwayland gets GPU textures onto the
   host compositor without a CPU copy by passing DMA-BUF file
   descriptors through `linux-dmabuf-v1` [15] [35]. The Ishizue
   bridge's X11 `ShmPutImage` and `PutImage` paths can be slow paths;
   the fast path is X11 clients that allocate DMA-BUFs (via DRI3 on
   Xwayland, or via the Ishizue native protocol on Ishizue clients)
   and pass them through. SPEC §8 already specifies the DMA-BUF
   import path; the bridge just needs to map X11 DRI3 requests onto
   it.

4. **GameScope's modern structure.** GameScope is the cleanest
   existing example of the shape Ishizue is aiming for: small
   compositing core, one directory per backend, Vulkan rendering, no
   GL legacy. The Ishizue bridge does not need Vulkan or a compositor
   core (Ishizue itself is the compositor), but the directory layout
   (`Backends/`, `Utils/`, `Apps/`) and the discipline of "backend
   owns the output surface, nothing else does" are directly
   applicable. SPEC §10's `isz_backend_ops` is the same pattern.

A fifth pattern worth copying is implicit in the xorgxrdp design:
when the output sink is a remote protocol, the DDX should write to a
local framebuffer and a separate component should ship the
framebuffer over the wire. This is the same shape as Xvnc, Weston's
RDP backend, and xorgxrdp, and it is the shape any future Ishizue
remote-access backend should take.

## What Ishizue's bridge should not borrow

Three anti-patterns to avoid.

1. **Xorg's DIX/DDX split as Xorg does it.** Xorg's DIX is 43k lines
   because it implements every X11 core protocol, every extension,
   every font format, and every acceleration architecture the project
   has ever shipped. The Ishizue bridge does not need a DIX. It needs
   an X11 wire parser, an X11-to-Ishizue translator, and an Ishizue
   client. The DIX/DDX split inside Xorg exists to let multiple
   servers share code; the Ishizue bridge is one binary with one
   purpose, and the equivalent split (X11 parsing vs. Ishizue
   translation) is just two source files.

2. **Xnest's old codebase.** Xnest is a proxy. It forwards every
   primitive to a parent and inherits every limitation of the
   parent. The Ishizue bridge is not a proxy and should not behave
   like one. It should own its state (window tree, visuals, atoms,
   selections) and translate at the boundary, not delegate to a
   parent X server. The Xephyr README is explicit that Xephyr exists
   because the Xnest proxy model does not work for modern extensions
   [5].

3. **Anything requiring libX11 or libXCB.** SPEC §13 says raw X11
   bytes, no libX11, no libXCB. The wlroots X11 backend is the
   reference for which extensions matter (DRI3, Present, XInput2,
   SHM, Render, Xfixes) but its implementation is built on `xcb/*`
   headers throughout. The Ishizue bridge needs to parse and emit
   those extensions by hand. This is more work up front and less work
   forever after: no libxcb dependency to track, no ABI to bind to,
   no second library to security-audit. The x11bridge scaffold in
   `x11bridge/x11_proto.{h,c}` already takes this approach.

A subtler non-borrow: do not copy GameScope's choice to embed Xwayland
as the X11 layer. GameScope can do that because it is a Wayland
compositor and Xwayland is the right answer for "X11 clients on
Wayland." Ishizue is not a Wayland compositor; it has its own native
protocol. The bridge has to speak X11 directly. The right thing to
borrow from GameScope is the backend boundary and the discipline, not
the Xwayland dependency.

## References

[1] Wikipedia, "X.Org Server", https://en.wikipedia.org/wiki/X.Org_Server
[2] Wikipedia, "Xephyr", https://en.wikipedia.org/wiki/Xephyr
[3] Wikipedia, "Xrdp", https://en.wikipedia.org/wiki/Xrdp
[4] Wikipedia, "RFB (protocol)", https://en.wikipedia.org/wiki/RFB_(protocol)
[5] Xephyr README, xorg/xserver source tree, https://github.com/XQuartz/xorg-server/blob/master/hw/kdrive/ephyr/README (mirrored from https://cgit.freedesktop.org/xorg/xserver/tree/hw/kdrive/ephyr/README)
[6] Julius Chroboczek, "The KDrive Tiny X Server", https://www.irif.fr/~jch/software/kdrive.html (archived copy: https://web.archive.org/web/2024/https://www.irif.fr/~jch/software/kdrive.html)
[7] Peter Hutterer, "Input threads in the X server", Who-T blog, 2016-09-09, http://who-t.blogspot.com/2016/09/input-threads-in-x-server.html
[8] Peter Hutterer, "Xorg being removed. What does this mean?", Who-T blog, 2023-12-14, http://who-t.blogspot.com/2023/12/xorg-being-removed-what-does-this-mean.html
[9] Freedesktop.org, "Glamor", https://www.freedesktop.org/wiki/Software/Glamor
[10] ArchWiki, "Xorg", https://wiki.archlinux.org/title/Xorg
[11] ArchWiki, "Xrdp", https://wiki.archlinux.org/title/Xrdp
[12] neutrinolabs/xorgxrdp README, https://github.com/neutrinolabs/xorgxrdp
[13] TightVNC, "Xvnc(1) manual page", https://www.tightvnc.com/Xvnc.1.php
[14] TigerVNC, "Xvnc documentation", https://tigervnc.org/doc/Xvnc.html
[15] Marius Vlad (Collabora), "Weston 13.0 release: Backends consolidation", 2023-12-21, https://www.collabora.com/news-and-blog/news-and-events/weston-13-release-backends-consolidation.html
[16] Weston documentation, "Running Weston", https://wayland.pages.freedesktop.org/weston/toc/running-weston.html
[17] Pengutronix, "Weston, VNC Server", 2022-12-21, https://pengutronix.de/en/blog/2022-12-21-weston-vnc.html
[18] ValveSoftware/gamescope README, https://github.com/ValveSoftware/gamescope
[19] Miriway/Miriway README, https://github.com/Miriway/Miriway
[20] Ubuntu Discourse, "Miriway - bringing Wayland to your desktop", https://discourse.ubuntu.com/t/miriway-bringing-wayland-to-your-desktop/40707
[21] Jonas Ådahl on GNOME Discourse, "Does any documentation about running nested mutter/gnome-shell exist?", 2024-06, https://discourse.gnome.org/t/does-any-documentation-about-running-nested-mutter-gnome-shell-exist/21476
[22] KDE Community Wiki, "KWin/Wayland", https://community.kde.org/KWin/Wayland
[23] Martin Grässlin, "KWin running in Weston", 2013-05, https://blog.martin-graesslin.com/blog/2013/05/kwin-running-in-weston
[24] Wayland book, "Types of Compositors", https://wayland.freedesktop.org/docs/book/Compositors.html
[25] ArchWiki, "Gamescope", https://wiki.archlinux.org/title/Gamescope
[26] Wayfire issue #616, "wayfire without Xorg", https://github.com/WayfireWM/wayfire/issues/616
[27] Wayfire issue #2171, "[backend/x11/backend.c:478] X11 does not support shared pixmaps", https://github.com/WayfireWM/wayfire/issues/2171
[28] It's FOSS, "Now Ubuntu is Also Ditching Xorg Completely for Wayland!", https://itsfoss.com/news/ubuntu-25-10-wayland-only
[29] Fedora discussion, "Xorg removed, how to get them back", https://discussion.fedoraproject.org/t/xorg-removed-how-to-get-them-back/132151
[30] Fedora Project, "F43 Change Proposal: Wayland-only GNOME (self-contained)", https://discussion.fedoraproject.org/t/f43-change-proposal-wayland-only-gnome-self-contained/150261
[31] Eric Anholt, "High Performance X Servers in the Kdrive Architecture", USENIX 2004, https://www.usenix.org/legacy/event/usenix04/tech/freenix/full_papers/anholt/anholt.pdf
[32] LinuxQuestions.org, "What is X11rdp?", https://www.linuxquestions.org/questions/ubuntu-63/what-is-x11rdp-4175556435
[33] Trisquel forum, "Xephyr X server instead of Xnest?", https://trisquel.info/en/forum/xephyr-x-server-instead-xnest
[34] X.Org, "XFree86 DDX Design", https://xorg.freedesktop.org/archive/current/doc/xorg-server/ddxDesign.html
[35] Wayland book, "X11 Application Support", https://wayland.freedesktop.org/docs/book/Xwayland.html
[36] X11Libre/xserver releases, https://github.com/X11Libre/xserver/releases
