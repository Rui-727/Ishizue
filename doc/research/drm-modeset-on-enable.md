# DRM modeset on output enable (W13-A)

Design document for the Ishizue DRM backend's output-enable path. The
current `isz_output_enable` implementation in `src/isz_output.c:335-358`
sets `out->enabled = true`, `out->current_mode = mode`, and returns. No
modeset happens. The screen stays in TTY text mode until the first
surface commit arrives via `isz_commit`, which may be seconds or never
depending on whether a client connects. The user ran `tinyisz --backend
drm` on an Intel N4100 panel (`/dev/dri/card1`, i915) and observed
exactly this: the bridge starts, no errors are logged, the panel never
leaves text mode.

This document walks how wlroots handles the same problem, what the
kernel actually requires for a modeset commit, the dumb-buffer
allocation mechanism, the atomic commit construction, and where the fix
belongs in Ishizue given the SPEC section 1 mechanism-not-policy rule.
A concrete implementation plan with function-level call chains closes
the document. Sources are cited inline as `[N]` and listed at the end.

The W13-B audit (`doc/research/drm-backend-audit.md`) covers eight
related bugs in the DRM backend. This document goes deeper on the
modeset-on-enable question alone and supersedes the sketch in W13-B Bug
H, which assumed the kernel accepts a modeset commit without an FB. It
does not, in practice, on i915.

## 1. How wlroots does it

wlroots splits the work across three files in `backend/drm/`:
`drm.c` (connector lifecycle and state setup), `atomic.c` (the atomic
commit builder), and `renderer.c` (GPU-side buffer allocation). The
flow for an enable-with-modeset on the current master branch is:

1. The compositor calls `wlr_output_state_set_enabled(state, true)` and
   `wlr_output_state_set_mode(state, mode)`, then attaches a buffer via
   `wlr_scene_output_build_state` (or `wlr_output_attach_render` in the
   legacy path), then calls `wlr_output_commit_state`. There is no
   separate `wlr_output_enable` call that triggers a modeset on its own
   in modern wlroots; the legacy `wlr_output_enable` is a setter on
   `output->pending` only [1][2].
2. `wlr_output_commit_state` dispatches to the backend's
   `impl->commit`, which for DRM is `drm_connector_commit`
   (`backend/drm/drm.c`) [1].
3. `drm_connector_commit` calls `drm_connector_test` first. The test
   rejects "enable without buffer" with the message `"Can't enable an
   output without a buffer"` (master branch) [1]. The check is:

   ```c
   if (pending.active) {
       if ((output->pending.committed &
               (WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE)) &&
               !(output->pending.committed & WLR_OUTPUT_STATE_BUFFER)) {
           wlr_drm_conn_log(conn, WLR_DEBUG,
               "Can't enable an output without a buffer");
           return false;
       }
   ```
4. `drm_connector_commit_state` builds a `wlr_drm_connector_state` with
   `state->modeset = true` (because `ENABLED | MODE` is committed), then
   calls `drm_connector_set_mode` [1].
5. `drm_connector_set_mode` allocates a CRTC, logs
   `"Modesetting with WxH@refresh mHz"`, then asserts a framebuffer is
   available on the primary plane via `plane_get_next_fb(plane)`. If
   none is available, on the 0.14 branch it calls
   `drm_surface_render_black_frame` followed by `drm_plane_lock_surface`
   to synthesize one. On master, the compositor is required to attach a
   buffer before commit; the test at step 3 enforces this [1][3].
6. `drm_crtc_page_flip` calls into the backend interface
   (`iface->crtc_commit`), which for atomic is `atomic_crtc_commit` in
   `backend/drm/atomic.c` [4].
7. `atomic_crtc_commit` sets `DRM_MODE_ATOMIC_ALLOW_MODESET` in the
   commit flags when `state->modeset` is true, builds the
   `drmModeAtomicReq`, and calls `drmModeAtomicCommit` [4].

The `drm_surface_render_black_frame` function in 0.14
(`backend/drm/renderer.c:481`) is short and worth quoting in full [3]:

```c
bool drm_surface_render_black_frame(struct wlr_drm_surface *surf) {
    if (!drm_surface_make_current(surf, NULL)) {
        return false;
    }
    struct wlr_renderer *renderer = surf->renderer->wlr_rend;
    wlr_renderer_begin(renderer, surf->width, surf->height);
    wlr_renderer_clear(renderer, (float[]){ 0.0, 0.0, 0.0, 1.0 });
    wlr_renderer_end(renderer);
    return true;
}
```

It makes a GBM surface current (which acquires a buffer from the
swapchain), calls `wlr_renderer_clear` with opaque black RGBA, and
returns. The caller then locks the surface's buffer as the primary
plane's `pending_fb`. The next `drm_crtc_page_flip` attaches that FB to
the atomic request.

The takeaway: wlroots requires a buffer at modeset time. The buffer can
come from the compositor's render pass (master branch, sway's pattern)
or from the library's own `drm_surface_render_black_frame` (0.14
branch). Either way, the kernel gets an FB on the primary plane during
the modeset commit. The kernel does not modeset a CRTC without a
scanout target on real i915 hardware, regardless of what the spec
allows in theory.

`wlr_output_enable` itself does no modeset. On the modern API it is a
thin setter that toggles `state->enabled` in the pending state struct.
The actual commit happens in `wlr_output_commit_state`. This split is
the same shape Ishizue's SPEC section 7.6 has: `isz_output_enable`
sets the mode, `isz_commit` does the modeset. The difference is that
wlroots's compositors always attach a buffer before the first commit,
while Ishizue's tinyisz does not.

## 2. The modeset-without-a-surface problem

When `isz_output_enable(out, mode)` returns, no surface is attached to
the output. The X11 bridge has not connected yet, or it has connected
but no client has created a toplevel. The library has three options
for what to put on screen at this point.

(a) Allocate a black framebuffer, set it as the primary plane FB, and
modeset. The screen goes black immediately. When the first surface
arrives, the next `isz_commit` page-flips to the surface's buffer.

(b) Skip the modeset until a surface arrives. The screen stays in TTY
text mode. The user sees the kernel's console framebuffer, not black.

(c) Modeset with a NULL FB (set `FB_ID = 0` on the primary plane,
`ACTIVE = 1` on the CRTC, `MODE_ID` set, `CRTC_ID` on the connector).
The kernel's behavior here is driver-dependent. The DRM atomic
documentation allows disabling a plane by setting `FB_ID = 0`, but
activating a CRTC with no enabled primary plane is rejected by i915,
amdgpu, and most other drivers with `-EINVAL`. The kernel's
`drm_atomic_check_only` calls `drm_atomic_normalize_zpos` and then
`mode_config->helper_private->atomic_check`, which for modern drivers
goes through `drm_atomic_helper_check_modeset` followed by
`drm_atomic_helper_check_planes`. The latter calls the CRTC's
`atomic_check` which on i915 (`intel_atomic_check`) requires a primary
plane FB when the CRTC is being activated [5].

Option (c) does not work on the target hardware. The kernel rejects
the commit, the screen stays in text mode, and the user sees the same
symptom as the original bug.

wlroots picks option (a). The 0.14 branch's `drm_surface_render_black_frame`
is the library-side implementation; the master branch pushes the
responsibility to the compositor via the "Can't enable an output
without a buffer" test, and sway satisfies it by calling
`wlr_scene_output_build_state` which renders the scene graph (whose
background is opaque black when no surfaces are mapped) into a buffer
attached to the commit state [6].

sway's `apply_resolved_output_configs` in `sway/config/output.c` shows
the pattern explicitly. The flow is:

1. `queue_output_config` sets `state->enabled = true` and
   `state->mode = preferred`, but does not attach a buffer [6].
2. `wlr_output_swapchain_manager_prepare` allocates the swapchain
   (buffer slots) for the new mode.
3. `wlr_scene_output_build_state(scene_output, state, &opts)` renders
   the scene graph into a buffer and attaches it to `state` [6].
4. `wlr_output_commit_state(output->wlr_output, state)` does the
   modeset commit with the buffer attached.

The scene graph contains at minimum the root black background, so the
buffer is always opaque black when no surfaces are mapped. This is how
sway avoids option (b) without writing a separate "black frame"
allocator.

tinywl on the wlroots master branch still uses the legacy
`wlr_output_enable` + `wlr_output_commit` API in `server_new_output`
[7]:

```c
wlr_output_set_mode(wlr_output, mode);
wlr_output_enable(wlr_output, true);
if (!wlr_output_commit(wlr_output)) {
    return;
}
```

This works because the legacy `wlr_output_commit` translates to
`wlr_output_commit_state` with the legacy pending state, and the
renderer's `wlr_output_attach_render` is implicitly invoked through
the legacy commit path's `output->impl->commit` -> `drm_connector_commit`
path. The renderer produces a black frame on the first commit when
`wlr_output_pending` has `ENABLED | MODE` set but no `BUFFER` bit, via
the same `drm_surface_render_black_frame` mechanism. On master, this
fallback was removed in favor of the explicit test, but tinywl was
updated at the same time to call `wlr_output_attach_render` before
commit. The net behavior on both branches: a black frame is on screen
within one vblank of `wlr_output_commit`.

Ishizue's choice is between option (a) with a dumb buffer (CPU
allocation, no GPU renderer needed) and option (a) with a GBM buffer
(GPU allocation, requires the renderer to be initialized). The dumb
buffer approach is simpler and matches the "fail-fast at backend-init
if atomic KMS missing" philosophy: no dependency on GBM or EGL being
available, no per-output renderer initialization at enable time.

## 3. The dumb buffer approach

A DRM dumb buffer is a CPU-mappable scanout buffer allocated via
`drmModeCreateDumbBuffer`. The kernel allocates a GEM handle on the
display GPU, returns the handle, the pitch, and the size in bytes.
The buffer can be mmap'd via `drmModeMapDumbBuffer` and written
directly. It is the lowest-overhead way to get a scanout FB without a
GPU context [8].

For a modeset black frame, the allocation is:

```c
struct drm_mode_create_dumb create = {0};
create.width  = mode->width;
create.height = mode->height;
create.bpp    = 32;  /* XRGB8888 = 4 bytes per pixel */
if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
    return -errno;
}
/* create.handle is the GEM handle.
 * create.pitch is the row pitch in bytes (may be > width * 4 due to alignment).
 * create.size is the total buffer size in bytes. */
```

The buffer's memory is uninitialized. To make it black, mmap it and
memset to zero. Black in XRGB8888 is `0x00000000` (X is ignored, RGB
are zero), so a plain `memset(0)` works.

```c
struct drm_mode_map_dumb mreq = {0};
mreq.handle = create.handle;
if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) {
    /* destroy the dumb buffer, return -errno */
}
void *map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                 drm_fd, mreq.offset);
if (map == MAP_FAILED) { /* ... */ }
memset(map, 0, create.size);
munmap(map, create.size);
```

The `drmModeAddFB2` call wraps the GEM handle in a framebuffer object
the kernel's atomic commit can reference:

```c
uint32_t fb_id = 0;
uint32_t handles[4] = { create.handle, 0, 0, 0 };
uint32_t pitches[4] = { create.pitch,   0, 0, 0 };
uint32_t offsets[4] = { 0,              0, 0, 0 };
uint32_t format = DRM_FORMAT_XRGB8888;  /* 0x34325258 */
if (drmModeAddFB2(drm_fd, mode->width, mode->height, format,
                  handles, pitches, offsets, &fb_id, 0) != 0) {
    /* destroy the dumb buffer, return -errno */
}
```

Format: `DRM_FORMAT_XRGB8888` (fourcc `'X','R','2','4'` = `0x34325258`).
This is the format every KMS driver supports on the primary plane for
scanout. `DRM_FORMAT_ARGB8888` would also work but the alpha channel
adds nothing for a black frame and some drivers reject alpha formats
on the primary plane without an alpha-capable blend mode. The literal
`0x34325258u` is already defined in `src/isz_output.c:56` as
`ISZ_FMT_XRGB8888`, so the dumb-buffer code can reuse that constant.

Size: the mode's `width` and `height`. The dumb buffer is a one-shot;
it is allocated, used as the modeset FB, and freed once the first real
surface commit replaces it. The size matches the mode so the
primary-plane `SRC_W`/`SRC_H`/`CRTC_W`/`CRTC_H` rectangles are
1:1 with the FB dimensions, avoiding any hardware scaling.

When to free: after the modeset commit succeeds and the page-flip
event arrives, the dumb buffer is no longer the active scanout target
once the next commit replaces it. The safe sequence is:

1. Allocate, mmap, memset, add FB. Stash the GEM handle and FB id on
   the output struct (e.g. `out->black_fb_id`, `out->black_handle`).
2. Modeset commit with the black FB on the primary plane. Wait for
   the page-flip event via `read_events`.
3. On the next `isz_commit` that attaches a real surface buffer, the
   atomic builder sets `FB_ID` to the surface's FB. After that commit's
   page-flip event arrives, the black FB is no longer being scanned
   out. Call `drmModeRmFB(drm_fd, black_fb_id)` and
   `drmModeDestroyDumbBuffer(drm_fd, black_handle)`.

The lifetime tracking has to handle the page-flip event correctly.
DRM buffers are reference-counted in the kernel: `drmModeRmFB` decrements
the FB's refcount, but the FB stays alive as long as a CRTC is
scanning it out. Calling `drmModeRmFB` while the FB is still the
active scanout target is safe (the kernel keeps it alive), but the
cleaner pattern is to defer the free until after the replacement
page-flip event. This matches how wlroots's `drm_fb_move` and the
`in_flight_releases` list in Ishizue's `isz_drm_event.c:86-103`
already track buffer releases.

The dumb buffer does not need a modifier. `drmModeAddFB2` (without
the `_MODIFIERS` variant) assumes linear layout, which is what
`drmModeCreateDumbBuffer` always produces. Some display drivers
require a modifier on the primary plane for tiled formats, but dumb
buffers are always linear, so the modifier-less `drmModeAddFB2` is
correct here.

## 4. The atomic commit path

The minimum viable modeset commit needs property sets on three KMS
objects: the connector, the CRTC, and the primary plane. The full
construction in wlroots's `atomic_crtc_commit` (`backend/drm/atomic.c`
lines 220-280 on master) is the reference [4]. The minimum subset for
a black-screen modeset is:

On the connector:

- `CRTC_ID` = `crtc_id` (binds the connector to the CRTC)

On the CRTC:

- `MODE_ID` = `blob_id` (a property blob containing the
  `drmModeModeInfo` struct)
- `ACTIVE` = `1` (powers on the CRTC)

On the primary plane:

- `FB_ID` = `fb_id` (the framebuffer to scan out)
- `CRTC_ID` = `crtc_id` (binds the plane to the CRTC)
- `SRC_X` = `0`, `SRC_Y` = `0` (source rectangle top-left, in 16.16
  fixed point)
- `SRC_W` = `width << 16` (source width, fixed point)
- `SRC_H` = `height << 16` (source height, fixed point)
- `CRTC_X` = `0`, `CRTC_Y` = `0` (destination rectangle top-left, in
  pixels)
- `CRTC_W` = `width` (destination width, pixels)
- `CRTC_H` = `height` (destination height, pixels)

The commit flags must include `DRM_MODE_ATOMIC_ALLOW_MODESET`. Without
it, the kernel rejects any commit that changes the CRTC's mode,
active state, or connector binding with `-EINVAL`. wlroots's
`atomic_crtc_commit` adds the flag when `state->modeset` is true [4]:

```c
if (modeset) {
    flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
} else if (!test_only) {
    flags |= DRM_MODE_ATOMIC_NONBLOCK;
}
```

For a non-blocking modeset, the flags are
`DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK |
DRM_MODE_PAGE_FLIP_EVENT`. The `NONBLOCK` flag means the commit
returns immediately and the page-flip event arrives later on the DRM
fd. The `PAGE_FLIP_EVENT` flag requests that event. Without
`PAGE_FLIP_EVENT`, the kernel completes the modeset synchronously and
no event arrives, but the CRTC is still modeset. For Ishizue's
state machine (`READY -> COMMITTING -> READY` via `read_events`), the
`PAGE_FLIP_EVENT` flag is required so `isz_drm_event_dispatch` can
transition the backend out of `COMMITTING`.

The mode blob creation is critical. `drmModeCreatePropertyBlob(fd,
&mode, sizeof(drmModeModeInfo), &blob_id)` registers the mode with the
kernel and returns a blob id. The blob must be destroyed with
`drmModeDestroyPropertyBlob` after the commit completes (or after a
test commit, since test commits do not consume the blob). The
`drmModeModeInfo` struct must be the full struct with correct
timings, not a fabricated one with `clock = refresh / 1000` and zero
blanking intervals. The W13-B audit's Bug E documents this in detail:
the current code in `isz_drm_atomic.c:263-273` fabricates the mode
from width/height/refresh and the kernel rejects it [9]. The fix is
to store the full `drmModeModeInfo` from `drmModeGetConnector`'s
`conn->modes[i]` at snapshot time and use it directly for the blob.

Optional properties that the modeset commit may also set, depending
on what the Architect has configured before `isz_output_enable`:

- `VRR_ENABLED` on the CRTC (if the connector is `vrr_capable` and
  the Architect enabled VRR).
- `DEGAMMA_LUT`, `CTM`, `GAMMA_LUT` on the CRTC (color management
  blobs, if set).
- `HDR_OUTPUT_METADATA` on the connector (if HDR is enabled).
- `rotation`, `zpos` on the primary plane (if the surface has a
  transform or zpos set; for the black-frame modeset these are
  default `DRM_MODE_ROTATE_0` and `0`).

For the minimum viable black-screen modeset, none of these are
required. The connector's `CRTC_ID`, the CRTC's `MODE_ID` and
`ACTIVE`, and the primary plane's `FB_ID`, `CRTC_ID`, and the four
`SRC_*`/`CRTC_*` rectangles are sufficient.

The primary-plane `SRC_W` and `SRC_H` are in 16.16 fixed point. This
is a KMS quirk: the source rectangle is in 16.16 to allow sub-pixel
source cropping for hardware scaling. For a 1:1 scanout with no
scaling, the values are `width << 16` and `height << 16`. The
destination rectangle `CRTC_W` and `CRTC_H` are in plain pixels. This
mismatch is a common source of off-by-65536 bugs; wlroots's
`set_plane_props` in `atomic.c` gets it right [4].

## 5. Where the fix belongs in Ishizue

Two approaches compete, both defensible against SPEC section 1.

Approach A: `isz_output_enable` triggers the modeset directly. After
validating the mode and setting `out->enabled = true`, it calls into
the DRM backend's atomic commit path with `ALLOW_MODESET` and a
black dumb buffer. The screen goes black within one vblank of
`isz_output_enable` returning.

Approach B: `isz_output_enable` stays a flag-set. The Architect must
call `isz_commit(out, 0)` afterward to drive the modeset. The commit
path detects "output enabled but no surface attached" and substitutes
the black dumb buffer for the primary-plane FB.

SPEC section 1 says: "At every point where the library could be
tempted to 'help' by making a decision (cursor fallback, memory
eviction, crash recovery, plane sharing), it exposes the mechanism
and leaves the decision to the Architect instead of making it
silently." [10]

The relevant question for the modeset-on-enable case is: whose
decision is it to put a black frame on screen at enable time? Under
Approach A, the library decides. Under Approach B, the Architect
decides (by calling `isz_commit`).

But there is a counter-argument. The API name is `isz_output_enable`.
"Enable" means "make the output active". An output that stays in TTY
text mode after `isz_output_enable` returns is not enabled by any
reasonable definition. The library is not deciding to enable; the
Architect is. The library is implementing what "enable" means, which
requires a modeset, which requires a buffer, which is black because
there is no surface yet. The black frame is not policy; it is a
physical requirement of KMS.

Compare to SPEC section 10's existing language: "The library never
lights up a new display on its own; that would be policy." This refers
to *connector hotplug*: when a new connector appears, the library
surfaces `ISZ_EVENT_OUTPUT_ADD` and waits for the Architect to call
`isz_output_enable`. The library does not auto-enable. That is the
policy decision the SPEC delegates to the Architect.

Once the Architect has called `isz_output_enable`, the policy decision
is made. The library's job is to execute it. Executing it requires a
modeset. The modeset requires a buffer. The buffer is black. None of
this is policy; it is the mechanism of "enable".

The same logic applies to `isz_output_disable`. When the Architect
calls `isz_output_disable`, the library should disable the CRTC (set
`ACTIVE = 0`, `CRTC_ID = 0` on the connector, `FB_ID = 0` and
`CRTC_ID = 0` on the primary plane) with `ALLOW_MODESET`. The screen
goes to the connector's DPMS-off state. SPEC section 10 already says
the library does not auto-disable on hot-unplug, which is the
policy decision; the disable call itself is the mechanism.

Recommendation: Approach A, with the modeset inside `isz_output_enable`
on the DRM backend. The headless backend's `isz_output_enable` stays
a flag-set (no real modeset needed; the test backend has no hardware
to modeset). The DRM backend's `isz_output_enable` does the modeset
directly with a black dumb buffer.

The trade-off against SPEC section 1 is acceptable because the black
frame is the minimum viable content for a modeset, not a policy choice
between alternatives. The alternative (skip the modeset, stay in text
mode) is a non-functional output. The library is not deciding "black
vs wallpaper vs splash screen"; it is implementing "the output is
enabled, here is the minimum content the hardware requires to accept
the modeset".

A weaker alternative that puts more control in the Architect's hands:
keep `isz_output_enable` as a flag-set, but have `isz_dispatch`
detect "output enabled but no modeset yet" and auto-commit the black
frame on the next dispatch iteration. This is worse than Approach A
because it adds a hidden state machine inside `isz_dispatch` that the
Architect cannot see or control. Approach A is explicit: the modeset
happens inside `isz_output_enable`, the Architect can observe the
return code, and the state machine is the existing
`READY -> COMMITTING -> READY` cycle.

Approach B (defer to `isz_commit`) is the second-best option. It
matches wlroots's modern API shape (setter + separate commit) and
keeps the modeset inside the existing commit path. The cost is that
every Architect must remember to call `isz_commit(out, 0)` after
`isz_output_enable`, or the screen stays dark. tinyisz already
forgets this (W13-B Bug H [9]). Making the library do the right thing
in `isz_output_enable` avoids the footgun.

## 6. The connector-not-found case

The user's machine has `/dev/dri/card1` as the Intel N4100 iGPU, not
`/dev/dri/card0`. The W13-B audit did not flag this as a bug because
the open path does scan `card0` through `card7`. But the scan order
matters: the code opens the first card that succeeds, not the first
card with a connected display.

The current open path in `src/backend/isz_drm.c:478-516`:

```c
for (int i = 0; i < 8; i++) {
    int rc = snprintf(path, sizeof(path), "/dev/dri/card%d", i);
    ...
#ifdef ISHIZUE_HAVE_LIBSEAT
    if (st->seat) {
        int fd = open_drm_via_libseat(st, path);
        if (fd >= 0)
            return fd;
        continue;
    }
#endif
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0)
        return fd;
}
return -1;
```

If `/dev/dri/card0` exists and opens successfully, the loop returns
`card0`'s fd. It never tries `card1`. On a machine with two DRM
devices (for example, an Intel iGPU at `card1` and a USB display
adapter or a virtio-gpu at `card0`), the code opens `card0` and stops.
If `card0` has no connected display, the connector enumeration finds
zero connected connectors, `fire_output_hook` is never called, no
`isz_output` is created, and the screen stays in text mode.

The user's report says the panel is at `card1`. This means either
`card0` does not exist (unlikely on a typical x86 machine, where the
kernel assigns the lowest free minor number), `card0` exists but
cannot be opened (permission denied without libseat, or libseat
refuses to open it), or `card0` exists, opens successfully, but has
no connected display. The third case is the bug.

Fix options:

(1) Scan all cards, count connected connectors on each, pick the one
with the most. This handles every multi-GPU layout but requires
opening every card, calling `drmModeGetResources` and
`drmModeGetConnector` on each, then closing the losers. Adds latency
to backend init.

(2) Prefer the card whose driver is in a known-good list (i915,
amdgpu, nouveau, radeon, msm, panfrost, etc.). Read the driver name
via `drmGetVersion(fd)->name` after opening. Skip cards whose driver
is `vgem`, `vkms`, `udl`, or other non-display drivers. This is
fast and matches what most compositors do implicitly.

(3) Require the Architect to specify the card via `ISZ_DRM_NODE` env
var. The code already supports this (`isz_drm.c:480-496`). Document
it as the workaround for multi-GPU machines.

(4) Try the first openable card. If it has zero connected connectors,
close it and try the next. This is option (1) with a short-circuit:
stop at the first card that has at least one connected connector.

Option (4) is the right default. It keeps the fast path (single-GPU
machines where `card0` is the only card and has a connected panel)
and fixes the multi-GPU case (scan until a card with a connected
display is found). The fallback to "first openable card with no
connected displays" only happens on headless machines, where the
library should still init (for testing) even with no display.

Libseat interaction: `libseat_open_device` takes a path and returns
an fd. If the path does not exist or libseat refuses to grant access,
it returns a negative value. The `continue` in the current code falls
through to the next `i`, so libseat does not interfere with the scan
order. The bug is purely the "first openable wins" logic, not a
libseat issue.

There is a separate libseat concern: `libseat_open_seat(NULL, st)` in
`isz_drm_init` (`isz_drm.c:539`) passes `NULL` for the seat listener.
Without a listener, libseat cannot deliver `enable_seat` / `disable_seat`
events to the backend. The `libseat_dispatch(st->seat, 0)` call at
line 544 drains pending events but with no listener registered, there
is nothing to receive. This means VT switch notifications may not
arrive, and `drmSetMaster` / `drmDropMaster` on VT switch may not fire
correctly. This is a separate bug from the connector-not-found case
but worth noting because it affects whether the DRM backend can
recover master after a VT switch.

For the user's immediate symptom (panel at `card1` not found), the
fix is option (4). The user can also work around it with
`ISZ_DRM_NODE=/dev/dri/card1` in the environment, which the existing
code path at `isz_drm.c:486-496` handles.

## 7. Concrete implementation plan

The fix touches four files: `src/isz_output.c` (the public
`isz_output_enable` function), `src/backend/isz_drm.c` (a new
`isz_drm_modeset_output` helper), `src/backend/isz_drm_atomic.c` (the
commit builder, to handle the no-surface case and the ALLOW_MODESET
flag), and `src/backend/isz_drm.h` (new function declarations and
state fields).

Step 1: Add state fields to `struct isz_output` in
`src/isz_server_internal.h`.

- `uint32_t drm_mode_blob_id` (cached mode blob id, destroyed on
  output destroy).
- `drmModeModeInfo drm_mode` (the full mode struct, copied from
  `drmModeGetConnector` at wrap time).
- `bool modeset_pending` (set by `isz_output_enable`, cleared by the
  post-commit path).
- `uint32_t black_fb_id`, `uint32_t black_handle` (the dumb buffer
  for the black-frame modeset; both 0 when no black buffer is
  allocated).

Step 2: Add `isz_drm_modeset_output` to `src/backend/isz_drm.c`. This
function takes `struct isz_backend *b` and `struct isz_output *out`,
allocates the dumb buffer, builds the atomic request, and submits
with `ALLOW_MODESET`. Sketch:

```c
int isz_drm_modeset_output(struct isz_backend *b, struct isz_output *out)
{
    struct isz_drm_state *st = b->impl;
    /* 1. Allocate the dumb buffer (section 3 above). */
    /* 2. mmap, memset to 0, munmap. */
    /* 3. drmModeAddFB2 with DRM_FORMAT_XRGB8888. */
    /* 4. Stash black_fb_id and black_handle on out. */
    /* 5. Call isz_drm_atomic_commit(st, out, ISZ_COMMIT_MODESET). */
    /*    The commit builder uses out->black_fb_id as the primary plane
     *    FB when no surface is attached. */
    /* 6. On success: set out->modeset_pending = false. */
    /* 7. On failure: free the dumb buffer, return the error. */
}
```

Step 3: Add a new commit flag `ISZ_COMMIT_MODESET` to `include/ishizue/isz.h`.
This flag tells the atomic builder to use `ALLOW_MODESET` and to use
the output's black FB when no surface is attached. It is an
internal-only flag (not for Architect use); the existing public flags
`ISZ_COMMIT_NORMAL`, `_ASYNC`, `_TEST_ONLY` are unchanged.

Step 4: Modify `isz_drm_atomic_commit` in `src/backend/isz_drm_atomic.c`
to handle the modeset case.

- Add `ALLOW_MODESET` to `commit_flags` when `flags &
  ISZ_COMMIT_MODESET` is set or when `out->modeset_pending` is true.
- When `primary_surf` is NULL (no surface attached), use
  `out->black_fb_id` as the primary plane FB. The rest of the primary
  plane property sets (`SRC_*`, `CRTC_*`) use the mode's width and
  height.
- When `primary_surf` is non-NULL, use the surface's buffer as today.
  The black FB is freed after the page-flip event arrives (the
  release path in `isz_drm_event.c` already handles per-buffer
  release tracking).
- Use `out->drm_mode` (the full `drmModeModeInfo` struct) for the
  mode blob, not the fabricated mode. This is the W13-B Bug E fix.

Step 5: Modify `isz_output_enable` in `src/isz_output.c` to call the
DRM backend's modeset path after setting the flag.

```c
ISZ_API int isz_output_enable(isz_output *out, isz_mode *mode)
{
    if (!out || !mode)
        return ISZ_ERR_INVALID_ARG;
    /* Validate mode is one of the output's modes. */
    bool found = false;
    for (size_t i = 0; i < out->mode_count; i++) {
        if (out->modes[i] == mode) { found = true; break; }
    }
    if (!found)
        return ISZ_ERR_INVALID_ARG;

    out->current_mode = mode;
    out->enabled      = true;
    out->dpms         = ISZ_DPMS_ON;
    out->modeset_pending = true;

    /* Headless backend: no real modeset. */
    if (out->is_headless)
        return ISZ_OK;

    /* DRM backend: trigger the modeset directly. */
    if (out->is_drm && out->backend && out->backend->type == ISZ_BACKEND_DRM) {
        int rc = isz_drm_modeset_output(out->backend, out);
        if (rc < 0) {
            out->enabled = false;
            out->current_mode = NULL;
            out->modeset_pending = false;
            return rc;
        }
    }
    return ISZ_OK;
}
```

Step 6: Add the `drm_crtc_mask` lookup to the
`isz_server_wrap_drm_output` helper (which W13-B Bug A adds). The
wrapper must set `out->drm_crtc_mask` by looking up the CRTC index in
`st->crtcs[]` so the primary-plane lookup at `isz_drm_atomic.c:234`
works. W13-B Bug C covers the `pick_crtc_for_connector` fix that
makes the CRTC id non-zero in the first place.

Step 7: Free the black FB after the first real surface commit
replaces it. In `isz_drm_event.c`'s `page_flip_handler`, after
`isz_backend_finish_commit`, check if the just-completed commit was
the modeset (the next commit will replace it). The simplest approach:
track `out->black_fb_id` and free it in the next commit's pre-commit
path (after the new FB is attached but before the atomic commit
submits). The kernel's FB refcount keeps the black FB alive until
the page-flip completes, so freeing it early is safe.

Step 8: Fix the connector-not-found case (section 6 above). Modify
`open_primary_drm_node` in `src/backend/isz_drm.c` to scan all cards
and pick the first one with at least one connected connector. If no
card has a connected connector, fall back to the first openable card
(so the headless case still works for testing).

Call chain from `isz_output_enable` to the actual
`drmModeAtomicCommit`:

```
isz_output_enable (src/isz_output.c)
  -> isz_drm_modeset_output (src/backend/isz_drm.c)
       -> allocate dumb buffer, drmModeAddFB2
       -> isz_drm_atomic_commit (src/backend/isz_drm_atomic.c)
            -> drmModeCreatePropertyBlob (mode blob)
            -> drmModeAtomicAlloc
            -> drmModeAtomicAddProperty (connector CRTC_ID,
                                         CRTC MODE_ID + ACTIVE,
                                         primary plane FB_ID + CRTC_ID
                                         + SRC_* + CRTC_*)
            -> drmModeAtomicCommit (ALLOW_MODESET | NONBLOCK
                                    | PAGE_FLIP_EVENT)
       -> on success: state -> COMMITTING, return ISZ_OK
       -> on failure: free dumb buffer, return error
  -> on failure: rollback out->enabled, return error
```

The page-flip event arrives on the DRM fd via `isz_drm_event_dispatch`,
which calls `page_flip_handler` in `src/backend/isz_drm_event.c:64`.
The handler calls `isz_backend_finish_commit` to transition
`COMMITTING -> READY`. The dumb buffer stays alive until the next
`isz_commit` replaces it with a real surface buffer; the page-flip
event for that replacement commit is where the dumb buffer can be
freed (via `drmModeRmFB` and `drmModeDestroyDumbBuffer`).

The implementation depends on the W13-B audit's Bug A (output hook
wiring), Bug C (CRTC picking), and Bug E (mode blob from real
`drmModeModeInfo`) being fixed first. Without those, the modeset
commit will fail for unrelated reasons and the black-frame logic
cannot be tested in isolation. The fix order in W13-B's "Fix order"
section is the right sequence; the modeset-on-enable work slots in
at step 7 (after bugs A, B, C, E, G are fixed) and uses the
ALLOW_MODESET flag from bug D.

## References

[1] wlroots master branch, `backend/drm/drm.c`. Fetched from the
GitHub mirror at
https://raw.githubusercontent.com/swaywm/wlroots/master/backend/drm/drm.c
on 2026-07-21. The canonical repo is at
https://gitlab.freedesktop.org/wlroots/wlroots but is behind a bot
check; the GitHub mirror tracks it. Key functions cited:
`drm_connector_test` (the "Can't enable an output without a buffer"
check), `drm_connector_commit_state`, `drm_connector_set_mode`,
`drm_connector_alloc_crtc`, `plane_get_next_fb`.

[2] wlroots `include/wlr/types/wlr_output.h` and the legacy
`wlr_output_enable` / `wlr_output_commit` API. Documentation at
https://wlroots.pages.freedesktop.org/wlroots/wlr/types/wlr_output.h.html
fetched 2026-07-21. The modern API is `wlr_output_state_set_enabled`
plus `wlr_output_commit_state`; the legacy API is preserved as a
compat shim.

[3] wlroots 0.14.1 tag, `backend/drm/renderer.c` and
`backend/drm/drm.c`. Fetched from
https://raw.githubusercontent.com/swaywm/wlroots/0.14.1/backend/drm/renderer.c
and `.../backend/drm/drm.c` on 2026-07-21. The 0.14 branch is the
last release before the move to gitlab.freedesktop.org and the last
with the in-library `drm_surface_render_black_frame` fallback. The
function body is quoted in section 1.

[4] wlroots master branch, `backend/drm/atomic.c`. Fetched from
https://raw.githubusercontent.com/swaywm/wlroots/master/backend/drm/atomic.c
on 2026-07-21. Key functions cited: `atomic_crtc_commit` (the
ALLOW_MODESET flag logic and the property set construction),
`set_plane_props` (the SRC_* / CRTC_* fixed-point convention),
`create_mode_blob`.

[5] Linux kernel source, `drivers/gpu/drm/drm_atomic.c` and
`drivers/gpu/drm/i915/display/intel_atomic.c`. The kernel's atomic
check path is `drm_atomic_check_only` -> `drm_atomic_helper_check`
-> `drm_atomic_helper_check_modeset` ->
`drm_atomic_helper_check_planes` -> CRTC `atomic_check`. On i915,
`intel_atomic_check` requires a primary plane FB when the CRTC is
being activated. Kernel source at
https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm
fetched 2026-07-21.

[6] sway master branch, `sway/config/output.c`. Fetched from
https://raw.githubusercontent.com/swaywm/sway/master/sway/config/output.c
on 2026-07-21. Key functions cited: `queue_output_config`,
`apply_resolved_output_configs` (the swapchain manager prepare and
`wlr_scene_output_build_state` call sequence),
`output_repaint_timer_handler` (the per-frame commit path).

[7] wlroots master branch, `tinywl/tinywl.c`. Fetched from
https://raw.githubusercontent.com/swaywm/wlroots/master/tinywl/tinywl.c
on 2026-07-21. The `server_new_output` function shows the legacy
`wlr_output_enable` + `wlr_output_commit` API in use.

[8] Linux kernel, DRM dumb buffer API. `drm_mode_create_dumb`,
`drm_mode_map_dumb`, and `drm_mode_destroy_dumb` ioctls documented in
https://drm.pages.freedesktop.org/drm-userspace-tools/dumb_buffer_create.html
fetched 2026-07-21. The manpage for `drmModeCreateDumbBuffer` in
libdrm is at `drm-memory(7)` and `drm-kms(7)` in the libdrm docs.

[9] Ishizue W13-B audit, `doc/research/drm-backend-audit.md`. Eight
bugs in the DRM backend found on real hardware (Intel N4100, i915).
Bug A (output hook not wired), Bug C (CRTC picking), Bug D
(ALLOW_MODESET missing), Bug E (fabricated mode blob), Bug H
(isz_output_enable does not call isz_commit) are the ones this
document builds on. The W13-B sketch of Bug H assumed the kernel
accepts a modeset without an FB; section 2 of this document
corrects that.

[10] Ishizue SPEC, section 1 ("What this is"). Fetched from
`/home/z/my-project/repos/Ishizue/SPEC.md` at the repo's current
HEAD. The governing philosophy quote is at line 15: "bare minimum,
mechanism-only, zero unilateral policy decisions."

[11] Ishizue SPEC, section 10 ("Backend abstraction & multi-GPU").
The state machine `UNINITIALIZED -> READY -> COMMITTING -> READY`
and the `isz_output_enable` contract are at lines 589-595. The
"library never lights up a new display on its own; that would be
policy" line is at line 593.

[12] Ishizue source files read for this document:
`src/isz_output.c` (the `isz_output_enable` function at line 335),
`src/backend/isz_drm.c` (the open path at line 478, the connector
enumeration at line 626), `src/backend/isz_drm_atomic.c` (the atomic
commit builder at line 177), `src/backend/isz_drm_event.c` (the
page-flip handler at line 64), `src/backend/isz_drm.h` (the state
struct), `src/backend/isz_drm_atomic.h` (the property cache),
`src/render/isz_commit.c` (the public `isz_commit` function),
`src/isz_server_internal.h` (the `struct isz_output` layout),
`src/backend/isz_backend.c` (the state machine), and
`tinyisz/tinyisz.c` (the Architect's seed-outputs path at line 247).
