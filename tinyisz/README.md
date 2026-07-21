# tinyisz

A minimal tiling window manager built on the Ishizue library. It is the
Ishizue equivalent of tinywm (X11) or tinywl (Wayland): a real but
small WM that demonstrates the library is usable for its intended
purpose.

## What it does

- Initializes Ishizue with the headless or DRM backend.
- Listens on a Unix domain socket for Ishizue clients.
- Spawns the X11 bridge as a subprocess (the same pattern as
  `tools/isz_compositor`).
- Tracks windows via `ISZ_EVENT_CLIENT_CONNECT` and
  `ISZ_EVENT_CLIENT_DISCONNECT`. Each connected client creates one
  tracked window.
- Tiles windows in a master/stack layout: the master takes the left
  fraction of the output, the stack splits the right fraction equally.
- Handles keyboard focus, keybindings, focus-follows-mouse, VT switch
  pause/resume, and clean shutdown on SIGINT/SIGTERM.

## Build

From the repo root, build the library first, then tinyisz:

```
make           # builds libishizue.so
cd tinyisz
make           # builds tinyisz, links against ../libishizue.so
```

The binary's RUNPATH is `$ORIGIN/..` so it finds the library when run
from `tinyisz/` without installing.

## Run

Headless (default, no privileges needed):

```
./tinyisz --backend headless --bridge ../x11bridge/x11bridge
```

DRM (needs a free VT and DRM master):

```
sudo ./tinyisz --backend drm --bridge ../x11bridge/x11bridge
```

The `--bridge` flag defaults to `$ISZ_X11BRIDGE_BIN` or
`../x11bridge/x11bridge`. X11 clients (xterm, xeyes) connect to the
bridge's X11 socket at `:99` (override with `--x11-display`).

### Smoke test

```
./tinyisz --backend headless --bridge ../x11bridge/x11bridge &
pid=$!
sleep 2
kill -TERM $pid
wait $pid
echo "exit code: $?"
```

Expected: tinyisz starts, the bridge connects, after 2 seconds SIGTERM
triggers a clean shutdown (exit code 0). The `make run` target does
this.

## Keybindings

All bindings use the Super (Mod4) modifier.

| Binding | Action |
|---|---|
| Super+Enter | Spawn xterm (prints "would spawn xterm" if xterm is not on PATH) |
| Super+1..9 | Focus the Nth window |
| Super+J | Cycle focus to the next window |
| Super+K | Cycle focus to the previous window |
| Super+H | Shrink the master area by 5 percent |
| Super+L | Grow the master area by 5 percent |
| Super+Shift+Q | Close the focused window |
| Super+Shift+Esc | Exit tinyisz |

Focus also follows the mouse: moving the pointer over a window focuses
it.

## Layout

Windows are stored in a fixed-size array. Index 0 is the master,
indices 1..N-1 are the stack (top to bottom).

- New window becomes the master; the old master drops to the top of
  the stack.
- When the master is destroyed, the top of the stack becomes the new
  master.
- The master takes the left `master_ratio_pct` of the output width
  (default 50, range 20..80).
- The stack splits the remaining width and the full output height
  equally. The last stack slot absorbs rounding.
- Every window gets `ISZ_PLANE_PRIMARY` and plane slot 0 (v1: the
  bridge composites).
- The focused window gets the highest zpos; the master is next; stack
  windows are below in index order.

## Files

| File | Role |
|---|---|
| `tinyisz.c` | Main entry point, arg parsing, signal handling, dispatch loop, bridge spawn, cleanup |
| `window.c` / `window.h` | Window list: add, remove, focus, cycle, point lookup |
| `layout.c` / `layout.h` | Master/stack tiling: geometry computation, surface positioning, commit |
| `input.c` / `input.h` | Keyboard/pointer event handling, keybinding dispatch, WM context struct |
| `Makefile` | Builds `tinyisz` against `../libishizue.so` |

## Limitations

The public Ishizue API (v1.3.0) does not yet expose a surface-create
event from the bridge. tinyisz works around this by creating its own
`isz_surface` for each tracked window (one per connected client).
This means:

- Each bridge connection creates exactly one window in tinyisz's
  list, regardless of how many X11 windows the bridge is tracking.
- X11 windows created by xterm or xeyes do not appear as separate
  tinyisz windows. The bridge composites them internally.
- Closing a window with Super+Shift+Q destroys tinyisz's
  representative surface, not the underlying X11 window.

When a future wave adds a surface-create event to the public API,
tinyisz can track real bridge surfaces without structural changes to
the layout or input code.

Other gaps:

- Output removal (`ISZ_EVENT_OUTPUT_REMOVE`) is logged but not acted
  on, because the event payload does not yet carry the output pointer
  (`isz_event_get_output` returns NULL). Window relocation off a
  dying output will be added when the payload is wired.
- No config file. All defaults are hardcoded.
- No status bar, no multi-monitor tiling (windows go to the first
  output only).
- The Super and Shift modifier bits are hardcoded (Mod4 = 0x40, Shift
  = 0x01) based on the standard US xkbcommon layout. A non-US layout
  that remaps Mod4 would need a keymap-name lookup.
