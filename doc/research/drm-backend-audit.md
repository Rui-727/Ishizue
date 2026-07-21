# DRM backend real-hardware audit (W13-B)

Target: Intel N4100, Gemini Lake, i915, `/dev/dri/card1`, run from a TTY
with sddm stopped. Symptom: `tinyisz --backend drm` starts, the X11
bridge starts, the screen never leaves TTY text mode, no errors are
logged. The DRM backend never modesets.

The audit walks the init path, the connector-to-output wiring, the
output-enable path, and the atomic commit path. Eight bugs found. Bug A
is the root cause of the reported symptom. Bugs B through H would bite
once Bug A is fixed.

## Reproduction

```
sudo systemctl stop sddm
sudo ./tinyisz --backend drm
```

Expected: panel leaves TTY text mode, goes to the tinyisz blank
compositor. Observed: panel stays in TTY text mode. No error text on
stderr. The bridge process is alive.

## Bug A (root cause): the DRM output hook is never registered

Files: `src/isz_lifecycle.c:163-193`, `src/backend/isz_drm.c:637-638`,
`src/backend/isz_drm.c:360-364`.

The headless backend path in `isz_init` wires its output hook before
wrapping the default output:

```c
if (backend == ISZ_BACKEND_HEADLESS) {
    isz_headless_set_output_hook(srv->backend, isz_headless_output_hook, srv);
    ...manual wrap of each headless output...
}
```

The DRM backend path does not do the equivalent:

```c
if (backend == ISZ_BACKEND_DRM) {
    isz_drm_set_server(srv->backend, srv);
}
```

It calls `isz_drm_set_server` only. It never calls
`isz_drm_set_output_hook`. There is no `isz_drm_output_hook` callback
in `isz_lifecycle.c` analogous to `isz_headless_output_hook`. There is
no `isz_server_wrap_drm_output` helper in `src/isz_output.c` analogous
to `isz_server_wrap_headless_output`. The `struct isz_output` fields
`drm_connector_id`, `drm_crtc_id`, `drm_crtc_mask`, `is_drm` exist in
`src/isz_server_internal.h:209-212` but nothing ever populates them.

The DRM backend's `isz_drm_init` (`src/backend/isz_drm.c:626-641`)
enumerates connectors during init and calls `fire_output_hook` for each
connected one. `fire_output_hook` (`src/backend/isz_drm.c:360-364`)
starts with:

```c
if (!st->hook_fn)
    return;
```

Because the hook is never registered, every call is a silent no-op. No
`isz_output` wrapper is ever created. `srv->outputs` stays empty.

Downstream effect in `tinyisz/tinyisz.c:247-257`:

```c
size_t n = 0;
isz_output **list = isz_output_list(srv, &n);
for (size_t i = 0; i < n; i++) { ... }
```

`n` is 0. The seed-outputs loop does nothing. The `on_output_add`
listener registered at `tinyisz.c:259` never fires because no
`ISZ_EVENT_OUTPUT_ADD` event is ever emitted. No output is enabled. No
commit is ever attempted. No modeset happens. The screen stays in TTY
text mode.

This matches the user's observation exactly: init succeeds, bridge
starts, no errors logged, no modeset.

What it should do: register a DRM output hook in `isz_init` that wraps
each connected connector into an `isz_output`, mirroring the headless
path. The minimal change has three parts.

1. Add `isz_server_wrap_drm_output` in `src/isz_output.c`, mirroring
   `isz_server_wrap_headless_output`. It takes `isz_server *srv` and
   `const struct isz_drm_output_info *info`, allocates an `isz_output`,
   fills `drm_connector_id`, `drm_crtc_id`, `is_drm`, `hdr_capable`,
   `vrr_capable`, copies EDID, builds one `isz_mode` from `info`'s
   width/height/refresh, pushes onto `srv->outputs`, emits
   `ISZ_EVENT_OUTPUT_ADD`. The `drm_crtc_mask` field must be set by
   looking up the CRTC index in the backend's `st->crtcs[]` (see Bug C
   for why this lookup needs a fix too).

2. Add `isz_drm_output_hook` in `src/isz_lifecycle.c`, mirroring
   `isz_headless_output_hook`. It calls `isz_server_wrap_drm_output` on
   add and marks the wrapper `disconnected` on remove.

3. In `isz_init`'s DRM branch, call
   `isz_drm_set_output_hook(srv->backend, isz_drm_output_hook, srv)`
   before `isz_drm_set_server`. See Bug B for why ordering matters.

## Bug B: initial connector enumeration runs before the hook can be wired

File: `src/backend/isz_drm.c:626-641` (called from `isz_drm_init`),
`src/isz_lifecycle.c:157` (`isz_backend_create` runs `isz_drm_init`).

`isz_drm_init` runs inside `isz_backend_create`, which `isz_init` calls
at line 157. The DRM branch's hook wiring would happen at line 190,
after `isz_backend_create` has already returned. By that point
`isz_drm_init` has already enumerated connectors and tried to fire the
hook (silently, per Bug A).

Even if Bug A is fixed by adding `isz_drm_set_output_hook` to
`isz_init`, the initial enumeration's `fire_output_hook` calls happen
while `st->hook_fn` is still NULL. Only future hotplug rescans would
fire the hook. The panel is already connected at boot, so its
`OUTPUT_ADD` is lost.

What it should do: pick one of two approaches.

(a) Defer enumeration. Move the connector-snapshot loop out of
`isz_drm_init` into a new `isz_drm_enumerate_connectors(struct
isz_backend *)`. `isz_init` calls `isz_drm_set_output_hook` then
`isz_drm_set_server` then `isz_drm_enumerate_connectors`.

(b) Replay on hook register. Have `isz_drm_set_output_hook` walk the
existing `st->connectors[]` cache and call `fire_output_hook` for each
connected entry. This mirrors how `isz_init` manually wraps each
headless output at `isz_lifecycle.c:165-181` after setting the hook.

(b) is the smaller diff. Either works.

## Bug C: `pick_crtc_for_connector` returns 0 for any unbound connector

File: `src/backend/isz_drm.c:335-354`.

```c
static uint32_t pick_crtc_for_connector(struct isz_drm_state *st,
                                        drmModeConnector *conn)
{
    if (!conn->encoder_id)
        return 0;
    drmModeEncoder *enc = drmModeGetEncoder(st->drm_fd, conn->encoder_id);
    ...
}
```

`drmModeConnector.encoder_id` is the currently bound encoder. On a
freshly mastered DRM device where no userspace modeset has happened,
`encoder_id` is 0. The function returns 0. Every connector gets
`crtc_id = 0` stored.

The atomic commit rejects this at `src/backend/isz_drm_atomic.c:219-220`:

```c
if (!connector_id || !crtc_id)
    return ISZ_ERR_INVALID_ARG;
```

On the N4100 panel after `systemctl stop sddm`, the i915 driver has
typically released the connector. `encoder_id` is 0. So even with Bugs
A and B fixed, every connector snapshot would carry `crtc_id = 0`, and
every commit would return `ISZ_ERR_INVALID_ARG` before reaching the
kernel.

What it should do: iterate `conn->encoders[]` (length
`conn->count_encoders`) instead of relying on the bound encoder. For
each encoder, look up its `possible_crtcs` mask and pick the first CRTC
whose bit is set and that is not already taken by another connector.
Sketch:

```c
for (int e = 0; e < conn->count_encoders; e++) {
    drmModeEncoder *enc = drmModeGetEncoder(st->drm_fd, conn->encoders[e]);
    if (!enc) continue;
    for (size_t i = 0; i < st->crtc_count; i++) {
        if ((enc->possible_crtcs & (1u << i)) &&
            !crtc_in_use(st, st->crtcs[i].crtc_id)) {
            drmModeFreeEncoder(enc);
            return st->crtcs[i].crtc_id;
        }
    }
    drmModeFreeEncoder(enc);
}
return 0;
```

The wrapper (Bug A fix) then sets `out->drm_crtc_mask` to
`st->crtcs[i].bitmask` for the chosen index, so the primary-plane
lookup at `isz_drm_atomic.c:234` works.

## Bug D: atomic commit never sets `DRM_MODE_ATOMIC_ALLOW_MODESET`

File: `src/backend/isz_drm_atomic.c:490-497`.

```c
commit_flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
if (flags & ISZ_COMMIT_TEST_ONLY) {
    commit_flags = DRM_MODE_ATOMIC_TEST_ONLY;
}
if (flags & ISZ_COMMIT_ASYNC) {
    commit_flags |= DRM_MODE_PAGE_FLIP_ASYNC;
}
```

The first commit after `isz_output_enable` sets `ACTIVE=1`, `MODE_ID`,
and the connector `CRTC_ID`. All three change the CRTC topology or
mode. The kernel rejects such commits with `-EINVAL` unless
`DRM_MODE_ATOMIC_ALLOW_MODESET` is in the flags. The commit at line 499
returns `-EINVAL`, the log at line 501 fires at `ISZ_LOG_ERROR`, but
per Bug G the user sees nothing.

Subsequent page-flip commits (same mode, same connector, only the FB
changes) do not need `ALLOW_MODESET`. The flag should be added only
when the commit changes mode, active state, or connector binding.

What it should do: track a `modeset_pending` flag on `struct
isz_output`. Set it in `isz_output_enable` and whenever the mode
changes. The atomic builder adds `DRM_MODE_ATOMIC_ALLOW_MODESET` when
the flag is set, and clears the flag after a successful commit.
Sketch:

```c
if (out->modeset_pending || !(flags & ISZ_COMMIT_PAGE_FLIP))
    commit_flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
```

`isz_output_enable` sets `out->modeset_pending = true`. The
post-commit path clears it on `rc == ISZ_OK`.

## Bug E: the mode blob is fabricated from width/height/refresh, not from the real EDID mode

File: `src/backend/isz_drm_atomic.c:263-273`.

```c
mode.clock       = out->current_mode->refresh_mhz ?
                   out->current_mode->refresh_mhz / 1000u : 60000;
mode.hdisplay    = out->current_mode->width;
mode.vdisplay    = out->current_mode->height;
mode.hsync_start = out->current_mode->width;
mode.hsync_end   = out->current_mode->width;
mode.htotal      = out->current_mode->width;
mode.vsync_start = out->current_mode->height;
mode.vsync_end   = out->current_mode->height;
mode.vtotal      = out->current_mode->height;
```

Two problems.

1. `clock` is wrong. `drmModeModeInfo.clock` is the pixel clock in kHz.
   `out->current_mode->refresh_mhz` is `vrefresh * 1000` (set at
   `isz_drm.c:243`). For a 60 Hz mode, `refresh_mhz = 60000`, so
   `refresh_mhz / 1000u = 60`. The blob gets `clock = 60`, meaning
   60 kHz. A 1920x1080 panel at 60 kHz would compute to roughly 0.03
   Hz. The kernel rejects the mode or, worse, programs the PLL to
   garbage.

2. `hsync_end == htotal == hdisplay` means zero horizontal blanking.
   `vtotal == vdisplay` means zero vertical blanking. Real panels
   require blanking intervals. The kernel's mode validator rejects
   modes with no front-porch, sync, or back-porch.

The real `drmModeModeInfo` is available at `conn->modes[0]` during
`snapshot_connector` (`isz_drm.c:239-244`) but is discarded. Only the
width, height, and refresh fields are kept; the rest of the struct is
thrown away.

What it should do: store the full `drmModeModeInfo` (or all of
`conn->modes[]`) at snapshot time. Add a `drmModeModeInfo mode` field
to `struct isz_drm_connector` and to `struct isz_output` (or a
`uint32_t mode_blob_id` cached at snapshot). The atomic builder creates
the blob from the stored `drmModeModeInfo` directly:

```c
drmModeModeInfo mode = out->drm_mode;  /* full struct, copied at wrap time */
drmModeCreatePropertyBlob(st->drm_fd, &mode, sizeof(mode), &blob_mode_id);
```

This preserves the panel's real timings.

## Bug F: connected-but-modeless connector produces a 0x0@0 output

File: `src/backend/isz_drm.c:239-244`.

```c
if (out->connected && conn->count_modes > 0) {
    drmModeModeInfo *m = &conn->modes[0];
    out->width       = m->hdisplay;
    out->height      = m->vdisplay;
    out->refresh_mhz = m->vrefresh ? m->vrefresh * 1000u : 60000u;
}
```

If `connected == true` but `count_modes == 0`, the `if` is false.
`out->width`, `out->height`, `out->refresh_mhz` stay 0 (from the
`memset` at line 229). The hook still fires because line 637 only
checks `connected`. The wrapper would create an `isz_output` with zero
modes. `tinyisz.c:142` checks `modes && mn > 0` and skips enabling it.
No modeset.

On the N4100's eDP panel the kernel usually has a mode from the panel's
fixed EDID, so `count_modes > 0`. On a hotplugged HDMI display where
the EDID read is still in flight, `count_modes` can be 0 at first
probe. The rescan path (`isz_drm_rescan_connectors`) would eventually
pick it up, but only if something triggers a rescan. The event-dispatch
code at `src/backend/isz_drm_event.c:138` pins to v2 and does not see
hotplug events (comment at lines 146-150).

What it should do: if `connected` but `count_modes == 0`, log at
`ISZ_LOG_WARN` with the connector id and skip firing the hook for that
connector. A later rescan that finds modes will fire it. Also wire the
v3 `sequence_handler` so kernel hotplug events trigger rescans without
relying on an external udev watcher.

## Bug G: tinyisz never registers a log callback, so `isz_log_internal` is a silent no-op

File: `tinyisz/tinyisz.c` (whole file), `src/util/isz_log.c:54-56`.

```c
isz_log_fn cb = atomic_load_explicit(&s_callback, memory_order_acquire);
if (cb == NULL)
    return;
```

The default `s_callback` is NULL. `tinyisz.c` never calls
`isz_set_log_callback`. Every `isz_log_internal` call in the library,
including all `ISZ_LOG_ERROR` messages from the DRM backend, is a
silent no-op. The user sees "no errors logged" because there is no log
sink, not because nothing failed.

Even the headless path's `tinyisz: backend=headless 1280x720@60` line
at `tinyisz.c:214` is a direct `fprintf`, not an `isz_log_internal`
call, so it shows up. But nothing from the library does.

What it should do: register a stderr sink in `tinyisz`'s `main`,
before `isz_init`:

```c
static void tinyisz_log(void *ud, enum isz_log_level lvl, const char *msg) {
    (void)ud;
    const char *tag = "??";
    switch (lvl) {
    case ISZ_LOG_ERROR: tag = "E"; break;
    case ISZ_LOG_WARN:  tag = "W"; break;
    case ISZ_LOG_INFO:  tag = "I"; break;
    case ISZ_LOG_DEBUG: tag = "D"; break;
    }
    fprintf(stderr, "isz[%s]: %s\n", tag, msg);
}

int main(...) {
    ...
    isz_set_log_callback(tinyisz_log, NULL);
    ...
}
```

And document `ISZ_LOG_LEVEL=info` for users who want to see init
messages. Default `s_max_level` is `ISZ_LOG_WARN`, so the user would
see warnings and errors. Setting `ISZ_LOG_LEVEL=info` would surface the
`drm init: fd=... crtcs=... planes=... connectors=...` line at
`isz_drm.c:647-649`.

## Bug H: `isz_output_enable` does not call into the backend, and `isz_commit` is never invoked on the seed path

File: `src/isz_output.c:335-358`, `tinyisz/tinyisz.c:247-257`,
`src/render/isz_commit.c:101-191`.

`isz_output_enable` sets `out->enabled = true`, `out->current_mode =
mode`, `out->dpms = ISZ_DPMS_ON`, and returns. It does not call
`isz_commit`. The comment at line 351-353 says the modeset lives in the
render wave's `drmModeAtomicCommit`. But the render wave's commit only
runs when something calls `isz_commit`. The seed-outputs path in
`tinyisz.c:247-257` calls `isz_output_enable` then
`tinyisz_ctx_add_output`. It never calls `isz_commit`.

For the headless backend this is fine: no real modeset is needed, the
first frame commit (triggered by the bridge drawing a window) happens
later. For the DRM backend, the panel needs a modeset before any frame
is visible, and the first frame commit might not happen for seconds
(the bridge has to connect, the client has to create a surface, etc.).
The user sees a black or TTY-text screen until then.

Also, `isz_commit` at `src/render/isz_commit.c:124-128` walks
`collect_output_surfaces`. If no surface is bound to the output (the
common state right after `isz_output_enable`), `nsurf == 0`, the
reservation loop is skipped, and `isz_backend_commit` is still called
at line 160. So `isz_commit(out, 0)` with no surfaces does reach the
backend. But nobody calls it.

What it should do: after `isz_output_enable` returns `ISZ_OK` on a DRM
output, the seed path should call `isz_commit(out, 0)` to drive the
initial modeset. The atomic builder must handle the no-surface case:
currently `isz_drm_atomic.c:368` skips the primary-plane FB assignment
if `primary_surf` is NULL, so the commit sets `ACTIVE=1` and `MODE_ID`
without an FB. The kernel accepts a modeset with no FB (it blanks the
CRTC to the background color). The first frame commit later attaches
the FB via a page-flip.

Sketch for `tinyisz.c:247-257`:

```c
for (size_t i = 0; i < n; i++) {
    size_t mn = 0;
    isz_mode **modes = isz_output_get_modes(list[i], &mn);
    if (modes && mn > 0 &&
        isz_output_enable(list[i], modes[0]) == ISZ_OK) {
        tinyisz_ctx_add_output(&ctx, list[i]);
        (void)isz_commit(list[i], 0);  /* drive initial modeset */
    }
}
```

The `isz_commit` call needs `ALLOW_MODESET` (Bug D) and a real mode
blob (Bug E) to actually take.

## Fix order

1. Bug A. Without the output hook wired, nothing else can be tested.
   Add `isz_server_wrap_drm_output`, `isz_drm_output_hook`, and the
   `isz_drm_set_output_hook` call in `isz_init`.
2. Bug B. Pick approach (b): have `isz_drm_set_output_hook` replay
   `fire_output_hook` for cached connected connectors.
3. Bug G. Register the stderr log sink in `tinyisz`. Without this, the
   next bugs' failures stay invisible.
4. Bug C. Fix `pick_crtc_for_connector` to iterate `conn->encoders[]`.
5. Bug E. Store the full `drmModeModeInfo` at snapshot and use it for
   the blob.
6. Bug D. Add `ALLOW_MODESET` on the modeset-pending commit.
7. Bug H. Call `isz_commit(out, 0)` from the seed path after enable.
8. Bug F. Skip modeless connected connectors and log a warning.

After bugs A through E plus G and H are fixed, `tinyisz --backend drm`
should leave TTY text mode and show the blank compositor background on
the N4100 panel. Bug F is a separate fix for hotplug and EDID read
timing, not on the critical path for the reported symptom.

## Verification checklist

After the fixes:

- `tinyisz --backend drm` prints `isz[I]: drm init: fd=N crtcs=...
  planes=... connectors=1` on stderr.
- The panel leaves TTY text mode within one vblank of startup.
- `isz_output_list(srv, &n)` returns `n == 1` before the seed loop.
- `isz_commit(out, 0)` returns `ISZ_OK` on the seed path.
- `drmModeAtomicCommit` does not return `-EINVAL`.
- `ISZ_LOG_LEVEL=debug` shows the `drm atomic: commit failed` path is
  not hit.
