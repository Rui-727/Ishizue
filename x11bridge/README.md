# x11bridge

X11 compatibility scaffold for Ishizue. A separate process that acts
as an ordinary Ishizue client (SPEC §13), listening on an X11 socket
and translating X11 requests into Ishizue surface, buffer, and input
operations.

This is a scaffold, not a working X server. Most of the X11 protocol
is not implemented. The bridge exists so the X11 compatibility path
is in place before the per-message dispatch wave of the Ishizue
library lands.

## What it does

- Connects to the Ishizue Unix socket and completes the §6.2
  handshake from the client side.
- Listens on `/tmp/.X11-unix/X<display>` (default `:0`).
- Accepts X11 client connections and replies with a real
  `setup_success` (one screen, one pixmap format, one TrueColor
  visual of depth 24).
- Parses and dispatches thirty-six core opcodes end-to-end:
  - `CreateWindow` (1): allocate an Ishizue surface, set position
    and size.
  - `ChangeWindowAttributes` (2): store event-mask,
    override-redirect, cursor, bit/win-gravity, backing-store,
    colormap, save-under, do-not-propagate-mask, background-pixel.
  - `GetWindowAttributes` (3): reply with class, visual, map-state,
    map-installed, override-redirect, colormap, event-mask,
    do-not-propagate-mask from per-window state.
  - `DestroyWindow` (4): destroy the Ishizue surface, free the XID,
    recurse into children.
  - `MapWindow` (8): set_plane_type, set_plane_slot, set_output,
    commit; emit MapNotify.
  - `UnmapWindow` (10): clear_output, commit; emit UnmapNotify; clear
    seat keyboard focus.
  - `ConfigureWindow` (12): set_position, set_size, set_zpos, commit;
    emit ConfigureNotify.
  - `GetGeometry` (14): reply with stored depth and geometry.
  - `QueryTree` (15): reply with the parent XID and the list of
    child XIDs. Root's parent is 0.
  - `InternAtom` (16): bridge-global atom table with predefined
    atoms 1..68; allocate dynamic atoms from 69 up.
  - `GetAtomName` (17): reverse atom lookup, reply with the name
    string. BadAtom error for atom 0 or unknown atoms.
  - `ChangeProperty` (18): store (property, type, format, value) on
    the window; emit PropertyNotify.
  - `DeleteProperty` (19): remove the property from the window;
    emit PropertyNotify (state=Deleted).
  - `GetProperty` (20): reply with stored value, sliced by
    long-offset / long-length.
  - `SetSelectionOwner` (22): store (selection -> owner, timestamp)
    in a per-client table; reject stale timestamps; emit
    SelectionClear to the previous owner if ownership changed.
  - `GetSelectionOwner` (23): reply with the current owner XID
    (0 if none).
  - `QueryPointer` (38): reply with root, child, root-x/y, win-x/y,
    button mask. Headless v1 returns (0, 0) and no buttons.
  - `SetInputFocus` (42): store focus window and revert-to policy;
    translate to isz_seat_set_keyboard_focus on the focused surface
    (or surface 0 to clear).
  - `GetInputFocus` (43): reply with focus=0, revert-to=None.
  - `CreatePixmap` (53): track pixmap geometry; no pixel storage.
  - `FreePixmap` (54): mark the pixmap slot free.
  - `CreateGC` (55): store a graphics context keyed by client-chosen
    XID with the value-list attributes (graphics-exposure,
    foreground, background, line-width, etc.).
  - `FreeGC` (60): mark the GC slot free. BadGC if the XID is not
    tracked.
  - `ClearArea` (61): if the window has a ZPixmap depth-24 backing
    image, paint the rect with background_pixel. width=0 or height=0
    means "to the end". Emit Expose if the exposures flag is set.
  - `CopyArea` (62): validate src, dst, gc; no-op the pixel copy.
    Emit NoExpose if the GC has graphics-exposure=false.
  - `PolyFillRectangle` (69): validate drawable and gc; accept and
    discard the rectangle list. No rendering in v1.
  - `PutImage` (72): stash the image data on the drawable as a
    backing store (window only; root and pixmaps discard).
    GraphicsExposure generation is skipped in v1.
  - `GetImage` (73): for a window with a ZPixmap depth-24 backing
    image, return the backing image rect; elsewhere return zeros.
    Reply depth matches the drawable's depth.
  - `CreateColormap` (78): track colormap XID; no real allocation.
  - `FreeColormap` (79): mark the colormap slot free.
  - `AllocColor` (84): echo the requested RGB, return a packed
    0x00RRGGBB pixel.
  - `QueryColors` (91): unpack each pixel as 0x00RRGGBB.
  - `LookupColor` (92): return exact RGB = screen RGB = (0,0,0).
  - `QueryExtension` (98): reply present=0 for every name.
  - `ListExtensions` (99): reply with 0 extensions.
  - `NoOperation` (119): silently consumed.
- Forwards Ishizue input events to the first mapped X11 top-level
  window:
  - `ISZ_MSG_INPUT_KEYBOARD_KEY` -> X11 `KeyPress` / `KeyRelease`.
  - `ISZ_MSG_INPUT_POINTER_MOTION` -> X11 `MotionNotify`.
  - `ISZ_MSG_INPUT_POINTER_BUTTON` -> X11 `ButtonPress` /
    `ButtonRelease`.
- Event delivery honors the per-window `event-mask` the client set
  via `ChangeWindowAttributes`. StructureNotify subscribers receive
  MapNotify / UnmapNotify / ConfigureNotify / DestroyNotify;
  PropertyChange subscribers receive PropertyNotify.

## Build

The bridge is not built by the top-level `make all`. Build it
explicitly:

```
make            # in repo root, builds ../libishizue.so
make -C x11bridge
```

The bridge links `../libishizue.so` for the public API and compiles
`../src/protocol/isz_protocol.c` and `../src/protocol/isz_conn.c`
directly into the binary. The framing primitives (`isz_proto_*`,
`isz_conn_*`) are `ISZ_INTERNAL` (hidden) in the `.so`, so the bridge
cannot link them from there. Compiling the `.c` files into the bridge
keeps the framing code in sync with the library without exporting
internal symbols.

## Run

```
# In one terminal: start an Architect that hosts libishizue.so and
# allowlists the bridge binary.
ISZ_SOCKET=/tmp/.ishizue-0 your-architect &

# In another terminal:
ISZ_SOCKET=/tmp/.ishizue-0 ISZ_X11_DISPLAY=0 ./x11bridge/x11bridge
```

Environment variables:

- `ISZ_SOCKET` (default `/tmp/.ishizue-0`): path to the Ishizue UDS.
- `ISZ_X11_DISPLAY` (default `0`): X11 display number to bind.

## Allowlisting the bridge

SPEC §6.3: every connecting Ishizue client is checked against a
binary/cgroup allowlist. The Architect must add the bridge binary to
the allowlist before `isz_init`:

```c
isz_allowlist_add_binary(srv, "/path/to/x11bridge");
```

If the bridge is not in the allowlist, the Ishizue server closes the
connection before sending any handshake bytes. The bridge logs
`handshake: server sent version 0 (closed?)` and exits.

The allowlist resolves the path to `(st_dev, st_ino)` at call time,
so the binary may be rebuilt in place without re-registering it. A
path move or replacement through a different inode requires
re-registering.

## What is stubbed (the long list)

The following are known gaps, not bugs:

- No X11 error messages for unsupported opcodes. The bridge silently
  drops opcodes it does not handle (PolyText, ImageText, CopyPlane,
  vector drawing other than PolyFillRectangle, and most others). Real
  X11 clients that block on a reply from those opcodes will stall.
  Reply-bearing opcodes that the bridge does handle (GetWindowAttributes,
  GetGeometry, QueryTree, InternAtom, GetAtomName, GetProperty,
  GetSelectionOwner, QueryPointer, GetImage, AllocColor, QueryColors,
  LookupColor) work end-to-end.
- No focus tracking. Forwarded input events go to the first mapped
  top-level window across all X11 clients. SetInputFocus stores the
  focus XID and translates it to a seat_set_keyboard_focus wire
  message, but the bridge does not yet consult the focus state when
  routing input events.
- No buffer translation. The bridge sends no `ATTACH_BUFFER`
  messages. PutImage stashes the bytes on the window's backing-store
  slot but never attaches them via `isz_surface_attach_buffer`.
  Surfaces are positioned and sized but never painted.
- No cursor translation. The bridge stores the cursor XID per
  window but never calls `isz_seat_set_cursor_surface`.
- No selection / clipboard data transfer. SetSelectionOwner and
  GetSelectionOwner track ownership; ConvertSelection (24),
  SelectionRequest (30), SelectionNotify (31) are not wired.
- No X11 extension support (XKB, XInput2, RANDR, etc.).
- No abstract-socket bind. Linux abstract namespace
  (`@/tmp/.X11-unix/X<n>`) is not bound; libX11 falls back to the
  filesystem path, but clients that hard-code the abstract path will
  not connect.
- No `XAUTHORITY` handling. The bridge accepts any
  `authorization-protocol-name` / `authorization-protocol-data` in
  the setup_request and discards it.
- Provisional wire payload layouts. The per-message payload formats
  for `ISZ_MSG_SURFACE_*`, `ISZ_MSG_COMMIT`, and
  `ISZ_MSG_INPUT_*` are not yet formalized in `isz_protocol.h`
  (Wave 3's dispatch is a stub). The bridge picks layouts based on
  SPEC §6 and §7 and will need updating when the per-message
  dispatch wave pins them down. See `isz_client.h` for the layouts
  in use.
- Surface id allocation is client-side. The protocol says the server
  allocates object ids (SPEC §6.4), but the wire reply carrying the
  assigned id is not yet defined. The bridge allocates ids starting
  at 1 and sends them in the `SURFACE_CREATE` payload; when the
  protocol gains a real reply, the bridge will switch to using the
  server-assigned id.
- Output id is 0 until the §6.5 globals broadcast is wired. The
  handshake stub in `isz_handshake.c` does not emit any globals yet,
  so the bridge has no real output id to bind surfaces to. `COMMIT`
  messages are sent with `output_id = 0` to exercise the wire path;
  the server will reject them until a real output exists.
- No inter-client event delivery. Events that would normally go to a
  different client (e.g. SubstructureNotify on the root for a window
  another client created) are dropped. The bridge delivers events
  only to the client that issued the trigger request.
- No CreateNotify. The bridge does not emit CreateNotify for new
  windows; the test does not require it.
- No GraphicsExposure from CopyArea. CopyArea (62) emits NoExpose
  when the GC has graphics-exposure=false, but does not emit
  GraphicsExposure when graphics-exposure=true and the source region
  is "lost". The bridge has no real backing-store loss tracking, so
  it emits nothing in the graphics-exposure=true case. PutImage (72)
  never emits GraphicsExposure or NoExpose.
- No ConvertSelection. The bridge tracks selection ownership but
  does not route ConvertSelection (24) to the owner or generate
  SelectionRequest / SelectionNotify events.

## Files

```
x11bridge/
  main.c             entry point, epoll loop, signal handling
  x11_proto.h/.c     X11 wire protocol constants, structs, builders
                     for setup_success, error, GetGeometry,
                     GetWindowAttributes, QueryTree, InternAtom,
                     GetAtomName, GetProperty, GetSelectionOwner,
                     QueryPointer, MapNotify, UnmapNotify,
                     ConfigureNotify, DestroyNotify, PropertyNotify,
                     SelectionClear, SelectionRequest, SelectionNotify,
                     GraphicsExposure, NoExpose, Expose
  x11_atoms.h/.c     bridge-global atom table (predefined 1..68 +
                     dynamic allocation from 69 up)
  x11_client.h/.c    per-X11-client state, setup handshake, request
                     parser; dispatches thirty-six handled opcodes;
                     per-window properties, GC table, selection
                     table, colormap table, focus state, PutImage
                     backing store
  isz_client.h/.c    client side of the Ishizue wire protocol
  translation.h/.c   X11 <-> Ishizue mapping (surface create/destroy,
                     set_position/set_size/set_plane_type/
                     set_plane_slot/set_zpos/set_output/clear_output/
                     commit, seat_set_keyboard_focus; event delivery)
  Makefile           builds the x11bridge binary + integration tests
  README.md          this file
  tests/test_x11_handshake.c  W7-C test: setup + CreateWindow +
                              GetGeometry end-to-end
  tests/test_x11_opcodes.c    W8-A test: CreateWindow,
                              ChangeWindowAttributes, MapWindow,
                              InternAtom (WM_PROTOCOLS, PRIMARY),
                              ChangeProperty, GetProperty,
                              ConfigureWindow, UnmapWindow,
                              DestroyWindow end-to-end
  tests/test_x11_opcodes2.c   W9-B test: GetWindowAttributes,
                              QueryTree, GetAtomName, DeleteProperty,
                              SetSelectionOwner, GetSelectionOwner,
                              QueryPointer, SetInputFocus, CreateGC,
                              PutImage end-to-end
  tests/test_x11_colormap.c   W10-B test: CreateColormap,
                              FreeColormap, AllocColor, QueryColors,
                              LookupColor end-to-end
  tests/test_x11_render.c     W10-A test: CreateGC, PutImage,
                              GetImage (verify roundtrip), ClearArea,
                              GetImage (verify zeroed), PolyFillRectangle,
                              CopyArea, FreeGC end-to-end
```

## License

MIT, same as the rest of Ishizue. See `../LICENSE`.
