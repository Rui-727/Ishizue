# Ishizue public API reference

Function-by-function reference for `<ishizue/isz.h>`. Read alongside
SPEC.md; each entry cross-references the section that fixes the behaviour.
Conventions:

- Every function returning `int` returns `ISZ_OK` (0) on success and a
  negative `ISZ_ERR_*` constant on failure. `isz_strerror()` translates
  codes to strings (SPEC §7.10).
- Unless noted, every `isz_*` function is main-thread-only. The single
  exception is `isz_thread_pool_submit()`, which is safe to call from
  worker threads (SPEC §5). The library does no internal locking on the
  hot path; callers must serialize.
- Handles returned by `isz_*_create*` are owned by the caller and freed
  by the matching `isz_*_destroy()`. Passing a borrowed handle does not
  transfer ownership of the handle. `dmabuf_fd` arguments are the one
  transfer-of-ownership case; see `isz_surface_attach_buffer`.

## Lifecycle (SPEC §5, §7.6, §10)

### `isz_server *isz_init(enum isz_backend_type backend, void *backend_config)`

Allocates the server, creates the backend (`isz_backend_create`
dispatches by type), creates the default seat, spawns the thread pool
(build-time `ENABLE_THREAD_POOL=1`; NULL pool is non-fatal), arms the
PSI monitor if `/sys/kernel/mm/pressure/` is usable, and registers the
headless output hook if `backend == ISZ_BACKEND_HEADLESS`. The epoll
set is created but empty; the listen fd is added later by
`isz_listen()`.

`backend_config` is cast per backend type:

| `backend`              | `backend_config` type        | Notes |
|---|---|---|
| `ISZ_BACKEND_DRM`      | `NULL`                       | Primary DRM node from libseat |
| `ISZ_BACKEND_HEADLESS` | `isz_headless_config *`      | `width`, `height`, `refresh_rate` (mHz) |
| `ISZ_BACKEND_NESTED`   | `isz_nested_config *`        | Post-v1; API shape fixed |

Return: a server handle on success, `NULL` on failure. Failure modes
are allocation failure, epoll_create1 failure, or backend init failure
(DRM master unavailable, headless config invalid). Errors go through
the log callback; there is no out-parameter.

The server starts in the `ISZ_SERVER_RUNNING` state. `isz_destroy()`
moves it to `ISZ_SERVER_DESTROYING` before tearing anything down;
listener callbacks can read that state via the internal accessor, but
the public API does not expose it.

### `void isz_dispatch(isz_server *srv)`

One non-blocking tick. Drains the library's epoll set with timeout 0
and routes each ready fd by tag: listen fd to `isz_accept_connection`,
client fd to `isz_recv_client_messages`, PSI fd to `isz_psi_dispatch`,
backend fd to `isz_backend_read_events`. After epoll it calls
`isz_session_dispatch` (libseat) and `isz_input_dispatch` (libinput);
both are no-ops until the DRM wave wires their fds.

Call this every iteration of your epoll loop. The library never blocks
inside `isz_dispatch`. Listeners fire synchronously from inside
subsystem handlers, so a listener callback runs in the same thread
that called `isz_dispatch`.

Return: none. Errors are routed through the log callback.

### `int isz_get_fds(isz_server *srv, int *fds, size_t max)`

Returns the pollable fds the Architect may want in their own epoll
set: libinput fd (when wired), PSI fd (when armed), and every client
socket fd. The listen fd is owned by the library's epoll set after
`isz_listen()`; you do not need to poll it yourself.

If `max` is smaller than the fd count, `fds` is filled up to `max` and
the return value is the true count. The caller learns of overflow by
comparing return value to `max`.

Return: number of fds (possibly exceeding `max`), or 0 on `NULL srv`
or `NULL fds`.

### `void isz_destroy(isz_server *srv)`

Moves the server to `ISZ_SERVER_DESTROYING`, closes the listen fd,
runs SPEC §6.12 cleanup for every client (close conn, free queue,
emit `CLIENT_DISCONNECT`), destroys outputs, tears down the input
subsystem, drains and joins the thread pool, destroys the backend,
frees the listener registry and allowlist, closes epoll, frees the
server.

`NULL srv` is a no-op. Calling `isz_destroy()` from a listener
callback is undefined; schedule destruction outside the dispatch
thread instead.

Return: none.

### `int isz_listen(isz_server *srv, int listen_fd)`

Hands a bound, listening UDS fd to the library (SPEC §6.1). The
library owns `accept()`, the §6.2 handshake, the §6.3 allowlist
check, and per-client dispatch from this point on. The fd is switched
to `O_NONBLOCK` so `isz_accept_connection` can drain the accept queue
in a tight loop.

Create the socket yourself (`socket(AF_UNIX, SOCK_STREAM, 0)`), bind
to your chosen path, call `listen(2)`, then hand the fd here. You
retain ownership of the path; `isz_destroy()` does not unlink it.

Return: `ISZ_OK` on success. `ISZ_ERR_INVALID_ARG` if `srv` is NULL,
`listen_fd` is negative, the server is not in `RUNNING`, or a listen
fd is already set. epoll_ctl failure also returns `ISZ_ERR_INVALID_ARG`.

## Client allowlist (SPEC §6.3)

An empty allowlist means deny-all: if neither function is called
before clients connect, every connection is closed before any
handshake byte is sent. Entries are checked at `accept()` time via
`SO_PEERCRED` (peer pid) → `stat("/proc/<pid>/exe")` for binary
entries, `read("/proc/<pid>/cgroup")` for cgroup entries.

### `int isz_allowlist_add_binary(isz_server *srv, const char *path)`

Stats `path` at call time and stores `(st_dev, st_ino)`. Path renames
do not invalidate the entry; the inode is the identity. The path must
exist when this is called.

Return: `ISZ_OK`, `ISZ_ERR_INVALID_ARG` (NULL args or stat failure),
or `ISZ_ERR_NO_MEMORY`.

### `int isz_allowlist_add_cgroup(isz_server *srv, const char *cgroup_path)`

Stores `cgroup_path` verbatim. The check at accept time reads
`/proc/<pid>/cgroup`, parses each line's path component (cgroup v1 and
v2 layouts both supported), and matches on prefix with a `/` boundary
so `/user.slice` does not match `/user.slice-foo`. Use a longer path
to allowlist a single slice; use `/` to allowlist everything under
the root cgroup (which is equivalent to disabling the cgroup check).

Return: `ISZ_OK`, `ISZ_ERR_INVALID_ARG`, or `ISZ_ERR_NO_MEMORY`.

## Outputs (SPEC §7.2, §7.6, §7.7, §10)

### `isz_output **isz_output_list(isz_server *srv, size_t *count)`

Returns a borrowed pointer to a cache of every output wrapper. The
cache is rebuilt on each call; do not free it. The pointers inside
stay valid until the next call to `isz_output_list` or
`isz_output_destroy`. `*count` receives the count.

Return: the array, or `NULL` if `srv` is NULL or allocation fails. On
NULL, `*count` is set to 0.

### `isz_mode **isz_output_get_modes(isz_output *out, size_t *count)`

Returns the output's mode list. For headless, this is a single
synthetic mode matching the configured geometry. For DRM, modes come
from `drmModeGetConnector` plus EDID-derived refresh rates; the
library does not parse EDID beyond the HDR static metadata block.

The returned array is owned by the output and stays valid until
`isz_output_destroy(out)`. Pass one of these pointers to
`isz_output_enable`.

Return: the array, or `NULL` if `out` is NULL.

### `int isz_output_enable(isz_output *out, isz_mode *mode)`

Records `mode` as the output's current mode and marks the output
enabled. The library never lights a display on its own; this is the
explicit enable call (SPEC §10). The headless path stores the
selection; the DRM path issues `drmModeAtomicCommit` with
`DRM_MODE_ATOMIC_ALLOW_MODESET` from the render wave.

`mode` must belong to `out` (pointer identity, not value). The
function returns `ISZ_ERR_INVALID_ARG` otherwise.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_output_disable(isz_output *out)`

Clears the current mode and marks the output disabled. Does not
change DPMS; call `isz_output_set_dpms()` separately if you need a
power transition.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `void isz_output_destroy(isz_output *out)`

Unlinks `out` from the server's output list, frees its modes, EDID,
plane slot table, color LUTs, and the wrapper struct. After this call
the pointer is invalid. SPEC §10 says the wrapper stays valid from
`ISZ_EVENT_OUTPUT_REMOVE` until you call this; commits to a removed
output return `ISZ_ERR_OUTPUT_DISCONNECTED` in the meantime.

`NULL out` is a no-op.

### `int isz_output_set_dpms(isz_output *out, enum isz_dpms_state state)`

Records the DPMS state (`ISZ_DPMS_ON`, `_STANDBY`, `_SUSPEND`,
`_OFF`). The KMS `DPMS` property write happens at the next commit.
Inactivity timers are the Architect's job (timerfd in your epoll
set); the library tracks nothing.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `const uint8_t *isz_output_get_edid(isz_output *out, size_t *size)`

Returns the raw EDID blob. The library validates the checksum and
parses only `HDR_STATIC_METADATA`; everything else (modes, physical
dimensions, serial) is exposed as opaque bytes for the Architect to
parse. Headless outputs return NULL with `*size == 0`.

The pointer is owned by the output and valid until `isz_output_destroy`.

Return: the blob, or `NULL`. `*size` is set on both success and
NULL-out cases.

### `int isz_output_set_gamma(isz_output *out, const uint16_t *r, const uint16_t *g, const uint16_t *b, size_t size)`

Sets the GAMMA_LUT. `size` must equal the hardware's
`GAMMA_LUT_SIZE`; for headless the sentinel is 256. The library
copies the LUT; you can free `r`/`g`/`b` on return. The KMS blob is
created at commit time.

Return: `ISZ_OK`, `ISZ_ERR_INVALID_ARG` (NULL args or size mismatch),
`ISZ_ERR_NO_MEMORY`.

### `int isz_output_set_degamma(isz_output *out, const uint16_t *r, const uint16_t *g, const uint16_t *b, size_t size)`

Same as `isz_output_set_gamma` but for the DEGAMMA_LUT. Same size
validation, same ownership.

### `int isz_output_set_ctm(isz_output *out, const float matrix[9])`

Sets the CTM (color transformation matrix). The library copies the 9
floats; you can free `matrix` on return.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_output_set_hdr_metadata(isz_output *out, const isz_hdr_metadata *meta)`

Sets the HDR_OUTPUT_METADATA blob. `meta->bytes` carries either the
EDID-parsed `HDR_STATIC_METADATA` or an Architect-supplied override;
`meta->size` is the byte count (capped internally at 64). When
`ENABLE_HDR=0` at build time, returns `ISZ_ERR_FEATURE_UNAVAIL`.

Return: `ISZ_OK`, `ISZ_ERR_INVALID_ARG`, `ISZ_ERR_FEATURE_UNAVAIL`.

### `size_t isz_output_get_plane_slots(isz_output *out, isz_plane_slot_info *out_slots, size_t max)`

Returns the output's plane slot table (SPEC §7.7). Pass `out_slots`
NULL (or `max == 0`) to query the count without copying. Otherwise
fills up to `max` entries and returns the true count.

Each `isz_plane_slot_info` carries: `id`, `type` (primary/overlay/
cursor), `supported_formats` (DRM fourccs, up to 16),
`format_count`, `supports_scaling`, `supports_transform`, `zpos_min`,
`zpos_max`. Headless synthesizes three slots (primary + overlay +
cursor) with XRGB8888/ARGB8888 support.

Return: slot count, or 0 if `out` is NULL.

## Surfaces (SPEC §6.6, §6.7, §7.6, §7.9, §8)

### `isz_surface *isz_surface_create(isz_server *srv)`

Creates a normal surface. The surface starts with no output, no
buffer, no plane type, no plane slot. Plane type and slot are
mandatory before commit; see §7.7.

Surfaces are tracked in a module-static list. The lifecycle wave will
move this onto the server struct; the public API does not change.

Return: a surface handle, or `NULL` if `srv` is NULL or allocation
fails.

### `void isz_surface_destroy(isz_surface *surf)`

Releases the surface's plane slot reservation (if any), releases and
unrefs the current and pending-release buffers, frees the damage
list, and frees the surface. Children of a subsurface are
independent surfaces; destroying the parent does not destroy them.

`NULL surf` is a no-op.

### `int isz_surface_attach_buffer(isz_surface *surf, int dmabuf_fd, isz_buffer_desc *desc)`

Imports `dmabuf_fd` (via `isz_buffer_import`) and swaps it in as the
surface's current buffer. The previous current buffer, if any, moves
to `pending_release` and stays referenced until its fence signals
(SPEC §8 double-buffering; up to 2 in-flight buffers per surface).

Ownership: `dmabuf_fd` transfers to the library on every path. On
failure the library closes the fd; on success the library owns it.
Do not close it yourself, even on a successful attach.

`desc` carries width, height, stride, offset, format (DRM fourcc),
modifier (`DRM_FORMAT_MOD_INVALID` for implicit), and `alpha_mode`
(`ISZ_ALPHA_NONE`, `_PREMULTIPLIED`, `_NON_PREMULTIPLIED`).
`alpha_mode` is only honoured on planes that support per-pixel
alpha; overlay planes generally ignore it.

Return: `ISZ_OK`, `ISZ_ERR_INVALID_ARG` (NULL surf, negative fd, NULL
desc), `ISZ_ERR_INVALID_DMABUF` (import failure), `ISZ_ERR_NO_MEMORY`,
`ISZ_ERR_RESOURCE_LIMIT`.

The attach + commit + release cycle is the only non-obvious usage
pattern in the API:

```c
isz_buffer_desc desc = { .width = w, .height = h, .stride = stride,
                         .offset = 0, .format = DRM_FORMAT_XRGB8888,
                         .modifier = DRM_FORMAT_MOD_INVALID,
                         .alpha_mode = ISZ_ALPHA_NONE };
int rc = isz_surface_attach_buffer(surf, dmabuf_fd, &desc);
/* dmabuf_fd is library-owned from here; do not close. */
if (rc != ISZ_OK) return rc;
isz_surface_damage(surf, &(isz_rect){0,0,w,h}, 1);
isz_commit(out, ISZ_COMMIT_NORMAL);
/* The library sends ISZ_MSG_RELEASE on the wire once the buffer is
 * no longer in flight. The client may then reuse or destroy it. */
```

### `int isz_surface_detach_buffer(isz_surface *surf)`

Releases the current buffer. The pending-release buffer (if any)
stays referenced until its fence signals. Detaching does not cancel
an in-flight commit.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_surface_damage(isz_surface *surf, isz_rect *rects, size_t count)`

Appends `count` damage rectangles to the surface's pending damage
list (SPEC §7.9). Rectangles are in surface-local coordinates,
half-open `[x1,y1,x2,y2)`. A `count == 0` call is a no-op success.

The internal cap is 256 rectangles per surface. Exceeding it returns
`ISZ_ERR_RESOURCE_LIMIT`.

Return: `ISZ_OK`, `ISZ_ERR_INVALID_ARG`, `ISZ_ERR_RESOURCE_LIMIT`,
`ISZ_ERR_NO_MEMORY`.

### `int isz_surface_set_output(isz_surface *surf, isz_output *out)`

Binds `surf` to `out`. A surface with no output is never scanned out.
Changing output does not release the old plane slot reservation; the
commit path re-reserves against the new output.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_surface_clear_output(isz_surface *surf)`

Releases the plane slot reservation (if any) and unbinds the surface
from its output. The surface's plane type and slot settings persist.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_surface_set_position(isz_surface *surf, int x, int y)`

Output-relative position, pixels. There is no validation against
output bounds; off-screen positions commit fine and are clipped by
KMS at scanout.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_surface_set_size(isz_surface *surf, int width, int height)`

Overrides the buffer's logical size for per-plane scaling (SPEC §7.2,
§8). `width` and `height` must be positive. If never called, the
surface's logical size comes from the attached buffer's
`desc.width`/`desc.height`.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG` (NULL surf or non-positive
dimensions).

### `int isz_surface_set_plane_type(isz_surface *surf, int type)`

Sets the plane type (`ISZ_PLANE_PRIMARY`, `_OVERLAY`, `_CURSOR`).
Mandatory; the commit path rejects a surface with no plane type set
with `ISZ_ERR_INVALID_ARG`. There is no library default (SPEC §7.7).

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG` (NULL surf or out-of-range
type).

### `int isz_surface_set_plane_slot(surf, int slot)`

Assigns the surface to one of the output's plane slots (the `id`
field of `isz_plane_slot_info`). Optional at the API level,
load-bearing at commit time: a surface with no slot set causes
`isz_commit()` to return `ISZ_ERR_SURFACE_NO_PLANE_SLOT`. The
library never silently assigns a default.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG` (NULL surf or negative
slot).

### `int isz_surface_set_zpos(isz_surface *surf, int zpos)`

Z-position. Must fall within the assigned slot's `[zpos_min,
zpos_max]`. The commit path validates this against the slot table.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_surface_set_transform(isz_surface *surf, enum isz_transform t)`

Sets the KMS `rotation` property. `t` is one of `ISZ_TRANSFORM_NORMAL`,
`_ROTATE_90`, `_ROTATE_180`, `_ROTATE_270`, `_REFLECT_X`, `_REFLECT_Y`.
If the assigned slot does not support transforms, the commit returns
`ISZ_ERR_TRANSFORM_UNSUPPORTED`. There is no software fallback (SPEC
§7.2, §11).

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `isz_surface *isz_surface_create_subsurface(isz_surface *parent)`

Creates a subsurface (SPEC §6.6). Position is relative to the
parent's surface-local coordinates; clipped to parent bounds.
Subsurface commits are synchronized with the parent by default; set
`ISZ_SUBSURFACE_DESYNC` to decouple. Children are tracked on the
parent's `children` list; destroying the parent does not destroy the
children (they become orphaned surfaces).

Return: a surface handle, or `NULL` if `parent` is NULL or allocation
fails.

### `int isz_surface_set_subsurface_flags(isz_surface *sub, uint32_t flags)`

Sets subsurface flags. Currently only `ISZ_SUBSURFACE_DESYNC` (1) is
defined. The surface must be a subsurface.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG` (NULL, wrong kind, or
non-subsurface).

### `isz_surface *isz_surface_create_popup(isz_surface *parent, int x, int y)`

Creates a popup (SPEC §6.7). Position is `(x, y)` relative to the
parent. The library clips to output bounds on commit. Popups grab
keyboard and pointer input until the client sends `popup_dismiss`.

Return: a surface handle, or `NULL` if `parent` is NULL or allocation
fails.

### `isz_surface *isz_surface_create_layer(isz_output *out, enum isz_layer layer)`

Creates a layer-shell surface (SPEC §6.7). `layer` is
`ISZ_LAYER_OVERLAY`, `_BOTTOM`, `_TOP`, or `_LOCK`. Layer-shell
surfaces sit outside the tiling logic; the library stacks them
internally based on layer type. The surface is bound to `out`
immediately.

Return: a surface handle, or `NULL` if `out` is NULL or allocation
fails.

## Buffer descriptor (SPEC §8)

`struct isz_buffer_desc` is a public struct, not an opaque handle:

```c
struct isz_buffer_desc {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t offset;
    uint32_t format;     /* DRM_FORMAT_* fourcc */
    uint64_t modifier;   /* DRM_FORMAT_MOD_* or DRM_FORMAT_MOD_INVALID */
    uint8_t  alpha_mode; /* isz_alpha_mode */
};
```

`alpha_mode` is `ISZ_ALPHA_NONE`, `ISZ_ALPHA_PREMULTIPLIED`, or
`ISZ_ALPHA_NON_PREMULTIPLIED`. Only honoured on planes that support
per-pixel alpha; ignored on overlay planes that do not.

## Composition target (SPEC §7.7)

### `int isz_composition_target_create(isz_server *srv, uint32_t width, uint32_t height, uint32_t format, int *dmabuf_fd_out)`

Allocates a DMA-BUF the Architect renders into for software
composition when surface count exceeds plane count. Attach the
returned fd to a surface via `isz_surface_attach_buffer` exactly like
any client-provided buffer.

The current implementation uses `memfd_create` as a stand-in (4
bytes/pixel, XRGB8888/ARGB8888 assumed). The real DMA-BUF allocation
(`drm_prime_alloc`, `gbm_bo`, or `dma-heap`) lands with the GPU wave;
`format` is accepted but ignored in this build.

Ownership: `*dmabuf_fd_out` is caller-owned. Close it after
`isz_surface_attach_buffer` succeeds (the library dups internally on
attach) - or, simpler, hand it directly to `attach_buffer` and let
the library own it.

Return: `ISZ_OK`, `ISZ_ERR_INVALID_ARG` (NULL args, zero dimensions),
`ISZ_ERR_NO_MEMORY`.

### `int isz_composition_target_get_egl_image(int dmabuf_fd, isz_buffer_desc *desc, void **egl_image_out)`

Returns an EGLImage for the given DMA-BUF, for the Architect's own
GLES pass. Stubbed in this build (returns `ISZ_ERR_FEATURE_UNAVAIL`,
sets `*egl_image_out = NULL`); real EGL wiring lands with the GLES
wave.

Return: `ISZ_OK` (not yet), `ISZ_ERR_INVALID_ARG`, or
`ISZ_ERR_FEATURE_UNAVAIL`.

## Seats (SPEC §9)

### `isz_seat *isz_seat_default(isz_server *srv)`

Returns the default seat (`seat0`), creating it on first call. The
headless backend has a seat with no devices; the DRM backend wires
libinput + libseat in the DRM wave.

Return: the seat, or `NULL` if `srv` is NULL or allocation fails.

### `int isz_seat_set_keyboard_focus(isz_seat *seat, isz_surface *surf)`

Sets keyboard focus to `surf` (or NULL to clear). The library never
reassigns focus automatically on disconnect; when the focused
surface's client disconnects, the library clears focus to NULL and
emits `ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED`. Deciding what gets
focus next is the Architect's job (SPEC §6.12, §9).

The call emits `ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED` synchronously.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_seat_set_cursor_surface(isz_seat *seat, isz_surface *surf)`

Binds `surf` as the cursor surface. The library does not load cursor
themes (SPEC §6.10); the Architect loads images via libxcursor or
similar and hands them in as ordinary surfaces. There is no software
cursor fallback (SPEC §7.4).

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_seat_set_cursor_hotspot(isz_seat *seat, uint32_t x, uint32_t y)`

Sets the cursor hotspot in pixels, relative to the cursor surface's
top-left.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_seat_set_cursor_visible(isz_seat *seat, bool visible)`

Hides or shows the cursor. Hidden state takes effect on the next
commit.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_seat_device_set_tap_enabled(isz_seat_device *dev, bool enabled)`

Enables tap-to-click on a touchpad. Maps to
`libinput_device_config_tap_set_enabled`. No-op if the device does
not support tapping (the libinput call returns the finger count, the
apply function checks it).

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_seat_device_set_tap_drag_enabled(isz_seat_device *dev, bool enabled)`

Enables tap-drag (locked drag during a tap-and-hold). Maps to
`libinput_device_config_tap_set_drag_enabled`.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_seat_device_set_natural_scroll(isz_seat_device *dev, bool enabled)`

Inverts scroll direction. Maps to
`libinput_device_config_scroll_set_natural_scroll_enabled`.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_seat_device_set_accel_profile(isz_seat_device *dev, enum isz_accel_profile profile)`

Sets the pointer acceleration profile. `profile` is `ISZ_ACCEL_NONE`,
`_FLAT`, or `_ADAPTIVE`. Maps to
`libinput_device_config_accel_set_profile`.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

### `int isz_seat_device_set_calibration(isz_seat_device *dev, const float matrix[9])`

Sets the calibration matrix for touch/pen devices. The library
applies the transform before delivering absolute coordinates. Maps
to `libinput_device_config_calibration_set_matrix`.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG` (NULL args).

## Commit (SPEC §7.3)

### `int isz_commit(isz_output *out, uint32_t flags)`

Walks every surface bound to `out`, validates each (plane type set,
plane slot set, transform supported by slot, format supported by
slot), reserves plane slots, and hands off to the backend's atomic
commit path. On success, emits `ISZ_MSG_PRESENTED` to every client
whose surface was scanned out and clears per-surface damage. On
failure, the backend rolls back KMS state and the commit releases
only newly-reserved slots; pre-existing reservations persist.

`flags` is a bitmask of:

- `ISZ_COMMIT_NORMAL` (0): wait for next vblank.
- `ISZ_COMMIT_ASYNC`: `DRM_MODE_PAGE_FLIP_ASYNC`, bypassing vblank.
  Only honoured if tearing is enabled for the output (SPEC §7.2);
  otherwise treated as `ISZ_COMMIT_NORMAL`.
- `ISZ_COMMIT_TEST_ONLY`: probe a configuration without producing a
  visible frame (SPEC §12). No `presented` events are sent.

Calling `isz_commit()` while a previous commit on the same output is
in flight returns `ISZ_ERR_COMMIT_PENDING`. The library does not
queue.

Return: `ISZ_OK`, `ISZ_ERR_COMMIT_PENDING`, `ISZ_ERR_COMMIT_FAILED`,
`ISZ_ERR_SURFACE_NO_PLANE_SLOT`, `ISZ_ERR_PLANE_UNAVAIL`,
`ISZ_ERR_TRANSFORM_UNSUPPORTED`, `ISZ_ERR_OUTPUT_DISCONNECTED`,
`ISZ_ERR_INVALID_ARG`, `ISZ_ERR_NO_MEMORY`.

## Thread pool (SPEC §5)

### `int isz_thread_pool_submit(isz_server *srv, isz_work_fn fn, void *ctx)`

Queues `fn(ctx)` on the worker pool and returns a pollable fence fd
(eventfd in semaphore mode, `EFD_CLOEXEC`). Poll the fd; it becomes
readable when the work completes. Close it after.

Cancellation is not supported (SPEC §5): any object referenced by
`ctx` must stay valid until the fence signals. Destroying an object
while a worker still references it is undefined behaviour.

When `ENABLE_THREAD_POOL=0` at build time, the function runs `fn`
inline on the calling thread and returns an already-signalled eventfd.
When the pool allocation failed at `isz_init`, the function returns
-1.

This is the only public function safe to call from a non-main thread.
Workers call `fn`; they do not call back into the library. If a
worker needs to trigger a commit, queue a work item whose body runs
on the main thread.

Return: a non-negative fence fd, or -1 on failure (NULL srv, NULL fn,
no pool, or eventfd exhaustion).

## Events (SPEC §5, §9)

### `int isz_add_listener(isz_server *srv, enum isz_event_type type, isz_event_listener_fn fn, void *userdata)`

Registers `fn` to be called for every event of `type`. Multiple
listeners per type are allowed; they fire in registration order.
Listeners are called only from the main dispatch thread, so they do
not need to be thread-safe.

`type` is one of the `ISZ_EVENT_*` constants (see isz.h for the full
list). `userdata` is opaque to the library.

The `isz_event` struct is opaque in the public header. The internal
layout (defined in `src/input/isz_seat_internal.h`) is a tagged union
with `type` and `time_ns` (CLOCK_MONOTONIC_RAW) headers. Public
accessors are not yet exposed; until they land, listeners include the
internal header to read fields. A future minor version will add
`isz_event_get_*` accessors so callers stay off internal headers.

Return: `ISZ_OK`, `ISZ_ERR_INVALID_ARG`, or `ISZ_ERR_NO_MEMORY`.

### `int isz_remove_listener(isz_server *srv, enum isz_event_type type, isz_event_listener_fn fn)`

Removes every listener matching `(type, fn)` from the registry.
`userdata` is not matched; if you registered the same `fn` with two
different `userdata` values, both are removed.

Safe to call from inside a listener callback for the same `type`:
the dispatch loop walks by node pointer, so unlinking mid-walk is
fine. Newly-added listeners do not fire for the event currently being
dispatched.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG`.

## Screen capture (SPEC §6.11, §7.11)

### `int isz_output_capture_start(isz_output *out, int dmabuf_fd, isz_buffer_desc *desc)`

Programs the writeback connector to write composited frames into the
provided DMA-BUF. The library treats the writeback connector as a
separate CRTC enabled with a mode matching `out` (SPEC §7.11). The
DMA-BUF is library-owned until `isz_output_capture_stop()` returns
it via `ISZ_MSG_CAPTURE_DONE` on the wire.

Portal consent (SPEC §6.11) must be granted before this call
succeeds. The Architect wires the consent UI and calls
`isz_capture_grant(srv, out)` when the user approves; the grant is
valid for 60 seconds. Without a valid grant, this function returns
`ISZ_ERR_ACCESS_DENIED` and closes the fd.

Ownership: `dmabuf_fd` transfers to the library on every path. On
failure the library closes the fd; on success the library owns it
until `capture_stop`.

Return: `ISZ_OK`, `ISZ_ERR_ACCESS_DENIED`, `ISZ_ERR_INVALID_ARG`
(NULL out, negative fd, NULL desc, already capturing),
`ISZ_ERR_RESOURCE_LIMIT` (capture table full).

### `int isz_output_capture_stop(isz_output *out)`

Stops capture on `out` and emits `ISZ_MSG_CAPTURE_DONE` to the client
with the DMA-BUF fd and descriptor. After this call the client owns
the fd again.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG` (NULL out or not
capturing).

### `void isz_capture_grant(isz_server *srv, isz_output *output)`

Grants capture consent for `(srv, output)`, valid 60 seconds. Call
this from your consent UI (keybinding, dialog, D-Bus prompt). The
library checks the grant lazily at `isz_output_capture_start` time;
expired entries are reclaimed on the next check or grant.

Not declared in isz.h but exported via the version script. Prototype
is in `src/render/isz_surface_internal.h`.

Return: none.

## Logging (SPEC §4, §12)

### `void isz_set_log_callback(isz_log_fn fn, void *userdata)`

Registers `fn` to receive log lines. The library never writes to
stderr or stdout directly (SPEC §12). `ISZ_LOG_LEVEL` environment
variable (read once at first log call) filters by level:
`error|warn|info|debug`. Default is `warn`.

`fn` receives `(userdata, level, msg)` where `level` is
`ISZ_LOG_ERROR`, `_WARN`, `_INFO`, or `_DEBUG`, and `msg` is a
NUL-terminated string. The callback may be invoked from any thread;
the atomic stores in `isz_set_log_callback` pair with the atomic
loads in `isz_log_internal` so updates propagate safely.

Setting `fn` to NULL disables logging.

Return: none.

### `const char *isz_strerror(int err)`

Translates an `ISZ_ERR_*` code to a short string. The strings match
the descriptions in SPEC §7.10. Unknown codes return `"unknown
error"`.

### `const char *isz_version_string(void)`

Returns the compile-time version string (e.g. `"1.1.0"`). Declared in
`<ishizue/version.h>`.

## Crash recovery (SPEC §12)

### `int isz_enable_crash_recovery(isz_server *srv)`

Opts in to crash recovery. Installs a SIGSEGV/SIGABRT/SIGBUS handler
that restores the VT (DRM backend; no-op for headless) and blanks
every CRTC, then re-raises the original signal so any
Architect-installed crash reporter further down the chain still runs.

The handler never calls `exit()`. It is async-signal-safe: no
malloc, no logging, no pthread mutexes. State is snapshotted at
enable time so the handler never calls back into the server.

Off by default to avoid clashing with an Architect-supplied crash
reporter.

Return: `ISZ_OK` or `ISZ_ERR_INVALID_ARG` (NULL srv or sigaction
failure).

## Test hooks (SPEC §4)

Only present when built with `-DISHIZUE_ENABLE_TEST_HOOKS`. Release
builds strip these symbols. The Makefile's `test` target adds the
flag.

### `isz_test_client *isz_test_connect(isz_server *srv, const char *fake_binary_path)`

Creates a socketpair, hands one end to the server-side connection
machinery via `isz_conn_create`, marks the conn as allowlisted and
handshake-done, and emits `ISZ_EVENT_CLIENT_CONNECT`. The peer end
is kept but unused; event injection goes straight through the
listener registry, no wire bytes flow.

`fake_binary_path` is checked against the binary allowlist by
`(st_dev, st_ino)`; allowlist a real file first.

Return: a test client handle, or NULL if `srv`/path is NULL, the
allowlist rejects the path, or allocation fails.

### `void isz_test_send_key(isz_test_client *client, uint32_t keycode, bool press)`

Injects a keyboard key event. Synchronous: the listener fires before
the call returns.

### `void isz_test_send_pointer_motion(isz_test_client *client, int x, int y)`

Injects a pointer motion event with relative `dx`/`dy` and no
absolute position. Synchronous.

### `void isz_test_simulate_output_hotplug(isz_server *srv, uint32_t width, uint32_t height)`

Asks the headless backend to create a new virtual output. The
output hook fires, the server wraps the new output, and
`ISZ_EVENT_OUTPUT_ADD` is emitted synchronously. No-op for non-
headless backends.
