# Ishizue (礎) - Window server library specification

**Status:** Planning / pre-implementation
**Scope:** v1, no phasing. Everything below is target-complete scope, not a roadmap.

## 1. What this is

Ishizue is a dynamic library (`.so`): a mechanism layer, analogous in role to wlroots, built around a fully custom, non-Wayland client protocol. There is no WM daemon and no bundled tiling logic.

- The library owns display output (DRM/KMS), input (libinput), buffer/GPU management, client protocol transport, and low-level primitives.
- The library does not own tiling logic, focus policy, hotkey bindings, stacking order policy, or any WM decision-making.
- The Architect is whoever links Ishizue and writes the actual tiling WM: same process, direct function calls, no IPC boundary between mechanism and policy. Ishizue is the foundation; the Architect is what's built on it.
- Ishizue is treated as a real public library from day one: semantic versioning, a stable ABI target, MIT/BSD-style license.

Governing philosophy: bare minimum, mechanism-only, zero unilateral policy decisions. At every point where the library could be tempted to "help" by making a decision (cursor fallback, memory eviction, crash recovery, plane sharing), it exposes the mechanism and leaves the decision to the Architect instead of making it silently.

## 2. Non-goals

Explicitly out of scope, not deferred:

| Excluded | Whose job it is |
|---|---|
| Surface stacking/z-order policy | Architect, via `isz_surface_set_zpos()` |
| Focus policy | Architect, via `isz_seat_set_keyboard_focus()` |
| Hotkey binding | Architect. Library only delivers raw keycodes |
| Tiling layout algorithm | Architect entirely |
| Render loop timer / idle polling | Nobody. No `clock_nanosleep()` anywhere; 0% CPU when idle, driven purely by damage + vblank events |
| Software cursor fallback | Nobody by default. Hardware cursor planes only; commit fails if unavailable, Architect decides what to do |
| Scripting/external control IPC (i3-ipc style) | Architect, if they want one |
| GPU compositing / plane sharing | Architect, via the plane-slot model (§7.7). The library never composites multiple surfaces into one plane |
| NVIDIA proprietary driver support | Out of scope entirely |
| Wayland protocol compatibility | Out of scope. Only X11 compat is planned (see §13) |

## 3. Target platform

- OS: Linux
- Display: direct to DRM/KMS, no compositor beneath Ishizue
- Input: libinput
- Seat/session management: libseat (abstracts logind/seatd/direct, not hard-coupled to systemd)
- GPU: Mesa-based only (Intel/AMD open-source drivers). NVIDIA explicitly unsupported.
- KMS mode: atomic KMS only. Hard requirement, not a fallback path; fails fast at backend-init with a clear error if atomic isn't available. No legacy KMS code path exists in the codebase.
- Multi-GPU: supported from day one (render-on-one-GPU / scanout-on-another offload scenarios), scoped to Mesa/Intel/AMD.

Minimum version dependencies: kernel 5.0+ recommended for stable atomic KMS, VRR, and HDR support across Intel/AMD (PSI itself only needs 4.20+, see §8). Mesa 20.0+ for stable `DRM_FORMAT_MOD_*` handling, though any Mesa version supporting the required formats works. These aren't build-time enforced; the library checks for feature support at runtime and fails fast if something required is missing.

Privilege requirements: the process linking Ishizue needs either `CAP_SYS_ADMIN`, root, or membership in the `video` group (distribution-dependent) to open `/dev/dri/card*` and `/dev/dri/renderD*`. The library does not attempt to drop or raise privileges itself; it assumes the Architect has already arranged the required capabilities (`setcap` on the executable, or a systemd unit with `CapabilityBoundingSet`, for example). The headless backend requires no privileges.

DRM master acquisition: on DRM backend init, the library calls `drmSetMaster()` on the primary DRM fd from libseat. If that fails, for example because another compositor already holds master, the library fails fast with `ISZ_ERR_DRM_MASTER` rather than attempting to steal master. On `ISZ_EVENT_SESSION_INACTIVE` (§9), the library releases master via `drmDropMaster()`; on `ISZ_EVENT_SESSION_ACTIVE`, it re-acquires it. If re-acquisition fails, `ISZ_ERR_DRM_MASTER` is surfaced to the Architect and all commits are blocked until it's resolved.

## 4. Language & engineering practices

| Aspect | Decision |
|---|---|
| Language | C |
| Build system | Plain Makefiles, no build system dependency |
| Keymap handling | libxkbcommon |
| Versioning | Semantic versioning, stable ABI target from early on |
| License | MIT License |
| Testing | Headless backend (virtual outputs, no real DRM/GPU) for integration tests, plus unit tests for isolated logic |
| API naming convention | `isz_` prefix on all public symbols |

Resource limits (build-time): the following are set via `-D` preprocessor macros at build time, not runtime, to avoid parsing overhead. Defaults are generic to the Mesa/Intel/AMD target class, not tuned to any single device:

- `ISZ_MAX_SURFACES_PER_CLIENT` (default: 64)
- `ISZ_MAX_CLIENTS` (default: 32)
- `ISZ_MAX_DMABUF_IMPORTS_TOTAL` (default: 256)
- `ISZ_THREAD_POOL_SIZE` (default: 4, adjust downward at build time on constrained hardware)
- `ISZ_MAX_EVENTS_PER_CLIENT` (default: 1024, see §6 event queue depth)

These limits are enforced strictly: exceeding one fails the relevant library call with `ISZ_ERR_RESOURCE_LIMIT`.

Optional feature flags (build-time): individual subsystems can be compiled out to shrink the binary and drop dependencies.

- `-DENABLE_HDR=1` (default 1): HDR metadata parsing and the KMS property path. Disabled, `isz_output_set_hdr_metadata()` returns `ISZ_ERR_FEATURE_UNAVAIL`.
- `-DENABLE_VRR=1` (default 1): VRR/Adaptive-Sync support. Disabled, VRR flags are ignored.
- `-DENABLE_THREAD_POOL=1` (default 1): the thread pool. Disabled, `isz_thread_pool_submit()` returns `ISZ_ERR_FEATURE_UNAVAIL` and offload work runs inline on the main thread.
- `-DENABLE_HEADLESS=1` (default 1): the headless backend. Disabled, `ISZ_BACKEND_HEADLESS` isn't available.

Symbol visibility: all public symbols are marked with `__attribute__((visibility("default")))`. A linker version script (`libishizue.map`) keeps only the `isz_*` API symbols visible and versions the ABI per semver major number. Internal functions are marked `static` or `__attribute__((visibility("hidden")))`, so the `.so`'s symbol table contains only the intended stable interface from day one.

ABI stability policy:
- Major version bump: breaking ABI changes (removed/renamed functions, changed struct layouts or signatures, removed enum values or error codes).
- Minor version bump: backward-compatible additions (new functions, new enum values appended at the end, new struct fields appended at the end and zero-initialized by the library).
- Patch version bump: bug fixes with no ABI-visible change.

An Architect built against `1.2.3` keeps working against any `1.x.y`. The version script enforces that only symbols belonging to the current major version are exported, and that new minor-version symbols are versioned correctly.

Build output: the library compiles to `libishizue.so.<major>.<minor>.<patch>` with a `libishizue.so` symlink pointing to it. The version is defined once in `version.h` and passed to the linker via `-Wl,-soname,libishizue.so.<major>`. `make install` copies the library to `$(DESTDIR)$(LIBDIR)` and headers to `$(DESTDIR)$(INCLUDEDIR)/ishizue/`; `make uninstall` reverses it.

Environment variables (debugging/diagnostics, read once at `isz_init()` and cached, not hot-reloadable):
- `ISZ_LOG_LEVEL=debug|info|warn|error`, forwarded to the Architect's logging callback.
- `ISZ_DRM_NODE=/dev/dri/card1`, overrides the primary DRM node instead of libseat's default (useful for testing specific GPUs on a multi-GPU box).
- `ISZ_HEADLESS_WIDTH`, `ISZ_HEADLESS_HEIGHT`, `ISZ_HEADLESS_REFRESH`, override headless backend defaults.
- `ISZ_DEBUG_SYNC=1`, verbose logging of dma-fence/syncobj waits, expensive, debugging only.

License headers: MIT License, standard text, every source file carries a copyright header with year and copyright holder (defaults to the project author, overridable per file for contributed code under a different holder).

Documentation: every public function needs a header comment formatted for man-page/HTML generation. The `doc/` directory holds `api.md` (function reference, usage patterns, examples), `protocol.md` (wire protocol: message IDs, object types, request/event formats), and `getting_started.md` (a minimal worked example of an Architect building a compositor with Ishizue). Incomplete documentation is treated as a bug, not a nice-to-have.

Test harness integration: built with `-DISHIZUE_ENABLE_TEST_HOOKS` (test builds only, stripped when `-DNDEBUG` is set for release), the library exposes:

```c
isz_test_client *isz_test_connect(isz_server *srv, const char *fake_binary_path);
void isz_test_send_key(isz_test_client *client, uint32_t keycode, bool press);
void isz_test_send_pointer_motion(isz_test_client *client, int x, int y);
void isz_test_simulate_output_hotplug(isz_server *srv, uint32_t width, uint32_t height);
```

These drive the headless backend (§10) for automated integration tests: simulated client connections, input injection, and output hotplug, without touching real hardware.

## 5. Concurrency & event loop model

The Architect owns the main event loop (their own epoll loop), not the library. The library exposes pollable fds and a `dispatch()`-style function; the Architect calls it each iteration. The library never blocks the caller and never runs its own loop.

Core model: single-threaded core event loop plus a thread pool for offload work only (texture upload, shader compilation, decode). Thread-pool work can be pinned to specific CPUs via an Architect-supplied `cpu_set_t`.

Internal data structures are lock-free where possible: surface/output/seat lists are treated as read-only during dispatch, and mutations happen only between commits, so there's no mutex contention on the hot path.

Thread pool queue: the thread pool's work queue is protected by a single mutex and condition variable (`pthread_mutex_t` + `pthread_cond_t`), not the lock-free scheme used for the core lists. The main thread pushes work and signals the condvar; worker threads block on the condvar when idle. This is deliberate: simplicity and correctness here outweigh the micro-optimization, and worker wake-up latency is dwarfed by the GPU work itself. The core event-loop data structures remain the only lock-free target.

Work cancellation: queued thread-pool jobs cannot be cancelled. The Architect must keep any object referenced by a submitted work function valid until its fence fd signals completion (pollable via `poll()`). Destroying an object while a thread-pool job still references it is undefined behavior.

Thread safety: the public API is not thread-safe. Every `isz_*()` function except `isz_thread_pool_submit()` must be called from the same thread that calls `isz_dispatch()`, the main event loop thread. Internal state isn't guarded with locks for these calls, since that would add overhead to the hot path for no benefit in the intended usage pattern. If the Architect needs to trigger a commit from a worker thread, they schedule it via `isz_thread_pool_submit()` with a work function that calls `isz_commit()` back on the main thread, or build their own message queue. The library does no automatic marshaling of API calls across threads.

Event listener API: events are identified by `enum isz_event_type` (see §9 for the full enumeration). Listeners have the signature:

```c
typedef void (*isz_event_listener_fn)(void *userdata, const isz_event *ev);

int isz_add_listener(isz_server *srv, enum isz_event_type type, isz_event_listener_fn fn, void *userdata);
int isz_remove_listener(isz_server *srv, enum isz_event_type type, isz_event_listener_fn fn, void *userdata);
```

Multiple listeners can attach to the same event type and are called in registration order. All listeners are called only from the main dispatch thread, so they don't need to be thread-safe themselves.

## 6. Client protocol

### 6.1 Transport and framing

- Transport: Unix domain socket plus a fully custom binary wire protocol (own message format, not Wayland's wire format).
- Socket lifecycle: the library does not create or bind the listening socket. The Architect passes a file descriptor (`int listen_fd`) to `isz_init()`, already bound and listening on the chosen UDS path. The library adds this fd to its internal epoll set, exposed to the Architect via `isz_get_fds()`. Socket path policy, permissions (`chmod 600`, for example), and `SO_PEERCRED` checks stay in the Architect's hands.
- Socket creation guidance: the Architect creates the socket, typically bound to `/tmp/.ishizue-<display>-<pid>` or a fixed well-known path. Permissions (`0600` for user-only, `0666` for world-accessible) depend on the intended trust model; `chmod`/`chown` are the Architect's responsibility. The socket is created before `isz_init()` and cleaned up after `isz_destroy()`; the library never removes the socket file itself, so the Architect must do so to avoid "address already in use" on restart.
- Message framing: every message is length-prefixed, a 32-bit length field followed by a message ID and payload. Fixed, predictable framing over anything more compact (varints, for example), because parsing reliability matters more than a few bytes of overhead here.
- Endianness: the wire protocol is explicitly little-endian. All multibyte fields (length, message ID, object IDs, coordinates) are transmitted little-endian regardless of host byte order, even though every currently-supported platform (x86_64, aarch64) is LE natively. Fixed explicitly to avoid ambiguity rather than left as an assumption.
- FD transmission: DMA-BUF file descriptors are passed as ancillary data (`SCM_RIGHTS`) alongside the wire message that references them. The message payload carries a 32-bit slot index (`fd_index`) mapping to the corresponding fd in the `cmsg` array. Fds are sent atomically with the message that references them.
- Protocol versioning: the wire protocol carries its own version number, separate from the library's ABI semver (§4). At connection handshake, the server reports its max supported protocol version, and clients negotiate down to the highest version both sides support.

### 6.2 Connection handshake

From the server's perspective:

1. Client connects to the Unix socket.
2. Server performs the `SO_PEERCRED` and cgroup checks (§6.3). On failure, the connection is closed immediately with no data sent.
3. Server sends an 8-byte magic header, `0x49535A48` (`"ISZH"`), followed by a 32-bit protocol version number. The magic header is fixed big-endian for reliable detection, independent of the little-endian convention used everywhere else in the protocol.
4. Client replies with its negotiated version (must be ≤ the server's max). A version greater than the server's max closes the connection.
5. Server sends `global` events for all current outputs and the active seat (§6.5).
6. Server sends a `capabilities` event, a bitmask covering `ISZ_CAP_HDR`, `ISZ_CAP_VRR`, `ISZ_CAP_TEARING`, `ISZ_CAP_SCREEN_CAPTURE`, and `ISZ_CAP_CURSOR_SIZE_MAX` (with max width/height as separate fields), so clients can degrade gracefully before attempting an unsupported feature. Capabilities are per-server, not per-output.
7. Server sends `handshake_done`, after which the client may send normal requests.

Any message received before `handshake_done`, other than the version reply, is a fatal protocol violation and the server disconnects.

### 6.3 Client trust and allowlisting

Client trust model: process/binary plus cgroup allowlisting. The server checks the connecting binary/cgroup against a known list before granting anything beyond baseline access (not trust-on-connect, not a runtime capability-request model).

Allowlist API, populated by the Architect before `isz_init()`:

```c
int isz_allowlist_add_binary(isz_server *srv, const char *path); // resolved to inode/dev at call time
int isz_allowlist_add_cgroup(isz_server *srv, const char *cgroup_path);
```

Entries are evaluated at `isz_init()` and cached. On connect, the library checks the peer's binary path (via `/proc/self/exe` of the peer) and cgroup membership; if neither matches, the client is disconnected immediately, before any handshake data is sent. An empty allowlist means deny-all: if the Architect never calls these functions, no clients can connect.

### 6.4 Object model

- Object IDs: the server owns object ID allocation. When the library creates an object (surface, buffer, seat proxy), it generates a unique 32-bit ID and returns it to the client in the response message. Clients refer to existing objects by these IDs in subsequent requests. IDs are per-connection, not global across the server.
- Surface serials: every surface creation also returns a serial, a 64-bit monotonic number unique per server lifetime, never reused after the surface is destroyed. Object IDs are per-connection; serials are global to the server. The serial is for cross-subsystem pairing. The X11 bridge (§13) can attach an X11 window XID to a surface via serial lookup rather than per-connection ID, and input events (§9) carry the serial of the surface they targeted so any subsystem can name a surface without holding the owning client's ID space.
- v1 object/feature set (full scope, not minimal): surface, buffer, output, seat (keyboard/pointer), popups/menus, drag-and-drop, clipboard, layer-shell equivalent (bars, panels, lock screens), screen capture, subsurfaces, cursor themes, surface roles (§6.17).

Rationale (surface serials, surface roles): research/wlroots-xwayland-patterns.md §5 (W6-C) documents the WL_SURFACE_ID race wlroots fixed via xwayland-shell-v1.set_serial; a native serial removes the need for that pairing dance. research/wlroots-xwayland-patterns.md §18 (W6-C, finding #2) recommends a native X11-surface role for direct XID pairing at surface creation.

### 6.5 Global objects (outputs and seat)

Output and seat objects are global singletons managed by the library, not per-client proxies with separate identities. On connect, the library sends `global` events for every currently-existing output and the active seat during handshake, and the client binds to those object IDs for subsequent requests. A hotplugged output (`ISZ_EVENT_OUTPUT_ADD` to the Architect, see §10) also triggers a `global` broadcast to all connected clients.

The library does not filter or hide outputs/seats per client; every client sees every output and the seat. Restricting which clients can access which outputs is Architect policy, enforced at the socket accept layer, not inside the library.

### 6.6 Subsurfaces

A subsurface is created with a parent reference (`isz_surface_create_subsurface(parent)`). Its position is relative to the parent's surface-local coordinates and it's clipped to the parent's bounds. A subsurface can be given its own plane slot (§7.7) independent of its parent, enabling zero-copy video overlays.

By default, subsurface commits are synchronized with the parent, meaning the parent's commit carries the subsurface's state along with it. A client can set `ISZ_SUBSURFACE_DESYNC` to decouple commit timing. Subsurface z-order is managed within the parent's stacking; the Architect sets the parent's zpos globally, and subsurfaces sort within that.

### 6.7 Popups and layer-shell

Popup: created with a parent surface and position offset (`isz_surface_create_popup(parent, x, y)`). The library clips it to output bounds on commit if needed. Popups grab keyboard and pointer input automatically until the client sends `popup_dismiss` to release the grab.

Layer-shell surface: created with an output reference and a layer type (`ISZ_LAYER_OVERLAY`, `ISZ_LAYER_BOTTOM`, `ISZ_LAYER_TOP`, `ISZ_LAYER_LOCK`). The library positions it relative to output edges using anchor flags and margins set by the client. Layer-shell surfaces sit outside the Architect's tiling logic entirely: the library stacks them above or below normal windows internally based on layer type, treating them as system surfaces.

### 6.8 Clipboard

Clipboard data transfers as a DMA-BUF (images) or a `memfd` (text and other small payloads), tagged with a MIME type. The owning client hands the library a `dmabuf_fd` or `memfd_fd`; when another client requests the clipboard, the library passes that fd via `SCM_RIGHTS` (the same mechanism as buffer transport) along with MIME type and size.

The library never parses, converts, or copies clipboard data; it only transfers the fd and MIME type atomically. Content filtering (allowing text but not images, for example) is Architect policy, implemented via a listener on `ISZ_EVENT_CLIPBOARD_REQUEST`.

Two selection slots per seat: PRIMARY (set by mouse-select, pasted by middle-click) and CLIPBOARD (set by Ctrl+C, pasted by Ctrl+V), mirroring the X11 convention. Each slot has its own owner and MIME list and is independent of the other; both use the same fd-passing mechanism described above. Whether the two are kept in sync, mirrored on focus change, or kept entirely separate is Architect policy. The library exposes them as two distinct slots: `isz_seat_set_selection_owner(seat, ISZ_SELECTION_PRIMARY, ...)` and `isz_seat_set_selection_owner(seat, ISZ_SELECTION_CLIPBOARD, ...)`.

Selection-ownership timestamps: every ownership change carries a `CLOCK_MONOTONIC_RAW` timestamp in nanoseconds. The library uses this to break ties when multiple clients claim ownership in quick succession: the latest timestamp wins; claims older than the current owner's timestamp are rejected. The timestamp is exposed to the Architect via `ISZ_EVENT_CLIPBOARD_REQUEST` so the Architect can reason about staleness. X11 selection timestamps are millisecond counters from an arbitrary epoch; the X11 bridge (§13) translates them to `CLOCK_MONOTONIC_RAW` on the way in.

Rationale (PRIMARY, selection timestamp): research/xwayland-architecture.md §7 (W6-A) notes X11 has both PRIMARY and CLIPBOARD selections and the original §6.8 described only one clipboard channel. research/wlroots-xwayland-patterns.md §12 (W6-C) and §18 (W6-C, finding #5) note wlroots treats X11 timestamps and Wayland serials as opaque and unrelated; a first-class timestamp lets the bridge translate cleanly and lets the Architect reject stale claims.

### 6.9 Drag-and-drop

A client starts a drag with `drag_start`, providing a source surface, an optional icon surface, and the MIME types on offer. The library tracks drag state and delivers `drag_motion` events to surfaces under the pointer. The surface under the pointer responds with `drag_accept` or `drag_reject`. On pointer release, the library sends `drag_drop` to the accepting target, followed by a data transfer using the same fd-passing mechanism as the clipboard (§6.8).

The library implements no visual feedback (drop-target highlighting, for example); that's the target client's own rendering responsibility. The Architect doesn't need to touch drag events at all unless they want to enforce policy, like forbidding drops on certain surfaces.

### 6.10 Cursor themes

The library does not include or parse cursor theme files (Xcursor and similar). The Architect loads cursor images from a theme (via `libxcursor`, for instance) and hands them to the library as ordinary surfaces:

```c
int isz_seat_set_cursor_surface(isz_seat *seat, isz_surface *surf);
int isz_seat_set_cursor_hotspot(isz_seat *seat, uint32_t x, uint32_t y);
int isz_seat_set_cursor_visible(isz_seat *seat, bool visible);
```

The library doesn't animate cursors or handle theme fallback; that's the Architect's job.

### 6.11 Portal consent

Portal-style consent: an explicit per-request user grant, with a timeout, even though Ishizue itself does the prompting (no external portal daemon dependency implied). The mechanism is the same per-request grant for every consent type; only the scope differs:

- Screen capture (see §7.11 for the capture API and its interaction with consent).
- File access, for sandboxed (flatpak-style) applications that need to read or write files outside their namespace.
- Notification forwarding, for sandboxed applications that need to surface notifications through the Architect.
- Any other portal-style consent the Architect chooses to route through this mechanism.

For each request, the library emits a consent-request event; the Architect prompts the user and responds with grant or deny. The library does not decide what counts as a grantable request; that is Architect policy. A separate `ishizue-portal` process, allowlisted like the X11 bridge (§13), is the expected shape for full xdg-desktop-portal compatibility; Ishizue itself only owns the consent primitive.

Rationale: research/client-stack-ecosystem.md §6 (W6-E) notes the original §6.11 covered screen capture only and that file access, notification forwarding, and other portal-style consent fit the same per-request user-grant mechanism.

### 6.12 Fault tolerance and disconnection

Fault tolerance on the wire: lenient. The server tries to tolerate or recover from bad client data where possible, and disconnects only on clearly fatal protocol violations.

Disconnect cleanup, whether via a clean `close()` or an abrupt broken pipe:

1. All surfaces belonging to the client are removed from any outputs and plane slots they held.
2. All buffers imported by the client are destroyed.
3. Any KMS plane-slot assignments held by those surfaces are released.
4. `ISZ_EVENT_CLIENT_DISCONNECT` is sent to the Architect once cleanup completes.
5. If the disconnected client held keyboard or pointer focus, the library does not reassign it automatically; the Architect receives the disconnect event and decides, via `isz_seat_set_keyboard_focus(NULL)` or reassignment to another surface. Automatic reassignment would be policy.

### 6.13 Event queue depth

The library uses `SO_SNDBUF` (default 262144 bytes) for each client socket. If a client isn't reading fast enough and the send buffer fills, the library queues outgoing events internally, up to `ISZ_MAX_EVENTS_PER_CLIENT` (default 1024, §4). Exceeding that queue disconnects the client with `ISZ_ERR_CLIENT_TOO_SLOW` rather than risking unbounded memory growth.

### 6.14 No separate scripting IPC

Anything like i3-ipc/sway-ipc for status bars or scripts is the Architect's problem, not part of this library.

### 6.15 Idle inhibit

The library exposes a per-surface idle-inhibit flag:

```c
int isz_surface_set_idle_inhibit(isz_surface *surf, bool inhibit);
```

For each output, the library tracks whether any surface on that output has the flag set. When the count goes from zero to one, the library emits `ISZ_EVENT_IDLE_INHIBIT_ACTIVE` for that output; when it returns to zero, the library emits `ISZ_EVENT_IDLE_INHIBIT_INACTIVE`. The library itself does not touch any idle timer, screensaver, or DPMS state. What the Architect does on `ACTIVE` or `INACTIVE` (disable a screensaver timer, inhibit DPMS-off, ignore the event) is policy and stays with the Architect. The library makes no decision about whether to honor an inhibit request from an unfocused or offscreen surface; if the flag is set, the flag is set.

Rationale: research/xwayland-architecture.md §12 (W6-A) notes the X11 `XScreenSaverSuspend` protocol does not work under rootless Xwayland and that Wayland's `zwp_idle_inhibit_manager_v1` is the equivalent; the original SPEC had no idle-inhibit mechanism.

### 6.16 Text input and input methods

Mobile on-screen keyboards, CJK input methods, and accessibility input methods need to exchange composition state with focused clients: preedit text (intermediate composition), commit text (final input), cursor position, and surrounding text retrieval. The library exposes a per-seat text-input object on the client side and a per-seat input-method object on the IME side.

Client side:

```c
isz_text_input *isz_seat_create_text_input(isz_seat *seat);
int isz_text_input_set_surrounding_text(isz_text_input *ti, const char *text,
                                        uint32_t cursor, uint32_t anchor);
int isz_text_input_set_content_type(isz_text_input *ti, enum isz_text_content_type hint);
int isz_text_input_set_cursor_rectangle(isz_text_input *ti, int32_t x, int32_t y,
                                        int32_t width, int32_t height);
int isz_text_input_enable(isz_text_input *ti);
int isz_text_input_disable(isz_text_input *ti);
int isz_text_input_commit_string(isz_text_input *ti, const char *text);
int isz_text_input_preedit_string(isz_text_input *ti, const char *text,
                                  int32_t cursor_begin, int32_t cursor_end);
```

IME side:

```c
isz_input_method *isz_seat_create_input_method(isz_seat *seat);
```

The library routes preedit, commit, and cursor-rectangle state between the active input method and the focused text-input. When a text-input is enabled on the focused surface, the library emits `ISZ_EVENT_TEXT_INPUT_PREEDIT` (carrying preedit text and cursor range), `ISZ_EVENT_TEXT_INPUT_COMMIT` (carrying committed text), and `ISZ_EVENT_TEXT_INPUT_CURSOR_RECTANGLE_NEEDED` (when the IME asks for the cursor rectangle, the library forwards the focused surface's cursor rectangle back to the IME). The library does no text composition itself; composition is the IME's job. Routing policy (which IME gets the seat, when to enable vs. disable, what to do when no IME is connected) is the Architect's, mediated by the enable and disable calls.

This is the Ishizue API shape, not a Wayland text-input-v3 or input-method-v2 binding. The two Wayland protocols are cross-referenced as design precedent; the Ishizue API owns its own message format and state machine.

Rationale: research/client-stack-ecosystem.md §12 (W6-E) identifies on-screen keyboards and CJK IMEs as a spec gap for mobile and accessibility, citing Wayland's `zwp_text_input_manager_v3` as the precedent; the original SPEC had no text-input mechanism.

### 6.17 Surface roles

A surface has an optional role attached at creation time. The role tells the library what kind of surface it is and what additional handle it carries. Roles:

- `ISZ_SURFACE_ROLE_NORMAL`: default, no additional handle.
- `ISZ_SURFACE_ROLE_X11_TOPLEVEL`: the surface represents an X11 top-level window. `role_handle` is the X11 window XID.
- `ISZ_SURFACE_ROLE_X11_POPUP`: the surface represents an X11 override-redirect popup. `role_handle` is the X11 window XID.
- `ISZ_SURFACE_ROLE_LAYER`: the surface is a layer-shell surface (§6.7). `role_handle` is the layer enum value.

```c
int isz_surface_set_role(isz_surface *surf, enum isz_surface_role role, uint64_t role_handle);
```

The role is set once at surface creation by the client that owns the surface. The X11 bridge (§13) sets `ISZ_SURFACE_ROLE_X11_TOPLEVEL` with the X11 window XID as the role handle, so the library can pair wire-protocol surface IDs with X11 window XIDs at creation time, without a serial-lookup round-trip or an association race. Creating an X11-role surface is a privileged operation gated by the allowlist (§6.3); only the bridge binary may do it. The surface serial (§6.4) remains available as a general cross-subsystem identifier; the role is the X11-specific shortcut.

Rationale: research/wlroots-xwayland-patterns.md §18 (W6-C, finding #2) recommends a native `x11_toplevel` surface role in the protocol so the X11 bridge attaches the XID at creation time, eliminating the WL_SURFACE_ID-vs-WL_SURFACE_SERIAL association race wlroots had to fix with xwayland-shell-v1.

## 7. Rendering pipeline

### 7.1 Graphics API
OpenGL ES first. Vulkan is a later consideration, only pursued if GLES proves insufficient for the latency target.

### 7.2 Atomic KMS specifics
| Feature | Behavior |
|---|---|
| Per-CRTC independent commits | Library manages multiple `drmModeAtomicCommit()` calls per output, each with independent timing. No global sync waits across outputs. |
| VRR / Adaptive-Sync | Library exposes a per-CRTC enable flag (`DRM_MODE_ATOMIC_ALLOW_MODESET` plus VRR props). The Architect decides when to use it, not the library. |
| HDR metadata | Library parses EDID `HDR_STATIC_METADATA` and hands the Architect a blob. Pure pass-through; the library never tonemaps. |
| Color management | KMS properties for `DEGAMMA_LUT`, `CTM`, `GAMMA_LUT`. Library handles blob creation/update mechanics only, zero CPU-side color conversion. |
| Per-plane scaling/filtering | Hardware bilinear/nearest-neighbor scaling on overlay planes, exposed directly, avoiding GPU shader cost for simple scaling. |
| Tearing control | `DRM_MODE_PAGE_FLIP_ASYNC` exposed per output; the Architect decides per-surface whether tearing is acceptable. |
| Writeback connectors | Treated as a regular output. The Architect attaches a dmabuf, the library commits, and the capture output lands directly in that dmabuf. This is the underlying mechanism for screen capture, see §7.11. |
| Transforms | `isz_surface_set_transform(surf, enum isz_transform)`, where the enum covers `ISZ_TRANSFORM_NORMAL`, `ROTATE_90`, `ROTATE_180`, `ROTATE_270`, `REFLECT_X`, `REFLECT_Y`. Maps to the KMS `rotation` property on the surface's assigned plane. If the plane doesn't support the requested transform, `isz_commit()` fails with `ISZ_ERR_TRANSFORM_UNSUPPORTED`; no software rotation fallback. |

EDID handling: the library reads the raw EDID blob from the DRM connector and validates its checksum, but only parses `HDR_STATIC_METADATA` out of it. Every other field (modes, physical dimensions, serial numbers) is exposed to the Architect as raw opaque bytes via `isz_output_get_edid()`. The Architect parses modes directly, or calls the library's `isz_output_get_modes()` helper, which wraps `drmModeGetConnector` but does not interpret EDID beyond that.

Color management API:

```c
int isz_output_set_gamma(isz_output *out, const uint16_t *red, const uint16_t *green, const uint16_t *blue, size_t size);
int isz_output_set_degamma(isz_output *out, const uint16_t *red, const uint16_t *green, const uint16_t *blue, size_t size);
int isz_output_set_ctm(isz_output *out, const float matrix[9]); // maps to the KMS CTM property
```

LUT sizes are validated against the hardware's advertised `DEGAMMA_LUT_SIZE`/`GAMMA_LUT_SIZE`; a mismatch fails with `ISZ_ERR_INVALID_ARG`. The library owns the blob's lifetime once set; these settings persist across commits.

HDR metadata API:

```c
int isz_output_set_hdr_metadata(isz_output *out, const isz_hdr_metadata *meta);
```

`isz_hdr_metadata` holds either the EDID-parsed `HDR_STATIC_METADATA` blob or an Architect-supplied override. The library serializes it into the KMS `HDR_OUTPUT_METADATA` property; it can be called at any time and takes effect on the next commit.

Fractional scale: integer per-plane scaling (above) does not cover HiDPI outputs where the correct factor is fractional (1.5x, 1.75x). Surfaces request a fractional scale via:

```c
int isz_surface_set_scale(isz_surface *surf, uint32_t numerator, uint32_t denominator);
```

The library passes the preferred scale to the client via a `preferred_scale` event so the client renders at the right resolution. The library itself does not composite or rescale; the client owns rendering at the requested scale, and the buffer is scanned out at its native resolution. Cross-reference: Wayland's `wp_fractional_scale_v1`.

Rationale: research/client-stack-ecosystem.md §12 (W6-E) identifies fractional scaling as a spec gap for HiDPI desktops and mobile, citing `wp_fractional_scale_v1`; the original §7.2 had integer per-plane scaling only.

### 7.3 Frame scheduling
Damage-tracked, event-driven, never a fixed redraw rate. Detailed scheduling/damage-coalescing implementation is internal to the library and not an Architect-facing policy knob beyond what's listed here.

Damage is coalesced per output: all surface damage targeting a given CRTC is collected and committed as a single atomic commit per refresh cycle, not per-surface commits.

The vblank fd is exposed in the Architect's epoll set; the Architect chooses whether to commit on vblank or immediately, if tearing is allowed for that surface/output.

Commit flags, passed to `isz_commit()`:
- `ISZ_COMMIT_NORMAL` (0, default): commit as soon as possible, waiting for the next vblank if the output isn't currently blanking.
- `ISZ_COMMIT_ASYNC`: uses `DRM_MODE_PAGE_FLIP_ASYNC` to commit immediately, bypassing vblank. Only honored if tearing is enabled for the output (§7.2); otherwise treated as `ISZ_COMMIT_NORMAL`.
- `ISZ_COMMIT_TEST_ONLY`: a test commit (§12), no visible frame produced.

The library provides no internal timer for scheduling commits at a specific time; the Architect uses their own timerfd in their epoll set for that.

Client presentation feedback: when a surface is successfully scanned out on an output, the library sends a `presented` event to that surface's client, carrying the `CLOCK_MONOTONIC` timestamp of the actual vblank where the frame appeared on screen. Clients use this to pace rendering and avoid producing frames faster than the refresh rate.

### 7.4 Cursor
Hardware cursor planes only (KMS cursor/overlay planes), for zero recomposite cost on cursor movement.

There is no software cursor fallback. If a cursor plane is unavailable on a given output, the relevant commit fails and it's the Architect's problem to handle, for example by building a software cursor on top using the general surface/plane-slot primitives.

### 7.5 Buffer synchronization
Explicit sync (dma-fence / `drm_syncobj`-based, equivalent to modern `linux-drm-syncobj`), chosen for multi-GPU correctness and lowest latency, accepting the added implementation complexity over implicit sync.

Cross-GPU sync: the render-GPU's fence is attached to the display-GPU's atomic commit using `drm_syncobj` and `sync_file`, so the display GPU correctly waits on the render GPU's completion.

### 7.6 Minimal API skeleton

Not exhaustive, just enough to fix the shape of the API and its ownership rules before implementation starts.

```c
// Lifecycle
isz_server *isz_init(isz_backend_type backend, void *backend_config); // config cast per backend type, NULL for DRM
void        isz_dispatch(isz_server *srv);   // non-blocking, one iteration
int         isz_get_fds(isz_server *srv, int *fds, size_t max);
void        isz_destroy(isz_server *srv);    // disables outputs, disconnects clients, tears down thread pool, frees everything

// Outputs
isz_output **isz_output_list(isz_server *srv, size_t *count);
isz_mode   **isz_output_get_modes(isz_output *out, size_t *count); // EDID-derived modes + refresh rates
int          isz_output_enable(isz_output *out, isz_mode *mode);
int          isz_output_disable(isz_output *out);
void         isz_output_destroy(isz_output *out);

// Surfaces
isz_surface *isz_surface_create(isz_server *srv);
void         isz_surface_destroy(isz_surface *surf);
int          isz_surface_attach_buffer(isz_surface *surf, int dmabuf_fd, isz_buffer_desc *desc); // library takes ownership of dmabuf_fd
int          isz_surface_detach_buffer(isz_surface *surf);
int          isz_surface_damage(isz_surface *surf, isz_rect *rects, size_t count); // surface-local coords, list required
int          isz_surface_set_output(isz_surface *surf, isz_output *out);
int          isz_surface_clear_output(isz_surface *surf);
int          isz_surface_set_position(isz_surface *surf, int x, int y); // output-relative
int          isz_surface_set_size(isz_surface *surf, int width, int height); // output-relative, overrides buffer size for scaling
int          isz_surface_set_plane_type(isz_surface *surf, enum isz_plane_type type); // mandatory per surface, no library default (7.7)
int          isz_surface_set_plane_slot(isz_surface *surf, int slot); // explicit plane-slot assignment (7.7)
int          isz_surface_set_zpos(isz_surface *surf, int zpos);

// Seat / input
int          isz_seat_set_keyboard_focus(isz_seat *seat, isz_surface *surf);
int          isz_seat_device_set_calibration(isz_seat_device *dev, float matrix[9]);

// Commit
int          isz_commit(isz_output *out, uint32_t flags); // flags: ISZ_COMMIT_NORMAL / ASYNC / TEST_ONLY (7.3)

// Thread pool
fence_fd     isz_thread_pool_submit(isz_server *srv, isz_work_fn fn, void *ctx); // Architect polls the returned fence fd
```

Error handling: functions returning `int` use `0` for success and negative `ISZ_ERR_*` codes for failure (§7.10). Ownership: `dmabuf_fd` passed to `isz_surface_attach_buffer()` becomes library-owned; the Architect must not close it, and the library `dup()`s internally if needed. The Architect never frees library-owned handles (`isz_surface *`, `isz_output *`, and so on) except through the matching `isz_*_destroy()`.

### 7.7 Plane assignment: the slot model

Ishizue surfaces are not directly backed by KMS planes. Instead, the library manages the output's KMS planes as an explicit resource pool, and the Architect assigns surfaces to plane slots by hand.

- `isz_output_get_plane_slots(out, &count)` returns the available plane slots for that output: type (primary/overlay/cursor), format support, scaling/transform capability, and zpos range.
- `isz_surface_set_plane_type(surf, type)` remains mandatory for every surface (no library-side default), same as before.
- `isz_surface_set_plane_slot(surf, slot)` explicitly assigns a surface to one of those slots. This is optional at the API level but load-bearing at commit time: if a surface has no slot assigned, `isz_commit()` fails with `ISZ_ERR_SURFACE_NO_PLANE_SLOT`. It is never scanned out silently and never gets a default slot.

This means the number of surfaces visible at once on a given output is capped by the number of hardware planes, unless the Architect does something about it, and that something is entirely their choice:

| Strategy | What it looks like |
|---|---|
| Dedicated planes | One surface per plane slot. Simplest, best performance, capped by hardware plane count (commonly 2 to 4 planes plus cursor on typical Intel/AMD hardware). |
| Architect-side software composition | When surface count exceeds plane count, the Architect creates a composition target (below), renders multiple window surfaces into it themselves, and attaches that single buffer to one plane slot. The library is not involved in the composition itself. |
| Hybrid | Some surfaces get dedicated slots, others get pre-composited into a shared slot, mixed per the Architect's own logic. |

When planes run out, the library doesn't switch to software composition, pick which surfaces get demoted, or implement composition itself. It only provides the primitives needed to make Architect-side composition tractable:

```c
int isz_composition_target_create(isz_server *srv, uint32_t width, uint32_t height, uint32_t format, int *dmabuf_fd_out);
int isz_composition_target_get_egl_image(int dmabuf_fd, isz_buffer_desc *desc, void **egl_image_out);
```

The composition target is just a DMA-BUF the Architect can render into by whatever means they choose (their own GLES pass using the provided EGL image, a software blit, or anything else), and then attach to a surface exactly like any client-provided buffer.

### 7.8 Cursor plane properties

At output-init, the library reads the KMS cursor plane's capability properties (`CURSOR_WIDTH`, `CURSOR_HEIGHT`, `HOT_X`/`HOT_Y`, supported formats) and exposes them to the Architect as limits. It does not silently clamp or reformat cursor buffers to fit.

### 7.9 Damage granularity and coordinate space

Damage is a list of rectangles, not a single bounding box, per surface per commit. Bounding-box-only damage forces a full-region recomposite on any multi-region update, which defeats the point of damage tracking on low-end hardware.

Damage rectangles are in surface-local coordinates, relative to the surface's own (0,0) origin, not buffer or output coordinates, the same coordinate system used for input events (§9). This lets the library apply transformations (scaling, rotation) without the client needing to know the output resolution. Rectangles are integer pixel coordinates using the standard `x1, y1, x2, y2` convention: inclusive of the left/top edge, exclusive of the right/bottom.

### 7.10 Error codes

All public functions return `0` on success. On failure, one of:

- `ISZ_ERR_COMMIT_FAILED`: atomic KMS commit failed, state rolled back.
- `ISZ_ERR_COMMIT_PENDING`: `isz_commit()` called while a previous commit on that output is still in flight.
- `ISZ_ERR_RESOURCE_LIMIT`: a build-time limit (§4) exceeded.
- `ISZ_ERR_SURFACE_NO_PLANE_SLOT`: commit attempted on a surface with no plane slot assigned (§7.7).
- `ISZ_ERR_PLANE_UNAVAIL`: requested plane type unavailable on the target output.
- `ISZ_ERR_TRANSFORM_UNSUPPORTED`: requested rotation/reflection unsupported by the assigned plane.
- `ISZ_ERR_OUTPUT_DISCONNECTED`: commit attempted on a removed output.
- `ISZ_ERR_INVALID_DMABUF`: DMA-BUF fd invalid or unsupported format/modifier.
- `ISZ_ERR_CLIENT_DISCONNECTED`: operation attempted on a surface whose client has disconnected.
- `ISZ_ERR_INVALID_ARG`: generic bad parameter.
- `ISZ_ERR_FEATURE_UNAVAIL`: requested feature not built in or not available on this system.
- `ISZ_ERR_NO_MEMORY`: allocation failure.
- `ISZ_ERR_DRM_MASTER`: DRM master acquisition failed (§3).
- `ISZ_ERR_CLIENT_TOO_SLOW`: client's event queue exceeded its depth limit (§6.13).
- `ISZ_ERR_ACCESS_DENIED`: operation requires consent that wasn't granted (screen capture, §7.11).

### 7.11 Screen capture

```c
int isz_output_capture_start(isz_output *out, int dmabuf_fd, isz_buffer_desc *desc);
int isz_output_capture_stop(isz_output *out);
```

The library treats the writeback connector as a separate CRTC enabled with a mode matching the output being captured. `capture_start` programs the writeback connector to write the composited frame into the provided dmabuf; a commit that includes both the display output and the writeback output produces the frame in both places. The dmabuf is library-owned until `capture_stop`, at which point the client gets it back via a `capture_done` event.

Portal consent (§6.11) must be granted before `capture_start` succeeds; without it, the call fails with `ISZ_ERR_ACCESS_DENIED`.

## 8. Buffer & memory management

- Primary transport: DMA-BUF only for v1 (zero-copy, GPU-to-GPU). SHM is not implemented as a parallel path in v1; see §11 for the fallback behavior when DMA-BUF import fails.
- Buffer release notification: for every imported buffer attached to a surface, the library tracks the dma-fence of the KMS commit that consumes it. Once that fence signals completion and the buffer is no longer referenced by any in-flight KMS commit, the library sends a `release` event to the client over the wire protocol. The client must not reuse or mutate the buffer's backing storage until this event arrives. Release event wire format: message ID `ISZ_MSG_RELEASE`, payload is the 32-bit `buffer_id`. If the client has already destroyed the buffer before the event arrives, the release is a silent no-op server-side.
- Buffer attach semantics: the `dmabuf_fd` passed to `isz_surface_attach_buffer()` is library-owned after the call, and the Architect must not close it. If a surface already has an unreleased buffer attached and a new one is attached, the library keeps the old buffer referenced until its fence signals, swaps in the new buffer for the next commit, and only then sends `release` for the old one. Up to 2 in-flight buffers per surface are tracked (double-buffering); a client can attach a new buffer every frame.
- Surface geometry: a surface's logical size comes from the buffer attached to it, taken from `isz_buffer_desc`'s width/height. There's no separate resize call; resizing is implicit in buffer attachment. To display a surface at a different size than its buffer, the Architect uses per-plane scaling (§7.2) together with `isz_surface_set_size()` (§7.6).
- Buffer descriptor:

```c
typedef struct {
    uint32_t width;          // pixels
    uint32_t height;         // pixels
    uint32_t stride;         // bytes
    uint32_t offset;         // bytes from start of dmabuf
    uint32_t format;         // DRM_FORMAT_* fourcc
    uint64_t modifier;       // DRM_FORMAT_MOD_* (or DRM_FORMAT_MOD_INVALID for implicit)
    uint8_t  alpha_mode;     // ISZ_ALPHA_NONE, ISZ_ALPHA_PREMULTIPLIED, ISZ_ALPHA_NON_PREMULTIPLIED
} isz_buffer_desc;
```

`alpha_mode` only matters if the surface sits on a plane that supports per-pixel alpha; overlay planes generally don't, and the field is ignored there.

- Cache-coherency tracking: the library tracks `dma_buf_attachment` and `dma_resv` fences internally. No CPU-side sync unless the Architect explicitly requests it.
- Format negotiation (wire protocol): during handshake, each `output` global event includes the supported `drm_format`/modifier list for that output's primary and overlay planes. When a client creates a buffer, it includes its own format/modifier list in the request; the library picks the first pair that matches the output's capabilities and returns the accepted pair in the response. No match, the buffer creation fails. For multi-GPU, matching is against the scanout GPU's capabilities; the render GPU's capabilities are never advertised to clients.
- Buffer recycling: the library caches recently-imported dmabuf handles per client. Repeated fds reuse the existing `drm_prime_fd_to_handle()` result rather than re-importing.
- Mapping discipline: `mmap()` is used only to read metadata (stride, offset). Pixel data never leaves GPU/DMA-BUF space through the library.
- Cross-GPU import/export: the library handles `drmPrimeHandleToFD`/`drmPrimeFDToHandle` across multiple render nodes, one import, zero copies, regardless of which GPU rendered the buffer versus which GPU scans it out.
- Memory pressure handling: Architect-configurable, not an automatic unilateral library policy. The library can expose PSI (`/sys/kernel/mm/pressure/`) monitoring and an eviction mechanism for offscreen surface backing storage, but the threshold, and whether it's enabled at all, is set by the Architect.
- PSI unavailable (kernel older than 4.20, or built without `CONFIG_PSI`): the monitoring mechanism is simply disabled. No `/proc/meminfo`-based fallback; that signal is too crude to be worth the added code path.
- Per-client memory limits: the library does not track total imported buffer size per client; only the count-based build-time limits (§4) apply. Tracking actual byte usage would add overhead to the hot path for a case that's rare in practice. An Architect wanting real memory-exhaustion protection can track sizes themselves via `ISZ_EVENT_CLIENT_CONNECT` and the buffer descriptors they see, disconnecting offenders, or lean on the PSI-based eviction mechanism above.

## 9. Input handling

- Event pump: `libinput_next_event()` is called directly in the main dispatch loop. No internal queue; events are pushed straight to the Architect's registered listeners.
- Per-device calibration: exposed for touch/pen devices (`isz_seat_device_set_calibration(float[9])`, for example); the library applies the transform before delivering absolute coordinates.
- Timestamps: every event carries `CLOCK_MONOTONIC_RAW` timestamps, usable by the Architect for input prediction/anti-lag techniques.
- Keymap compilation: compiled once per layout change via libxkbcommon, cached per seat, not recompiled per keystroke.
- Hotplug: libseat supplies device fds; the library surfaces `ISZ_EVENT_SEAT_ADD` / `ISZ_EVENT_SEAT_REMOVE` events. Policy on what to do about a hotplugged device is entirely the Architect's.

Session management: the library initializes libseat and registers its own seat listener internally. When a VT switch away occurs, it surfaces `ISZ_EVENT_SESSION_INACTIVE`; when the compositor's VT becomes active again, it surfaces `ISZ_EVENT_SESSION_ACTIVE`. The Architect must pause rendering and KMS commits on `INACTIVE` and may resume on `ACTIVE`. The library does not automatically block commits on session loss; a setup doing screen capture via writeback may need to keep rendering while switched away, so that decision stays with the Architect.

libinput context ownership: the library owns the libinput context; the Architect has no direct access to it. Configuration knobs relevant to display-server policy are exposed through the library's own API instead:

```c
int isz_seat_device_set_tap_enabled(isz_seat_device *dev, bool enabled);
int isz_seat_device_set_tap_drag_enabled(isz_seat_device *dev, bool enabled);
int isz_seat_device_set_natural_scroll(isz_seat_device *dev, bool enabled);
int isz_seat_device_set_accel_profile(isz_seat_device *dev, enum isz_accel_profile profile);
```

These map directly to the corresponding libinput config calls. This spec covers the common cases for a desktop compositor; a libinput feature not exposed here is a gap in the library, not something the Architect can reach around.

Keyboard state: the library tracks XKB state (modifiers, active layout, locked modifiers) internally. On a key press or release, it sends the raw event via `ISZ_EVENT_INPUT_KEYBOARD_KEY` (keycode, press/release, timestamp). A separate `ISZ_EVENT_INPUT_KEYBOARD_MODIFIERS` event fires when modifier state changes, carrying `mods_depressed`, `mods_latched`, `mods_locked`, and `group` (active layout). The library does no key-to-symbol translation; that's the Architect's job if they want one, via libxkbcommon directly.

Input focus cleanup: when a client disconnects while its surface holds keyboard focus, the library sets focus to `NULL` and sends `ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED` with `NULL` as the new focus. The library never reassigns focus to another surface automatically; deciding what gets focus next is the Architect's job via `isz_seat_set_keyboard_focus()`.

Event types (`isz_event`, a union tagged by `isz_event_type`, every event carrying a `CLOCK_MONOTONIC_RAW` timestamp):

- `ISZ_EVENT_INPUT_POINTER_MOTION`: delta x/y, absolute x/y if available.
- `ISZ_EVENT_INPUT_POINTER_BUTTON`: button number, press/release.
- `ISZ_EVENT_INPUT_POINTER_AXIS`: scroll delta x/y, source (finger/wheel/continuous).
- `ISZ_EVENT_INPUT_KEYBOARD_KEY`: keycode, press/release.
- `ISZ_EVENT_INPUT_KEYBOARD_MODIFIERS`: modifier/layout state, as above.
- `ISZ_EVENT_INPUT_TOUCH_DOWN` / `TOUCH_MOTION` / `TOUCH_UP`: touch id, absolute x/y where applicable.
- `ISZ_EVENT_INPUT_TOUCH_FRAME`: end of a touch frame.
- `ISZ_EVENT_SEAT_ADD` / `SEAT_REMOVE`: device hotplug.
- `ISZ_EVENT_SESSION_ACTIVE` / `SESSION_INACTIVE`: VT switch.
- `ISZ_EVENT_OUTPUT_ADD` / `OUTPUT_REMOVE`: connector hotplug (§10).
- `ISZ_EVENT_CLIENT_CONNECT` / `CLIENT_DISCONNECT`: client lifecycle, post-allowlist.
- `ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED`: focus cleared or moved.
- `ISZ_EVENT_CLIPBOARD_REQUEST`: another client requested clipboard contents (§6.8).
- `ISZ_EVENT_IDLE_INHIBIT_ACTIVE` / `IDLE_INHIBIT_INACTIVE`: an output's idle-inhibit state changed (§6.15).
- `ISZ_EVENT_TEXT_INPUT_PREEDIT`: preedit text from the active input method (§6.16).
- `ISZ_EVENT_TEXT_INPUT_COMMIT`: committed text from the active input method (§6.16).
- `ISZ_EVENT_TEXT_INPUT_CURSOR_RECTANGLE_NEEDED`: the IME requested the cursor rectangle for the focused text-input (§6.16).

## 10. Backend abstraction & multi-GPU

- Formal backend interface: an `isz_backend_ops`-style struct with at minimum `init()`, `commit()`, `read_events()`. DRM/KMS is the default backend, headless is the test backend, and nested (running inside an existing desktop session) is a possible future backend, not required for v1 but the interface shouldn't preclude it.
- Backend selection: `isz_backend_type` covers `ISZ_BACKEND_DRM` (default, no extra init data needed, opens the primary DRM node from libseat's session), `ISZ_BACKEND_HEADLESS` (needs an `isz_headless_config` with `width`, `height`, `refresh_rate` for the virtual outputs), and `ISZ_BACKEND_NESTED` (needs an `isz_nested_config` with a parent window handle; deferred to post-v1, API shape fixed now so it doesn't need to change later). `isz_init()` takes a `void *config` cast per backend type; `NULL` for DRM.
- Backend state machine: `UNINITIALIZED` (no resources) → `init()` → `READY` (resources allocated, no active modeset) → `commit()` → `COMMITTING` (atomic commit in flight, fence not yet signaled) → `read_events()` moves it back to `READY` once the commit completes. An `ERROR` state (DRM master lost, for example) surfaces the error via `read_events()`; recovery requires closing the backend's resources and calling `init()` again. Calling `commit()` while already `COMMITTING` returns `ISZ_ERR_COMMIT_PENDING` rather than queueing a second commit, which would violate the single-threaded model (§5).
- GPU node enumeration: the library scans `/dev/dri/renderD*` at init. The Architect chooses which node is used per output; the library doesn't auto-select a "best" GPU.
- Multi-GPU plane assignment: a surface is assigned to a specific output via `isz_surface_set_output()` (§7.6). At commit time, the library looks up that output's associated GPU node (set when the output was enabled) and requests the surface's plane slot from that GPU's plane pool. If the surface's buffer was rendered on a different GPU than the one doing scanout, the library handles the cross-GPU import (§8) transparently; the surface itself never needs to know which GPU it ends up on.
- Hotplug-aware connector tracking: the library listens for `DRM_EVENT_CONNECT` and updates its connector list, surfacing a callback to the Architect on change.
- Output enablement: when a new connector appears, the library surfaces `ISZ_EVENT_OUTPUT_ADD` to the Architect. The Architect must explicitly call `isz_output_enable(output, mode)` to assign a mode and CRTC to that connector. The library never lights up a new display on its own; that would be policy.
- Output removal: when a connector disappears, the library surfaces `ISZ_EVENT_OUTPUT_REMOVE`. The output object stays valid until the Architect calls `isz_output_disable()` followed by `isz_output_destroy()`; the library doesn't destroy it automatically, since the Architect may want to preserve surface assignments in case the monitor comes back. Any commit targeting a disconnected output is rejected with `ISZ_ERR_OUTPUT_DISCONNECTED` in the meantime.
- DPMS / output power state: `isz_output_set_dpms(out, state)`, where `state` is `ISZ_DPMS_ON`, `OFF`, `STANDBY`, or `SUSPEND`, mapped to the KMS `DPMS` property. Deciding when to power down after inactivity is the Architect's own timerfd-driven logic, not something the library tracks.

## 11. Hardware fallback matrix

Atomic KMS is a hard requirement (§3); there is no fallback for it, the library fails fast at backend-init instead. The remaining hardware-dependent behaviors:

| Capability | If unsupported by the GPU/driver | Notes |
|---|---|---|
| DMA-BUF import for a given format/modifier | Server attempts modifier renegotiation with the client first; if that also fails, falls back to an SHM path. Capability is advertised per-output at connect time so clients can negotiate up front rather than failing mid-session. | This is the one place SHM appears in v1, as a fallback, not a parallel primary path. |
| Explicit sync (`drm_syncobj`) | Falls back to implicit sync via the DMA-BUF's own kernel-level fencing. Functionally correct, just less precise (it can't express "wait for this specific point" the way explicit sync can). | Considered a "free" fallback, since implicit sync is what DMA-BUF guarantees underneath regardless. |
| Hardware cursor plane | No fallback. Commit fails; it's the Architect's problem entirely. | Decided per-output at output-init time, since some outputs on a multi-GPU system may support it while others don't, with no library-level compensation either way. |
| Multi-GPU render-offload | Falls back to single-GPU mode: the library uses only the GPU that has at least one connected output (the scanout/primary GPU) at startup, and ignores render-only GPUs entirely. | No partial/best-effort offload attempted; either full offload works or the second GPU is unused. Distinct from the DMA-BUF modifier fallback above: if the primary scanout GPU can't import a format/modifier the render GPU produced, that's handled by the DMA-BUF fallback path (modifier renegotiation, then SHM), not by this one. |

## 12. Fault tolerance & debugging

| Feature | Behavior |
|---|---|
| Crash recovery / VT restore | Runtime opt-in, not automatic and not a compile-time build flag. The Architect explicitly calls something like `isz_enable_crash_recovery()` to have the library install a SIGSEGV/SIGABRT handler that restores the VT and blanks all CRTCs to black on crash. Off by default, so it never silently clashes with an Architect-supplied crash reporter. The handler never calls `exit()`; it restores the VT, blanks all CRTCs, then re-raises the original signal (or calls `abort()`), so any Architect-installed crash handler further down the chain still runs. |
| Failed atomic commit | The library automatically rolls back to the previous known-good state. The Architect receives `ISZ_ERR_COMMIT_FAILED`, but display state remains consistent, never left partial or undefined. |
| Logging | No built-in stderr output. The Architect registers a logging callback; the library calls it and never writes to stderr/stdout directly. |
| Test-only commits | `isz_commit(out, ISZ_COMMIT_TEST_ONLY)` lets the Architect probe a configuration (mode, plane assignment, etc.) without producing a visible/committed frame. |
| FD exhaustion hardening | The library tracks its own open fd count (DRM, input, epoll) internally and refuses to import new DMA-BUFs if `EMFILE` is close, rather than crashing on the syscall failure. |

## 13. X11 compatibility (lower priority)

App compatibility is scoped to X11 only: an XWayland-equivalent that bridges X11 clients into Ishizue's native compositor. No Wayland compatibility layer is planned.

Implementation shape: the bridge is a separate process, not part of the library, that acts as an ordinary Ishizue client. It speaks the native wire protocol (§6) over the same Unix socket mechanism as any other client, while listening on the X11 socket (`/tmp/.X11-unix/X<display>`) and translating X11 requests into Ishizue surface/buffer/input operations. The library provides no special-cased APIs for it; it's treated as a privileged client via the allowlist (§6.3), using the same primitives everyone else uses.

This is a lower-priority, later piece of work, not required for v1 to be architecturally complete. Its absence is fine given that native-protocol-first was chosen deliberately, even at the cost of delayed daily usability. The v1 library itself doesn't need to know X11 exists.

## 14. Dev workflow

Nested mode (running the server as a window inside an existing desktop session) is a nice-to-have, not a v1 blocker.

Primary dev/test path is the headless backend (§4, §10) plus real hardware testing on bare-metal TTY via DRM/KMS.

Reference development hardware is an Intel N4100 (quad-core, 4GB RAM). This is the lower bound used for day-to-day testing, not a design constraint: the library targets the full Mesa/Intel/AMD class (§3), and the build-time resource limits in §4 are set generically for that class rather than squeezed to fit this one machine.

## 15. Open items / explicitly deferred

- SHM as a first-class parallel path (currently fallback-only, see §11)
- Vulkan backend (only pursued if GLES latency proves insufficient)
- Wayland client compatibility (not planned at all, X11 only)
- Nested backend implementation (interface should allow it, not required now)
- NVIDIA support (explicitly out of scope, not just deferred)

## 16. Changelog

- 2026-07-18: §6.4: surface serial (64-bit, monotonic, global to server) for cross-subsystem pairing.
- 2026-07-18: §6.4: surface-role concept added to the v1 object/feature set; details in §6.17.
- 2026-07-18: §6.8: PRIMARY selection slot per seat, distinct from CLIPBOARD.
- 2026-07-18: §6.8: selection-ownership timestamp (`CLOCK_MONOTONIC_RAW` nanoseconds) and stale-claim rejection.
- 2026-07-18: §6.11: portal-consent scope expanded from screen-capture-only to screen capture, file access, notification forwarding, and other portal-style consent; heading renamed from "Screen Capture Permission" to "Portal consent".
- 2026-07-18: §6.15: idle-inhibit flag (`isz_surface_set_idle_inhibit`) and `ISZ_EVENT_IDLE_INHIBIT_ACTIVE` / `_INACTIVE` events.
- 2026-07-18: §6.16: text-input and input-method objects (`isz_text_input`, `isz_input_method`) and `ISZ_EVENT_TEXT_INPUT_PREEDIT` / `_COMMIT` / `_CURSOR_RECTANGLE_NEEDED` events.
- 2026-07-18: §6.17: surface-role enum and `isz_surface_set_role()` covering `ISZ_SURFACE_ROLE_NORMAL`, `_X11_TOPLEVEL`, `_X11_POPUP`, `_LAYER`.
- 2026-07-18: §7.2: fractional scale via `isz_surface_set_scale(numerator, denominator)` and the `preferred_scale` client event.
- 2026-07-18: §9: `ISZ_EVENT_IDLE_INHIBIT_ACTIVE` / `_INACTIVE` and `ISZ_EVENT_TEXT_INPUT_PREEDIT` / `_COMMIT` / `_CURSOR_RECTANGLE_NEEDED` added to the `isz_event_type` enum.
