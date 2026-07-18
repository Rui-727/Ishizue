# Ishizue client-stack ecosystem: a strategic survey

This document maps the client stacks and window-system protocols a real-world Linux window server built on Ishizue would have to talk to. It is written against Ishizue SPEC v1 (the `SPEC.md` in this repo) and the wire protocol in `doc/protocol.md`. The goal is not to argue for any particular feature. It is to fix the territory so that decisions about what Ishizue v1 ships, what it defers, and what it explicitly refuses are made against the same set of facts.

Ishizue is a mechanism library, not a compositor [1]. It owns DRM/KMS, libinput, the wire protocol, and buffer management. The Architect (whoever links the library) owns policy. The wire protocol is custom, not Wayland [1, §2]. The only client-compatibility layer currently scoped is an X11 bridge, run as a separate privileged client [1, §13]. Everything else this document covers is either a candidate for v2, a candidate for an out-of-tree add-on, or an explicit non-goal.

Each section ends with a short note on what this means for Ishizue. Section 16 collects those notes into a priority order for v1.

## 1. Native Ishizue clients

A native Ishizue client is a process that speaks the wire protocol defined in `doc/protocol.md` directly over the Unix domain socket the Architect bound and passed to `isz_init()` as `listen_fd` [1, §6.1] [3]. There is no `libishizue-client` today. The `x11bridge/` directory compiles `src/protocol/isz_protocol.c` and `src/protocol/isz_conn.c` directly into the bridge binary, because the framing primitives are `ISZ_INTERNAL` (hidden) in the `.so` and cannot be linked from there [60]. That works for a single in-tree binary. It does not scale to out-of-tree clients, and it is not a usable client story.

The closest analogue in the existing ecosystem is `libwayland-client`. The Wayland reference implementation is split into `libwayland-client` and `libwayland-server`, both C libraries whose main job is IPC, marshalling, and message synchronisation [5]. A client uses `libwayland-client` to talk to one or more servers. A `wl_display` object manages each open connection. At least one `wl_event_queue` is created per display, holding events until the client processes them. Each object the client creates is represented by a `wl_proxy` that the library allocates, carrying the object id sent over the socket, a `void *` user-data pointer, and a pointer to a static `wl_interface` generated from the protocol XML [5] [6]. Messages are sent by `wl_proxy_marshal`, which uses the message id and the interface to identify argument types and serialise them; type-safe wrappers generated from the XML call `wl_proxy_marshal` underneath [5]. The library exposes a file descriptor and lets the caller pick its own polling mechanism. It provides low-level access only.

That is the bar a `libishizue-client` would have to clear. The minimum surface is small: connect to a path, do the `ISZH` magic plus version handshake [3, §6.2], drive a non-blocking recv loop with `SCM_RIGHTS` handling, and expose typed wrappers for the 50 message ids in the current table [3, message table]. Beyond transport, a usable client library needs four things that the spec already implies but does not yet expose client-side:

- Surface creation and lifecycle. The protocol has `ISZ_MSG_SURFACE_CREATE`, `_DESTROY`, `_SET_POSITION`, `_SET_SIZE`, `_SET_PLANE_TYPE`, `_SET_PLANE_SLOT`, `_SET_ZPOS`, `_SET_TRANSFORM` [3]. A client helper would wrap these into a `isz_client_surface` opaque type with `create`, `attach_buffer`, `damage`, `commit`, `destroy`. Plane slot assignment is mandatory at commit time [1, §7.7], so the client helper has to expose it; it cannot pick a default silently, because picking a default would be exactly the kind of unilateral policy decision Ishizue refuses to make [1].
- Buffer attach. DMA-BUF fds travel as `SCM_RIGHTS` ancillary data with a `fd_index` slot mapping into the cmsg array [3, FD passing]. The client library has to own `sendmsg` with cmsg, track in-flight buffers (up to two per surface [1, §8]), and surface `release` events back to the caller. This is the single fiddliest piece of a client library because partial writes are fatal for the connection [3, queue depth].
- Input handling. Keyboard, pointer, and touch events arrive unsolicited [3, input messages]. The client library has to dispatch them to listener callbacks keyed by seat id. Keycodes are raw evdev; the spec deliberately does no keysym translation [1, §9], so a client library should expose libxkbcommon plumbing as an optional helper, not bake it in.
- Frame pacing. `ISZ_MSG_PRESENTED` carries a `CLOCK_MONOTONIC` vblank timestamp [3, presented]. The client library should surface this as a callback so callers can avoid producing frames faster than the refresh rate [1, §7.3]. Without this, a naive client burns CPU rendering into a queued buffer that gets dropped.

Should Ishizue ship a client library at all? The spec's stance on the server side is mechanism-only, with the Architect making every policy decision [1, §1]. The same logic applies to clients. A thin `libishizue-client` that does transport, handshake, framing, and typed message wrappers is mechanism. Anything beyond that, cursor fallbacks, default plane slotting, damage coalescing, keymap compilation, is policy and should stay in the caller. The Wayland split is instructive: `libwayland-client` is deliberately low-level and expects the caller to wait for events however it likes [4] [5]. Ishizue should do the same. The right v1 deliverable is a small client library that mirrors `libwayland-client` in scope: connect, marshal, dispatch, fd-passing. Higher-level toolkits (section 4, section 5) build on top of it.

The spec's socket-ownership choice matters here. `isz_init()` takes an already-bound `listen_fd` rather than creating the socket itself [1, §6.1]. That makes the server library usable as an embedded component, the same pattern Wayland uses for nested compositors where the host application links the server library and accepts the connection itself [56]. A client library should mirror this: `isz_client_connect_fd(int fd)` for the case where the caller already did `connect(2)`, and `isz_client_connect_path(const char *path)` for the common case. The `x11bridge` already does the latter by hand in `isz_client.c` [60]; a library would absorb that code.

## 2. Wayland client compatibility

SPEC §2 lists Wayland protocol compatibility as out of scope, with only X11 compatibility planned [1, §2]. This section explains why that is the right call for v1 and what would change if it were not.

The adoption picture: a January 2025 survey of 3,923 Arch Linux users found 80% running Wayland and 20% on Xorg, the widest gap recorded to date [11]. Plasma 6 telemetry shows 73% of Plasma 6 users on Wayland sessions [12]. LWN reported in 2024 that Wayland had crossed 50% overall and was approaching 70% on the most popular desktops [13]. Factoring in XWayland, the share of Linux desktop sessions that touch Wayland at all is around 92-94% [11]. The remaining Xorg-only sessions are concentrated on NVIDIA proprietary drivers (which Ishizue also excludes [1, §2]) and on older or niche hardware.

Those numbers describe sessions, not applications. The application picture is messier. Most GTK and Qt apps are dual-stack: they link both the Wayland and X11 backends and pick at runtime. A 2024-2026 trend is that the major toolkits have finished their Wayland ports and are starting to deprecate X11. Electron switched to Wayland by default on Linux [31]. The remaining X11-only apps are a long tail: legacy Motif and Xt apps, some scientific software, some games, anything still on Qt 4 or GTK 2. XWayland covers that tail, and so does the Ishizue X11 bridge (section 3).

A Wayland compatibility layer for Ishizue would be the inverse of XWayland. XWayland is a full X server that runs as a Wayland client [8] [9] [10]. It speaks X11 to X11 apps on one side and Wayland to the host compositor on the other. An Ishizue Wayland bridge would speak Wayland to Wayland apps on one side and the Ishizue wire protocol on the other. It would have to embed `libwayland-server`, which is the server-side counterpart of `libwayland-client`: it provides `wl_resource` for client-created objects, `wl_global` for server-created objects, and `wl_client` for the connection state [7]. The bridge would create `wl_compositor`, `wl_shm`, `wl_shell`/`xdg_wm_base`, `wl_seat`, `wl_output`, and the rest of the core globals, plus the extension protocols (`wl_shm`, `linux-dmabuf-v1`, `presentation-time`, `xdg-shell`, `wlr-layer-shell`, etc.) that real Wayland clients expect.

Feasibility. wlroots exists and does exactly this: it provides backends that abstract KMS/DRM, libinput, Wayland, X11, and headless, plus the Wayland server-side protocol implementation [59]. libweston does the same. An Ishizue Wayland bridge would be a new compositor built on Ishizue instead of wlroots, with `libwayland-server` doing the client-facing side. This is a large but well-understood body of work. Weston and wlroots together represent roughly 60k+ lines of code, much of it protocol marshalling and the compositor logic that maps Wayland surfaces onto hardware [59]. The Ishizue-side mapping would be simpler than wlroots' KMS path because Ishizue already owns KMS: the bridge just creates `isz_surface` objects and assigns plane slots.

Why out of scope for v1. Three reasons. First, the spec chose a custom protocol deliberately, to avoid inheriting Wayland's design decisions [1, §1]. Building a Wayland bridge in v1 would import all of those decisions through the back door: the bridge would have to implement `wl_surface` commit semantics, `wl_shm` format negotiation, `xdg_wm_base` configure events, `wp_presentation` feedback, and so on, and would either replicate Wayland's policy defaults inside the bridge (violating the mechanism-only principle) or expose every one of them as a knob the Architect has to set. Second, the X11 bridge already covers the long tail of apps that cannot speak Ishizue natively [1, §13]. The marginal value of a Wayland bridge over an X11 bridge is the set of apps that are Wayland-only and cannot fall back to X11. That set is small and shrinking as Electron and the major toolkits move to Wayland-by-default with X11 fallback [31]. Third, a Wayland bridge would compete with the native protocol for mindshare. If Wayland clients work via a bridge, no one writes native Ishizue clients, and the protocol stops getting exercised. The spec's "native-protocol-first" stance is a bet that the protocol has to prove itself before compatibility layers obscure it [1, §13].

What would change if it were in scope. The library would not need to grow: the bridge is a separate process, the same shape as the X11 bridge [1, §13]. The work is all in the bridge. It would link `libwayland-server` and `libishizue-client` (section 1) and translate. The Architect would allowlist it [1, §6.3] [63]. The hard parts are not Ishizue-side; they are Wayland-side. The bridge has to implement enough of the Wayland protocol surface that real GTK and Qt apps work, which means `linux-dmabuf-v1` with modifier negotiation, `presentation-time`, `xdg-shell`, `wp_fractional_scale_v1`, `wp_tearing_control_v1`, `zwp_pointer_gestures_v1`, `zwp_relative_pointer_manager_v1`, `zwp_text_input_manager_v3` for input methods, and the long tail of wlr and kde protocols. That is a multi-year project for a single team. Weston and wlroots have been at it for over a decade.

The recommendation matches the spec. Wayland compat stays out of v1. If Ishizue gains traction and a third party wants to write the bridge, the spec already supports it: the bridge is just another privileged client [1, §13] [60].

## 3. X11 client compatibility

X11 compatibility is covered in detail by the W6-A through W6-D tasks, which build the X11 bridge scaffold described in SPEC §13 [1, §13] [60]. This section is a one-paragraph summary for completeness.

The bridge is a separate process that listens on `/tmp/.X11-unix/X<display>` and translates X11 requests into Ishizue surface, buffer, and input operations [60]. It connects to the Ishizue Unix socket as an ordinary client, completes the §6.2 handshake, and is allowlisted by the Architect [1, §6.3] [60]. The current scaffold in `x11bridge/` handles `CreateWindow`, `ConfigureWindow`, `MapWindow`, and forwards `ISZ_MSG_INPUT_*` events as X11 `KeyPress`/`KeyRelease`/`MotionNotify`/`ButtonPress`/`ButtonRelease` [60]. Most of the X11 protocol is not yet implemented: no visuals, no replies, no errors, no buffer translation, no extensions (XKB, XInput2, RANDR), no clipboard, no cursor translation [60]. The scaffold exists to fix the architectural shape before the per-message dispatch wave of the Ishizue library lands, not to run real X11 apps. The X11 bridge is the only client-compatibility layer v1 plans to ship [1, §2] [1, §13].

## 4. SDL and GLFW apps

SDL and GLFW are the two cross-platform windowing abstractions that games and embedded toolkits build on. Supporting them on Ishizue means writing a backend for each, because neither speaks raw Wayland or X11.

SDL3. SDL has a pluggable video-driver architecture. On Linux it ships with `x11`, `wayland`, `directfb`, `offscreen`, and `dummy` backends, selected at runtime via `SDL_HINT_VIDEO_DRIVER` or auto-detected [15]. SDL3 prefers Wayland over X11 by default, falling back to X11 if Wayland is unavailable or missing required protocols [14] [16]. The Wayland backend requires the host compositor to support `fifo-v1` for vsync; if it does not, SDL3 falls back to X11 [16]. The Wayland backend source is in `src/video/wayland/SDL_wayland*.c` in the SDL tree and runs to several thousand lines: it links `libwayland-client`, generates protocol bindings from `wayland-protocols.xml`, and implements `SDL_VideoDevice` with `CreateWindow`, `CreateWindowFramebuffer`/`GL_CreateContext`/`Vulkan_CreateSurface`, `SetWindowSize`, `SetWindowPosition`, `ShowWindow`, `HideWindow`, `RaiseWindow`, `SetWindowFullscreen`, `GetWindowWMInfo`, `PumpEvents`, `UpdateWindowFramebuffer`, and the cursor, clipboard, and input-method paths.

A `libSDL3` Ishizue backend would do the same against `libishizue-client` instead of `libwayland-client`. The minimum surface to get a window on screen is small: create a surface, set its plane type and slot, set its position and size, attach a DMA-BUF, damage, commit, pump events. The minimum surface to be usable for real games is much larger: Vulkan surface creation (`isz_surface_get_dma_buf` plus a custom `VkCreateIshizueSurface` equivalent, or more realistically a `wl_surface`-equivalent opaque handle the GPU driver can import), relative pointer motion, pointer confinement, keyboard grab, clipboard, drag-and-drop, frame pacing via `ISZ_MSG_PRESENTED`, and cursor surface management. The SDL3 Wayland backend is the right reference for sizing: a usable Ishizue SDL backend is on the order of 5,000-8,000 lines of C.

GLFW. GLFW 3.4 shipped runtime platform selection and improved Wayland support, with both Wayland and X11 backends buildable into the same binary [17] [18]. The Wayland backend lives in `src/wl_platform.c` and `src/wayland_*.c` in the GLFW tree. It is smaller than SDL's because GLFW is a narrower API: windows, contexts, input, monitors, no audio, no haptics. A GLFW Ishizue backend would be on the order of 2,000-4,000 lines.

Other toolkits. SFML builds on top of a windowing layer (it has its own `sf::Window` that uses X11 or Wayland directly via `libudev` plus EGL, or can ride on SDL in some configurations). Godot has its own platform layer with separate `Wayland` and `X11` drivers in `platform/linuxbsd/`. Each would need its own Ishizue driver. Dear ImGui rides on SDL or GLFW, so an Ishizue SDL or GLFW backend covers ImGui for free [Dear ImGui issue #9365 referenced in search results]. FLTK 1.5 is moving to an external SDL3 driver to replace its internal native drivers, which would also pick up an Ishizue SDL backend automatically.

The practical recommendation for v1 is to not write any of these. The X11 bridge already covers SDL and GLFW apps, because both have working X11 backends and the X11 bridge is the v1 compatibility path. An Ishizue-native SDL backend is a v2 or v3 item, justified when there are native Ishizue games that want to skip the X11 round-trip and use `ISZ_MSG_PRESENTED` directly for frame pacing. The SDL3 Wayland backend is the reference implementation to study when that work starts.

## 5. Qt and GTK apps

Qt and GTK are the two dominant desktop toolkits. Both have Wayland backends built on `libwayland-client` and X11 backends built on Xlib/XCB. Neither speaks raw Wayland. Qt uses the QtWayland module, which is split into a client side (the `wayland` QPA platform plugin) and a server side (the Qt Wayland Compositor API) [19] [20]. GTK uses the GDK Wayland backend (`gdkdisplay-wayland.c`, `GdkWaylandWaylandDisplay`), which is one of several GDK backends alongside X11, Broadway, and Win32 [22] [23].

Getting Qt and GTK apps onto Ishizue has three options, in increasing order of work and decreasing order of long-term payoff.

(a) Write a native Qt platform plugin and a native GDK backend that speak the Ishizue protocol. QPA (Qt Platform Abstraction) is the main platform abstraction layer in Qt, identified by the `QPlatform*` class prefix [21]. `QPlatformIntegration` is the single entry point for window-system-specific functionality, with factory functions for creating platform windows, screens, event dispatchers, clipboard, drag, input methods, and so on [21]. A `QPlatformIntegration` subclass for Ishizue would create `QPlatformWindow` instances backed by `isz_surface`, a `QPlatformScreen` backed by `isz_output`, a `QPlatformClipboard` using `ISZ_MSG_CLIPBOARD_*`, and so on. On the GTK side, GDK's `GdkDisplay` subclass for Ishizue would do the same. This is the most work and gives the best result: native Ishizue apps with no intermediate protocol layer, full access to Ishizue features (plane slots, hardware planes, tearing control, VRR), and no foreign policy decisions imported through a compatibility shim. QtWayland's client-side plugin and GDK's Wayland backend together represent the right size estimate: a usable QPA plugin is 8,000-15,000 lines, a usable GDK backend is similar.

(b) Write a Wayland compatibility layer so the existing Qt and GTK Wayland backends work. This is option (b) of section 2: a Wayland bridge process. Qt and GTK would run their existing Wayland backends, talk Wayland to the bridge, and the bridge would translate to Ishizue. The advantage is that every Wayland-native toolkit works without per-toolkit effort, not just Qt and GTK. The disadvantage is the multi-year protocol surface discussed in section 2.

(c) Require Qt and GTK to use their X11 backends via the X11 bridge. This is the zero-effort path and the right one for v1. Both toolkits have working X11 backends. Setting `QT_QPA_PLATFORM=xcb` and `GDK_BACKEND=x11` forces X11, which then runs through the Ishizue X11 bridge. The cost is the X11 round-trip and the loss of Wayland-native features (fractional scaling, per-output DPI, tearing hints). For v1 that is acceptable. The X11 bridge is already scoped [1, §13] and is the lowest-effort path to getting real Qt and GTK apps on screen.

The recommendation for v1 is (c), with (a) deferred to v2 and (b) left as an explicit non-goal unless a third party takes it on. The trade-off is clear: (c) gets apps working with the bridge work already planned, (a) is the right long-term answer because it gives Qt and GTK apps first-class access to Ishizue's plane-slot model and hardware planes, and (b) is a trap because it imports all of Wayland's policy decisions through the bridge.

## 6. Flatpak and Snap

Flatpak and Snap sandbox applications. Both have to solve the same problem: a sandboxed app needs to talk to the display server without trusting it fully, and the display server needs to grant capabilities to the app piecemeal rather than all-or-nothing.

How it works today on Wayland. The Flatpak runtime bind-mounts the Wayland socket (`$XDG_RUNTIME_DIR/wayland-0`) into the sandbox's namespace [27]. The app connects to it as an ordinary Wayland client. The compositor does the trust check at connect time, usually via `SO_PEERCRED` and the cgroup of the connecting process, which is exactly the model Ishizue's allowlist uses [1, §6.3] [63]. Snap does the same via its confinement mechanism. For capabilities that should not be granted automatically, the freedesktop `xdg-desktop-portal` service provides a D-Bus API the app calls, the portal prompts the user, and on consent the portal instructs the compositor to grant the capability [24] [25]. The two canonical examples are screen sharing (the `ScreenCast` and `RemoteDesktop` portals, backed by PipeWire on the compositor side [26]) and file access (the `FileChooser` portal). Portals were designed for Flatpak but any app can use them [25].

How it works today on X11. The X11 model is broken for sandboxing, because any X11 client can read the global window tree, snoop on keystrokes, and inject events into other clients. Flatpak on X11 gives the app the whole X server. This is one of the reasons Wayland won the security argument.

What Ishizue needs. The spec already has the load-bearing pieces. The allowlist (`isz_allowlist_add_binary`, `isz_allowlist_add_cgroup`) is the connect-time trust check [1, §6.3] [63]. Per-client socket namespaces are an Architect concern: the Architect creates the listen socket, so it can create one socket per sandbox and pass different fds to different `isz_init` instances, or run multiple `isz_init` instances in the same process with different allowlists. The §6.11 portal-style consent for screen capture is already in the spec [1, §6.11] [64]: the library does the prompting itself rather than depending on an external portal daemon, which is a deliberate choice to avoid the D-Bus dependency. The capture API in §7.11 fails with `ISZ_ERR_ACCESS_DENIED` until consent is granted [1, §7.11].

What is missing for full Flatpak/Snap support. Two things. First, an `xdg-desktop-portal` backend implementation. Sandboxed apps expect to call `org.freedesktop.portal.ScreenCast` and friends over D-Bus. Ishizue does not provide a D-Bus API and should not, because D-Bus is policy infrastructure that belongs to the Architect. The right shape is a separate `ishizue-portal` process, allowlisted like the X11 bridge, that exposes the portal D-Bus interfaces and translates them into Ishizue wire-protocol requests. This mirrors the spec's separate-process pattern for the X11 bridge [1, §13] [61]. Second, the spec's consent model is per-request and per-capture [1, §6.11], which covers screen capture but not the longer portal list (file chooser, open URI, print, settings, wallpaper, secret, location, camera). Those are out of scope for a window-server library; they belong in a portal daemon that happens to use Ishizue for the screen-related ones. The split is clean: Ishizue does screen capture consent, the portal daemon does everything else, and the portal daemon calls into Ishizue for screen capture via the wire protocol.

What works today. The allowlist plus the X11 bridge gets Flatpak and Snap apps running via X11, with the same broken-sandbox caveat as X11 everywhere else. Native Ishizue Flatpak apps would need the portal daemon and the per-sandbox socket work, both of which are out of v1 scope but do not require spec changes.

## 7. Android apps via Waydroid

Waydroid runs Android apps on Linux by booting a full Android system in a container [28] [29]. It uses Linux namespaces (user, pid, uts, net, mount, ipc) for containment and shares the host kernel [28]. The Android `SurfaceFlinger` compositor runs inside the container and speaks Wayland to the host compositor [28] [30]. Waydroid only works in a Wayland session, because its compositor glue is a Wayland client [Waydroid forum/snippets]. In multi-window mode, each Android app appears as a host Wayland surface; in full-UI mode, the entire Android launcher runs as one surface [30].

Could Waydroid speak Ishizue instead? The Wayland dependency is in Waydroid's `platform` code, the piece that takes surfaces from `SurfaceFlinger` and pushes them to the host. Replacing that with an Ishizue client would mean writing an `libishizue-client`-based platform module for Waydroid. The surface itself is straightforward: Waydroid already produces DMA-BUFs from the Android side, and Ishizue takes DMA-BUFs directly [1, §8]. Input is straightforward: Ishizue's input events map cleanly onto Android's `MotionEvent` model, modulo the keysym translation the spec refuses to do [1, §9]. The non-obvious piece is multi-window mode, where Waydroid currently relies on Wayland's `xdg-shell` for window management. Ishizue does not do window management [1, §1]; the Architect does, via `isz_surface_set_zpos` and friends [1, §7.6]. Waydroid on Ishizue would either need to talk to the Architect's window-management IPC (if the Architect exposes one, which the spec says is the Architect's problem [1, §6.14]) or run in full-UI mode and let the Architect treat the whole Android launcher as one surface.

The work is modest in lines of code but depends on `libishizue-client` existing first (section 1). It is a v2 or v3 item, not v1. The X11 bridge does not help here, because Waydroid does not speak X11. If Ishizue v1 needs Android apps, the path is Waydroid-on-Weston-on-Xwayland-on-Ishizue, which is too many layers to recommend. The clean path is to wait for `libishizue-client` and then port the Waydroid platform module.

## 8. Web apps (PWAs, Electron, Tauri)

This section is short because the answer collapses onto section 5.

Electron is Chromium plus Node. On Linux, Chromium's rendering path goes through its Ozone platform abstraction, which has Wayland and X11 backends [31]. Electron recently switched to Wayland by default, with X11 still working via XWayland [31]. Electron does not use GTK directly. It uses Ozone, and Ozone-on-Wayland talks `libwayland-client`. So Electron's Ishizue story is the same as the Ozone story: either an Ozone Ishizue backend (analogous to the Ozone Wayland backend, several thousand lines), or the X11 backend via the Ishizue X11 bridge, or a Wayland bridge (section 2). For v1, the X11 path works.

Tauri uses `wry`, a cross-platform WebView library [32]. On Linux, `wry` depends on `webkit2gtk`, which depends on GTK [33]. So a Tauri app is a GTK app with a WebKitWebView inside. The Ishizue story is the GTK story from section 5: native GDK backend (option a), Wayland bridge (option b), or X11 bridge (option c). For v1, option c.

PWAs running in a browser follow the browser's backend. Firefox, Chrome, and Epiphany (GNOME Web) all have Wayland and X11 backends. Epiphany is GTK; Firefox and Chrome are Ozone-based. The same three options apply.

The general pattern: every web-app stack on Linux bottoms out at either Ozone (Chromium-derived), WebKitGTK (GTK-derived), or a native toolkit. None of them speaks a raw display protocol. So the web-app question reduces to: does Ishizue support Ozone, GTK, or some compatibility path? For v1 the answer is the X11 bridge. For v2 the answer is a native Ozone backend and a native GDK backend.

## 9. Terminal emulators

Terminals are interesting because each one has its own rendering path and its own windowing backend. There is no shared abstraction the way SDL covers games.

foot is Wayland-only. It was built for Wayland with no X11 backend [34] [35]. foot on Ishizue would need either a native Ishizue backend or the Wayland bridge. This is the one terminal in the list that the X11 bridge cannot help with.

alacritty uses `winit` and `glutin` for windowing, which give it both X11 and Wayland backends [36]. `WINIT_UNIX_BACKEND=x11` forces X11; `WINIT_UNIX_BACKEND=wayland` forces Wayland [36]. alacritty on Ishizue would work via the X11 bridge today, or via a future `winit` Ishizue backend. The `winit` maintainers have discussed defaulting to X11 on Wayland systems because the Wayland backend historically lagged on feature parity [36], which suggests that an Ishizue backend in `winit` would automatically cover alacritty, many Rust GUI experiments, and a chunk of the Rust game ecosystem.

kitty runs natively on both Wayland and X11, with its own platform code rather than going through a windowing abstraction [37]. It uses the GPU directly for rendering and speaks the host display protocol itself. A kitty Ishizue backend would be a few thousand lines inside kitty's own platform layer. The X11 backend works via the X11 bridge today.

wezterm runs on X11, Wayland, macOS, and Windows, with `enable_wayland` defaulting to true on Linux [38]. It has its own multiplexer (`mux`) and its own platform abstractions. Like kitty, a wezterm Ishizue backend would live inside wezterm's platform code.

gnome-terminal is a GTK app. It uses the GDK backend GTK picks at runtime. The GTK story from section 5 applies directly.

konsole is a KDE/Qt app. It uses the QtWayland or X11 QPA plugin. The Qt story from section 5 applies.

The pattern: the terminals split into three groups. The Wayland-only ones (foot) need either the Wayland bridge or a native Ishizue backend, with no X11 fallback. The cross-platform ones (alacritty, kitty, wezterm) have X11 backends and work via the X11 bridge today, with native Ishizue backends as a future win. The toolkit-backed ones (gnome-terminal, konsole) follow their toolkit. For v1, the X11 bridge covers all of them except foot. foot is small enough that a native Ishizue backend for it specifically is plausible as a v2 demo app: it proves the native client story, exercises the input and presentation-feedback paths, and gives the project a fast terminal people can actually use.

## 10. Games

Linux games come in three flavours: native Linux games (usually SDL-based), Windows games via Proton/Wine, and browser games.

Native Linux games. Most use SDL2 or SDL3. The Steam Linux Runtime (Sniper, Scout, Soldier) ships SDL3 and `sdl2-compat` so older SDL2 games run on SDL3 underneath [41]. An Ishizue-native game would either use the SDL Ishizue backend (section 4) or speak the wire protocol directly via `libishizue-client` (section 1). The minimum a game needs is window creation, Vulkan surface creation, input, and presentation feedback. Ishizue has all of those in the spec. The plane-slot model [1, §7.7] is interesting for games because a fullscreen game on a dedicated plane slot is zero-copy scanout, which is the lowest-latency path the hardware offers.

Proton and Wine. Proton is Valve's Wine fork bundled with DXVK and other patches [40]. DXVK translates DirectX 9, 10, and 11 calls to Vulkan [39]. DXVK 3.x is the current line [39]. The rendering path is: Windows game -> DXVK -> Vulkan -> Mesa driver -> DMA-BUF -> display server. Proton games historically ran under XWayland because Wine's Wayland driver was experimental, but Wine-Wayland has matured and pure-Wayland Proton is increasingly viable [search snippets]. For Ishizue, the path is: Proton game renders to a Vulkan swapchain, the swapchain's DMA-BUF gets attached to an Ishizue surface, the surface goes on a plane slot. If Wine uses XWayland, the path is Proton -> XWayland -> Ishizue X11 bridge -> Ishizue surface. If Wine uses its native Wayland driver, the path requires the Wayland bridge (section 2). For v1, the X11 bridge path works. For v2, a native Wine Ishizue driver would be the cleanest path, but Wine's driver model is not well-suited to adding new backends and this is probably not worth the effort unless Ishizue has measurable adoption.

Steam Runtime. The Steam Linux Runtime is a container (Sniper is the current one, based on a fixed Ubuntu baseline) that provides a stable library environment for games [41]. It does not affect the display protocol; the game inside the runtime still speaks SDL/Wayland/X11 to the host. From Ishizue's perspective, a Steam Runtime game is the same as any other game.

An Ishizue-native game gets direct hardware access: the plane-slot model and tearing control (`DRM_MODE_PAGE_FLIP_ASYNC`, `ISZ_COMMIT_ASYNC` [1, §7.2] [1, §7.3]) put games on a plane without a compositor in the way. This is what Gamescope offers on top of wlroots. The v1 recommendation is: games run via the X11 bridge, and the native-game story is a v2 or v3 item gated on `libishizue-client` and an SDL backend existing.

## 11. Remote desktop (RDP, VNC, SPICE)

Remote desktop on Linux is fragmented across at least three protocols and several implementations per protocol. This section maps the territory and then asks where Ishizue's remote-desktop story should live.

RDP. xrdp is the main X11-side option. It runs as a bridge between RDP clients and the X server, accepting connections on TCP 3389, presenting a login screen via `xrdp-sesman`, and starting an X session per connection [42] [43]. xrdp uses `X11rdp` or `xorgxrdp` as the X server module that draws into an RDP framebuffer instead of a real screen [43]. On the Wayland side, Weston has an RDP backend that uses FreeRDP for transport [45] [46]. GNOME has `gnome-remote-desktop`, which has both RDP and VNC backends and uses PipeWire to stream pixel content from the compositor [44]. `gnome-remote-desktop` runs inside the compositor process and uses PipeWire's `pipewire-stream` to get frames, then encodes them as RDP or VNC [44]. Microsoft's WSLg stack uses a forked Weston with RDP for WSL's graphical apps [search snippets].

VNC. VNC is older and simpler. On X11, `x11vnc` and `TigerVNC` server share an X server's framebuffer. On Wayland, `wayvnc` is a standalone VNC server that works with wlroots-based compositors via the `wlr-screencopy` protocol [45].

SPICE. SPICE is a remote computing solution primarily used for virtual machines. QEMU uses `spice-server` to provide remote access to VMs through the SPICE protocol [47]. QEMU emulates a virtual GPU (QXL or virtio-gpu), passes frames to the SPICE server, which streams them to a SPICE client [47] [48]. SPICE is not used for desktop sharing; it is a VM display protocol. It is relevant to Ishizue only if Ishizue ever runs as a VM display, which is out of scope.

What an Ishizue remote-desktop backend looks like. The spec's §13 separate-process pattern [61] is the right shape. A remote-desktop bridge is a process that: connects to Ishizue as a privileged client, requests screen capture via `isz_output_capture_start` [1, §7.11] [64], gets a DMA-BUF back per frame via `ISZ_MSG_CAPTURE_DONE` [3], encodes it (H.264, RDP's codecs, or raw), and serves it over RDP or VNC to a remote client. Input from the remote client becomes `ISZ_MSG_INPUT_*` events injected via the test hooks API [1, §4] [66] or a new privileged input-injection message. The consent model from §6.11 already covers the capture side [1, §6.11]. The bridge does not need any new library APIs; it uses the same primitives as every other client. This is the same architectural call the spec made for the X11 bridge [1, §13].

In the library or separate? Separate, for the same reasons the X11 bridge is separate [1, §13]. RDP and VNC are policy-laden protocols with codec choices, authentication, multi-session handling, and TLS configuration. Putting them in the library would import all of that. A separate `ishizue-rdp-bridge` binary, allowlisted by the Architect [1, §6.3] [63], keeps the library mechanism-only. The library's job is to expose capture and input injection; the bridge's job is to speak RDP.

The recommendation for v1 is to not ship a remote-desktop bridge. The capture API [1, §7.11] and the consent model [1, §6.11] are in v1, which is the load-bearing part. A bridge can be written by a third party once `libishizue-client` exists. If v1 needs remote desktop, the path is the X11 bridge plus xrdp, which works today for X11 apps.

## 12. Embedded and mobile

PostmarketOS is an Alpine-based Linux distribution for mobile devices, capable of running different X and Wayland based user interfaces [49] [50]. Its recommended UIs on devices with working GPU acceleration are Phosh, Plasma Mobile, and Sxmo [50]. Phosh is a phone shell based on the GNOME stack with Wayland as the display server [51]. Plasma Mobile is KDE's mobile shell, also on Wayland [52]. The mobile Linux world is Wayland-only; there is no X11 mobile stack in active use.

Could they use Ishizue? Mechanically yes. Ishizue targets Linux with DRM/KMS, libinput, and libseat [1, §3], which is what mobile kernels expose. The hard parts are the mobile-specific issues the spec covers or does not.

Touch input. The spec has `ISZ_EVENT_INPUT_TOUCH_DOWN`, `_MOTION`, `_UP`, `_FRAME` with touch id and absolute coordinates [1, §9]. Touch device calibration is exposed via `isz_seat_device_set_calibration` [1, §9]. This is enough for basic touch. What is missing is gesture recognition, which the spec correctly leaves to the Architect [1, §1] because gesture policy is policy. Phosh and Plasma Mobile have their own gesture handlers; they would run on top of Ishizue raw touch events.

On-screen keyboard. The spec does not mention input methods or on-screen keyboards. An OSK is just a layer-shell surface [1, §6.7] that the Architect positions at the bottom of the output and that sends synthesized key events. The library does not need to know it is an OSK. What is missing is a text-input protocol so focused apps can receive composed text rather than raw keycodes. Wayland has `zwp_text_input_manager_v3` for this. Ishizue would need an equivalent, or the Architect would need to handle text composition and inject composed text via a new message. This is a spec gap for mobile.

Rotation. The spec has `isz_surface_set_transform` covering `ROTATE_90`/`180`/`270`/`REFLECT_X`/`REFLECT_Y`, mapped to the KMS `rotation` property [1, §7.2]. This handles output rotation at the hardware level. The Architect decides when to rotate; the library applies it. This is sufficient.

DPI and scaling. The spec has per-plane scaling and filtering on overlay planes [1, §7.2] and `isz_surface_set_size` for logical-versus-buffer size [1, §7.6]. Fractional scaling is not mentioned. Wayland's `wp_fractional_scale_v1` is the modern answer. Ishizue would need either a fractional-scale message or an Architect-side convention. This is a spec gap for mobile and for high-DPI desktops.

The recommendation for v1 is that mobile stays out of scope. The spec is desktop-first. The gaps (text input, fractional scaling) are real and would need spec work before a mobile shell could run cleanly. postmarketOS and Plasma Mobile are not going to switch to Ishizue in v1; the value of mobile support is a v3 or v4 question once the desktop story is proven.

## 13. Kiosk and signage

Kiosk and signage deployments are the boring use case that quietly dominates unit volume: point-of-sale terminals, info displays, factory floor screens, museum exhibits, digital signage. The stacks in production are heterogeneous. Porteus Kiosk is a full Linux distribution tuned for kiosk use, booting into a locked-down browser [54]. Ubuntu Frame is a Mir-based display server for digital signage on Ubuntu Core [53]. Many deployments just run X11 with a fullscreen browser, or even direct framebuffer with no display server at all.

What Ishizue offers for kiosk is the spec's idle behaviour and the plane-slot model. The spec mandates 0% CPU when idle, driven purely by damage plus vblank events, with no `clock_nanosleep` anywhere [1, §5] [68]. A kiosk showing a static image uses zero CPU between content changes, which matters for thermal and power budgets on always-on hardware. The plane-slot model [1, §7.7] lets a kiosk put its single fullscreen surface on a primary plane and use hardware planes for overlays (a clock, a ticker) without GPU compositing. Hardware cursor planes [1, §7.4] mean cursor movement (for interactive kiosks) does not recomposite anything.

What Ishizue needs for kiosk that the spec does not already cover: not much. A kiosk is one client, one surface, one output. The allowlist [1, §6.3] handles the trust model (the kiosk binary is the only allowed client). The headless backend [1, §10] is not relevant; kiosks use real DRM. The crash-recovery feature [1, §12] is valuable for kiosks because it restores the VT and blanks CRTCs on crash, so a buggy kiosk app does not leave the screen in an undefined state.

The recommendation for v1 is that kiosk is a viable target once the DRM backend is solid. It is the simplest real-world deployment: no window management to speak of, no multi-client complexity, no portals. A kiosk Architect is a good v1 demo because it exercises the library without exercising the policy layer.

## 14. Headless and CI rendering

The use case is: run a GUI app in CI without a real display, capture frames, run tests. This is what Xvfb has done for X11 since the 1990s. Xvfb is a virtual framebuffer that implements the X11 display server protocol, performing all graphical operations in memory without a physical display [55]. Electron's own testing docs recommend Xvfb for headless CI [55]. WebdriverIO, Playwright, and Selenium all use Xvfb underneath for browser testing on Linux.

The Ishizue spec already has the pieces for this. The headless backend [1, §10] provides virtual outputs with no real DRM or GPU. The test hooks [1, §4] expose `isz_test_connect`, `isz_test_send_key`, `isz_test_send_pointer_motion`, `isz_test_simulate_output_hotplug` [1, §4] [66]. A client connecting to a headless Ishizue server is already what the test harness does. The capture API [1, §7.11] produces a DMA-BUF per frame, which a CI test can mmap and compare against a golden image.

What is missing for real-world CI use. Three things. First, a stable `libishizue-client` so test code can connect without re-implementing the wire protocol (section 1). The test hooks use an in-process `isz_test_client *` [1, §4] [66], which works for tests that link the library but not for tests that want to run a real app binary against the server. Second, a way to extract frames as PNG or raw RGB from the captured DMA-BUF without writing GPU code in the test harness. The library could ship a small helper that imports a DMA-BUF via GBM and reads it back, or the headless backend could offer a "capture to memfd" mode that hands back CPU-readable pixels directly. Third, a way to run the headless server without an Architect binary, so CI can do `ishizue-headless --width=1920 --height=1080 --socket=/tmp/isz-0` and then run apps against it. That means a minimal Architect that just calls `isz_init(ISZ_BACKEND_HEADLESS, ...)`, sets up the allowlist to accept the test client, and loops on `isz_dispatch`. The spec does not forbid this; it just does not provide it. A reference headless Architect is a small, useful v1 deliverable.

The recommendation for v1 is that the headless backend plus a reference headless Architect plus `libishizue-client` together form the CI story. Xvfb-equivalence is the bar. If a test framework can swap `Xvfb :99` for `ishizue-headless :99` and have browser tests still pass, that is success. This is plausible for v1 because the headless backend already exists [1, §10] and the test hooks already drive it [1, §4] [66].

## 15. Nested Ishizue

Nested compositing is running one compositor inside another. The outer compositor owns the hardware; the inner compositor is a client of the outer. Use cases: development (run a new compositor as a window inside your existing desktop so you do not have to drop to a TTY), sandboxing (run untrusted apps in an inner compositor that has a tighter policy than the outer), and multi-level compositing for VR (the VR compositor runs inside the desktop compositor, and apps run inside the VR compositor).

Wayland already has patterns for this. The Wayland documentation describes nested Wayland instances where the Wayland server is a library the host application links to, and the host application is a Wayland client of the outer compositor [56]. Sommelier is Chrome OS's nested Wayland compositor that delegates compositing to a host compositor [57]. wlroots has a Wayland backend (for running a wlroots compositor inside another Wayland compositor), an X11 backend (for running inside X11), and a headless backend (for testing) [59]. Sway can run nested in another Sway or in another Wayland compositor [search snippets]. Phoronix reported in 2015 on Wayland gaining a nested compositor backend that communicates to whatever compositor runs underneath [58].

SPEC §10 mentions a nested backend as a possible future backend, not required for v1, with the API shape fixed now (`ISZ_BACKEND_NESTED` taking an `isz_nested_config` with a parent window handle) [1, §10] [62]. SPEC §15 lists the nested backend implementation as explicitly deferred [1, §15]. The interface is designed to not preclude it.

Three nested scenarios for Ishizue.

Ishizue inside Ishizue. The inner Ishizue server runs as a client of the outer Ishizue server. The inner server creates one `isz_surface` on the outer server for each of its own outputs, attaches DMA-BUFs to those surfaces, and forwards input events from the outer to its own clients. The nested backend in §10 is exactly this: `ISZ_BACKEND_NESTED` with a parent window handle [1, §10] [62]. The implementation would link `libishizue-client` for the outer connection and use the same wire protocol for the inner clients. This is the cleanest nested case because both sides speak Ishizue.

Ishizue inside Wayland. The Ishizue server runs as a Wayland client of a host Wayland compositor. The nested backend's `parent_window_handle` would be a `wl_surface`. The implementation links `libwayland-client` for the outer connection and exposes the inner Ishizue protocol to inner clients. This is the development case: a developer running an Ishizue compositor as a window inside their GNOME or KDE session.

Ishizue inside X11. The same, with an X11 window as the parent. The implementation links Xlib or XCB for the outer connection. This case is less useful now that X11 is being deprecated, but it is the same pattern.

What the nested backend protocol would look like. The nested backend implements the `isz_backend_ops` interface (`init`, `commit`, `read_events`) [1, §10] [62] by translating each commit into an outer-server `ISZ_MSG_SURFACE_ATTACH_BUFFER` plus `ISZ_MSG_COMMIT`, and each outer input event into the corresponding `ISZ_MSG_INPUT_*`. The hard part is buffer sharing: the inner server's clients produce DMA-BUFs that have to be importable by the outer server's GPU. If both sides share the same GPU (the common case for Ishizue-inside-Ishizue and Ishizue-inside-Wayland on the same machine), the DMA-BUF fd is just passed through. If they do not (Ishizue inside a different-GPU host), the cross-GPU import path from §8 applies [1, §8].

The recommendation for v1 matches the spec: the API shape is fixed, the implementation is deferred [1, §10] [1, §15] [62]. Nested is a development convenience, not a v1 deliverable. The headless backend [1, §10] plus bare-metal DRM testing is the v1 dev path [1, §14]. Nested lands when someone needs it; the interface is ready.

## 16. Recommended client-stack priority for Ishizue v1

Based on the fifteen sections above, this is the concrete priority order for Ishizue v1, with justification.

1. Native protocol and `libishizue-client` first. The protocol is the bet the spec is making [1, §1]. It has to prove itself before compatibility layers obscure it. `libishizue-client` is the load-bearing dependency for everything else native: the X11 bridge already needs it (and currently inlines the protocol code to avoid it [60]), the portal daemon needs it (section 6), the remote-desktop bridge needs it (section 11), and every native backend for SDL, GLFW, Qt, GDK, and Wine needs it (sections 4, 5, 10). Shipping v1 without a client library means every native client re-implements the wire protocol. The client library should mirror `libwayland-client` in scope: connect, marshal, dispatch, fd-passing, event queues, and nothing more [5].

2. X11 bridge second. The X11 bridge is already scoped [1, §13] and scaffolded [60]. It gets real apps working: Qt and GTK via their X11 backends (section 5, option c), SDL and GLFW games via their X11 backends (section 4), Electron and Tauri via X11/Ozone (section 8), Proton games via XWayland (section 10), gnome-terminal and konsole (section 9). This is the single most useful piece of compatibility work because it covers the long tail of X11-capable apps with one bridge. The scaffold exists; the work is filling in the X11 protocol surface (visuals, replies, errors, extensions, buffer translation) [60].

3. Headless CI story third. The headless backend exists [1, §10] and the test hooks exist [1, §4] [66]. The gap is a reference headless Architect, a frame-capture helper that produces PNG or raw RGB from a DMA-BUF, and `libishizue-client` support for the same. This is small work and makes Ishizue testable by third parties, which matters for adoption. Xvfb-equivalence is the bar (section 14).

4. Kiosk as the first real-world deployment target. A kiosk is the simplest real-world Ishizue use case: one client, one surface, one output, no window management to speak of, no portals (section 13). It exercises the DRM backend, the idle-at-zero-CPU behaviour [1, §5] [68], the plane-slot model [1, §7.7], and crash recovery [1, §12] without exercising policy. A kiosk Architect is a good v1 demo and a plausible v1 production target.

5. Wayland compatibility explicitly deferred. Wayland compat stays out of v1, matching SPEC §2 [1, §2]. The X11 bridge covers the app tail. The marginal value of a Wayland bridge over an X11 bridge is small and shrinking as Electron and the major toolkits move to Wayland-by-default with X11 fallback (section 2). If a third party wants to write a Wayland bridge later, the spec already supports it: the bridge is just another privileged client [1, §13].

Beyond v1, the order is: native Qt QPA plugin and native GDK backend (section 5, option a), native SDL backend (section 4), native Ozone backend for Chromium/Electron (section 8), native foot backend as a fast terminal demo (section 9), remote-desktop bridge (section 11), Waydroid platform module (section 7), nested backend (section 15), mobile shell support with text-input and fractional-scale spec work (section 12). Each of these depends on `libishizue-client` existing, which is why item 1 of the v1 list is the priority.

The spec's mechanism-only stance [1, §1] and its separate-process pattern for compatibility [1, §13] [61] are what make this ordering work: the library stays small, the compatibility layers live outside it, and the Architect decides what runs.

## References

[1] Ishizue SPEC.md, this repository. §1 What This Is; §2 Non-Goals; §3 Target Platform; §4 Language and Engineering Practices; §5 Concurrency and Event Loop Model; §6 Client Protocol; §7 Rendering Pipeline; §8 Buffer and Memory Management; §9 Input Handling; §10 Backend Abstraction and Multi-GPU; §11 Hardware Fallback Matrix; §12 Fault Tolerance and Debugging; §13 X11 Compatibility; §14 Dev Workflow; §15 Open Items.

[3] Ishizue `doc/protocol.md`, this repository. Wire protocol reference: framing, handshake, FD passing, capabilities, message table, fault tolerance, queue depth, byte-exact examples.

[4] Drew DeVault, "The Wayland Protocol" book, "libwayland in depth" chapter. https://wayland-book.com/libwayland.html

[5] Wayland, "Appendix B. Client API." https://wayland.freedesktop.org/docs/html/apb.html

[6] Debian manpages, "wl_proxy(3) -- libwayland-doc." https://manpages.debian.org/experimental/libwayland-doc/wl_proxy.3.en.html

[7] Wayland, "Appendix C. Server API." https://wayland.freedesktop.org/docs/html/apc.html

[8] Wayland, "Architecture." https://wayland.freedesktop.org/architecture.html

[9] GNOME Wiki, "Initiatives/Wayland/Xwayland." https://wiki.gnome.org/Initiatives/Wayland/Xwayland.html

[10] ArchWiki, "Wayland." https://wiki.archlinux.org/title/Wayland

[11] Command Linux, "Wayland vs Xorg Adoption Trends Statistics." https://commandlinux.com/statistics/wayland-vs-xorg-adoption-trends

[12] TWiT, "The Great X11-to-Wayland Migration." https://twit.tv/posts/tech/great-x11-wayland-migration

[13] LWN.net, "Wayland starting to work." https://lwn.net/Articles/1057107

[14] SDL Wiki, "SDL3/README-wayland." https://wiki.libsdl.org/SDL3/README-wayland

[15] SDL Wiki, "SDL3/SDL_HINT_VIDEO_DRIVER." https://wiki.libsdl.org/SDL3/SDL_HINT_VIDEO_DRIVER

[16] Inner Computing, "X11/Wayland in SDL 3." https://www.innercomputing.com/blog/x11-wayland-in-sdl3

[17] Phoronix, "GLFW 3.4 Released." https://www.phoronix.com/news/GLFW-3.4-Released

[18] GLFW, "Compiling GLFW." https://www.glfw.org/docs/latest/compile.html

[19] Qt, "Wayland and Qt" (Qt 6.11). https://doc.qt.io/qt-6/wayland-and-qt.html

[20] Qt Wiki, "QtWayland." https://wiki.qt.io/QtWayland

[21] Qt, "Qt Platform Abstraction" (Qt 6.11). https://doc.qt.io/qt-6/qpa.html

[22] GTK, "Using GTK with Wayland" (GTK 3.0 docs). https://docs.gtk.org/gtk3/wayland.html

[23] GDK4 Wayland, "WaylandDisplay." https://docs.gtk.org/gdk4-wayland/class.WaylandDisplay.html

[24] Flatpak, "XDG Desktop Portal." https://flatpak.github.io/xdg-desktop-portal

[25] ArchWiki, "XDG Desktop Portal." https://wiki.archlinux.org/title/XDG_Desktop_Portal

[26] Jan Grulich, "How to enable and use screen sharing on Wayland." https://jgrulich.cz/2018/07/04/how-to-enable-and-use-screen-sharing-on-wayland

[27] Flatpak, "Sandbox Permissions." https://docs.flatpak.org/en/latest/sandbox-permissions.html

[28] Waydroid, "Android in a Linux container" (homepage). https://waydro.id

[29] ArchWiki, "Waydroid." https://wiki.archlinux.org/title/Waydroid

[30] LWN.net, "Android apps on Linux with Waydroid." https://lwn.net/Articles/901459

[31] Electron Blog, "Tech Talk: How Electron went Wayland-native, and what it means for your apps." https://electronjs.org/blog/tech-talk-wayland

[32] tauri-apps/wry, "Cross-platform WebView library in Rust." https://github.com/tauri-apps/wry

[33] tauri-apps/wry, Discussion #996, "wry on linux in the future." https://github.com/tauri-apps/wry/discussions/996

[34] Command Linux, "Linux Terminal Emulator Statistics." https://commandlinux.com/statistics/linux-terminal-emulator-popularity

[35] DEV Community, "State of Linux Terminal Emulators in 2026." https://dev.to/shrsv/state-of-linux-terminal-emulators-in-2026-1gh5

[36] rust-windowing/winit, Issue #305, "Consider defaulting to X11 when running in Wayland." https://github.com/rust-windowing/winit/issues/305

[37] Petronella Tech, "Kitty Terminal: GPU-Accelerated Terminal Guide." https://petronellatech.com/blog/kitty-terminal-setup-guide-2026

[38] WezTerm, "enable_wayland" config reference. https://wezterm.org/config/lua/config/enable_wayland.html

[39] DXVK, "Why use DXVK with Proton?" https://dxvk.org/why-use-dxvk-with-proton

[40] ValveSoftware/Proton, "Compatibility tool for Steam Play based on Wine." https://github.com/valvesoftware/proton

[41] ValveSoftware/steam-runtime, Issue #525, "Run native Linux games inside the sniper runtime inside CI." https://github.com/ValveSoftware/steam-runtime/issues/525

[42] xrdp.org, "xrdp by neutrinolabs." https://www.xrdp.org

[43] ArchWiki, "Xrdp." https://wiki.archlinux.org/title/Xrdp

[44] GNOME, "gnome-remote-desktop" (GitLab project page). https://gitlab.gnome.org/GNOME/gnome-remote-desktop

[45] Wayland, "FAQ." https://wayland.freedesktop.org/faq.html

[46] FreeRDP, Discussion #7212, "Freerdp server for wayland." https://github.com/FreeRDP/FreeRDP/discussions/7212

[47] spice-space.org, "Spice User Manual." https://www.spice-space.org/spice-user-manual.html

[48] Wikipedia, "Simple Protocol for Independent Computing Environments." https://en.wikipedia.org/wiki/Simple_Protocol_for_Independent_Computing_Environments

[49] postmarketOS, homepage. https://postmarketos.org

[50] Wikipedia, "PostmarketOS." https://en.wikipedia.org/wiki/PostmarketOS

[51] postmarketOS Wiki, "Phosh." https://wiki.postmarketos.org/wiki/Phosh

[52] LWN.net, "Plasma Mobile for highly configurable Linux phones." https://lwn.net/Articles/986899

[53] Ubuntu Discourse, "How to set up digital signage on Ubuntu Frame." https://discourse.ubuntu.com/t/how-to-set-up-digital-signage-on-ubuntu-frame/31068

[54] Porteus Kiosk, homepage. https://porteus-kiosk.org

[55] Electron Docs, "Testing on Headless CI Systems." https://electronjs.org/docs/latest/tutorial/testing-on-headless-ci

[56] Wayland, "Types of Compositors" (book). https://wayland.freedesktop.org/docs/book/Compositors.html

[57] Chromium OS platform2, "Sommelier" README. https://chromium.googlesource.com/chromiumos/platform2/+/eb0cd4d70bd65aa086865e4ffbdb1ca11070f56a/vm_tools/sommelier/README.md

[58] Phoronix, "Wayland Now Has A Nested Compositor Back-End." https://www.phoronix.com/news/ODgzNA

[59] swaywm/wlroots, "A modular Wayland compositor library." https://github.com/swaywm/wlroots

[60] Ishizue `x11bridge/README.md`, this repository.

[61] Ishizue SPEC.md §13, "X11 Compatibility (Lower Priority)" - separate-process pattern.

[62] Ishizue SPEC.md §10, "Backend Abstraction and Multi-GPU" - nested backend interface.

[63] Ishizue SPEC.md §6.3, "Client Trust and Allowlisting."

[64] Ishizue SPEC.md §6.11 and §7.11, screen capture consent and API.

[66] Ishizue SPEC.md §4, test hooks (`isz_test_connect`, `isz_test_send_key`, `isz_test_send_pointer_motion`, `isz_test_simulate_output_hotplug`).

[68] Ishizue SPEC.md §5, "Concurrency and Event Loop Model" (0% CPU when idle).
