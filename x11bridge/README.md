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
- Accepts X11 client connections and replies with a minimal
  `setup_success` (one screen, one pixmap format, no visuals).
- Parses the 4-byte request header of each subsequent request and
  routes the few it understands:
  - `CreateWindow` (parent = root) -> allocate a client-side Ishizue
    surface id and send `ISZ_MSG_SURFACE_CREATE`, then
    `ISZ_MSG_SURFACE_SET_POSITION` and `ISZ_MSG_SURFACE_SET_SIZE`.
  - `ConfigureWindow` -> `ISZ_MSG_SURFACE_SET_POSITION` and
    `ISZ_MSG_SURFACE_SET_SIZE` with the new geometry.
  - `MapWindow` -> `ISZ_MSG_SURFACE_SET_OUTPUT` and `ISZ_MSG_COMMIT`.
  - `UnmapWindow`, `DestroyWindow` -> logged, not yet sent over the
    wire.
- Forwards Ishizue input events to the first mapped X11 top-level
  window:
  - `ISZ_MSG_INPUT_KEYBOARD_KEY` -> X11 `KeyPress` / `KeyRelease`.
  - `ISZ_MSG_INPUT_POINTER_MOTION` -> X11 `MotionNotify`.
  - `ISZ_MSG_INPUT_POINTER_BUTTON` -> X11 `ButtonPress` /
    `ButtonRelease`.

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

- No X11 visuals or depths are advertised. The `setup_success`
  contains one root screen with `depths_len = 0`. Real X11 clients
  that need a visual to create a window with a non-default visual
  will fail.
- No reply messages. Most X11 requests expect a reply (InternAtom,
  GetInputFocus, QueryExtension, GetWindowAttributes, GetGeometry,
  ...). The bridge logs and discards them. Real X11 clients that
  block on a reply will stall at the first such request.
- No X11 error messages. The bridge never sends an X11 error event,
  so a client that misbehaves will not learn about it.
- No focus tracking. Forwarded input events go to the first mapped
  top-level window across all X11 clients. Real focus policy is the
  Architect's job (SPEC §1) and would arrive as a focus field on
  the input event once the protocol formalizes it.
- No buffer translation. The bridge sends no `ATTACH_BUFFER`
  messages, so X11 `PutImage` / `CopyArea` / `ShmPutImage` etc. have
  no effect on the Ishizue side. Surfaces are positioned and sized
  but never painted.
- No cursor translation.
- No selection / clipboard translation.
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

## Files

```
x11bridge/
  main.c           entry point, epoll loop, signal handling
  x11_proto.h/.c   minimal X11 wire protocol constants and structs,
                   setup_success builder
  x11_client.h/.c  per-X11-client state, setup handshake, request
                   parser
  isz_client.h/.c  client side of the Ishizue wire protocol
  translation.h/.c X11 <-> Ishizue mapping
  Makefile         builds the x11bridge binary
  README.md        this file
```

## License

MIT, same as the rest of Ishizue. See `../LICENSE`.
