# Ishizue wire protocol

The binary protocol spoken over the Unix domain socket between the
Ishizue library (server) and its clients. This document is the
authoritative reference for v1; the implementation lives in
`src/protocol/`. SPEC §6 is the design source.

## Transport and framing

Transport is a Unix domain socket. The Architect creates and binds
the listening socket (SPEC §6.1); the library owns `accept()`, the
handshake, the allowlist, and per-client dispatch from there.

Every message is framed as:

```
+----------------+----------------+------------------+
| length (u32 LE)| msg_id (u32 LE)| payload (bytes)  |
+----------------+----------------+------------------+
```

`length` counts the bytes after it: `sizeof(msg_id) + payload_len =
4 + payload_len`. A message with no payload has `length == 4`. Total
wire bytes for one message are `4 + length`. The minimum complete
message is 8 bytes (length=4, msg_id, no payload).

Constants (from `src/protocol/isz_protocol.h`):

| Constant                  | Value | Meaning |
|---|---|---|
| `ISZ_PROTOCOL_MAGIC`      | `0x49535A48` | "ISZH" as a host-order u32 |
| `ISZ_PROTOCOL_MAGIC_STR`  | `"ISZH"` | Bytes on the wire: `49 53 5A 48` |
| `ISZ_PROTOCOL_VERSION`    | `1`    | Current wire protocol version |
| `ISZ_PROTO_MAX_MESSAGE`   | `8192` | Upper bound on total wire bytes |
| `ISZ_PROTO_MAX_FDS`       | `8`    | Upper bound on fds per message |
| `ISZ_MSG_HEADER_SIZE`     | `8`    | `sizeof(length) + sizeof(msg_id)` |

Endianness is little-endian everywhere except the 4-byte connection
magic, which is big-endian so the protocol can be detected by a
byte-order-independent signature. The magic bytes on the wire are
always `49 53 5A 48` (`'I','S','Z','H'`).

The library encodes with `isz_proto_encode` and decodes with
`isz_proto_decode` (both internal, in `isz_protocol.c`). Both go
through explicit `isz_put_u32_le` / `isz_get_u32_le` / `_be`
helpers; do not `memcpy` a `struct isz_msg_header` and expect a
portable wire layout.

## Handshake (SPEC §6.2)

The handshake runs synchronously at `accept()` time on a blocking
socket. The server then switches the socket to `O_NONBLOCK` for
epoll-driven I/O. Steps:

1. Client `connect(2)` to the UDS.
2. Server `accept4()` and reads `SO_PEERCRED` to get the peer pid.
   The allowlist check (§6.3) runs before any byte is sent. Deny
   closes the fd immediately with no data on the wire.
3. Server sends 8 bytes: BE magic + LE server max version.

   ```
   49 53 5A 48   01 00 00 00
   ^^^^^^^^^^^   ^^^^^^^^^^^
   "ISZH" (BE)   version 1 (LE)
   ```

4. Client replies with 4 bytes: LE chosen version. Must be `<=`
   server max. Zero or greater-than-max closes the connection.

   ```
   01 00 00 00     (client picks version 1)
   ```

5. Server broadcasts `ISZ_MSG_GLOBAL` for every existing output and
   the active seat (§6.5). v1 payload is 8 bytes:

   ```
   kind (u32 LE)     object_id (u32 LE)
   ```

   `kind`: `0` = output, `1` = seat. `object_id` is per-connection
   (§6.4), server-assigned. Per-output format/modifier lists land in
   a follow-up; v1 clients bind to the id and query further state
   via per-object requests.
6. Server sends `ISZ_MSG_CAPABILITIES`. Payload is 12 bytes:

   ```
   caps (u32 LE)   max_cursor_w (u32 LE)   max_cursor_h (u32 LE)
   ```

   `caps` is a bitmask of `ISZ_CAP_*` (see below).
7. Server sends `ISZ_MSG_HANDSHAKE_DONE` (no payload). After this
   the client may send normal requests.

Any message received before `handshake_done`, other than the version
reply, is a fatal protocol violation and the server disconnects.

The handshake is implemented in `src/protocol/isz_handshake.c`.
Magic and version helpers are in `src/protocol/isz_protocol.c`.

## Object model (SPEC §6.4)

Object IDs are 32-bit, server-owned, per-connection. The server
allocates an ID when it creates an object (surface, buffer, seat
proxy) and returns it to the client in the response message. Clients
refer to existing objects by these IDs in subsequent requests. IDs
are not global across the server; two clients holding ID 5 see
different objects.

v1 object set (SPEC §6.4): surface, buffer, output, seat
(keyboard/pointer), popup, layer-shell, subsurface, screen capture,
drag-and-drop, clipboard, cursor themes.

Output and seat objects are global singletons (SPEC §6.5). On
connect the server sends `ISZ_MSG_GLOBAL` for each existing output
and the active seat. A hotplugged output (`ISZ_EVENT_OUTPUT_ADD` to
the Architect) also triggers a `global` broadcast to every connected
client. The library does not filter outputs/seats per client;
restricting access is Architect policy at the socket accept layer.

## FD passing via SCM_RIGHTS

DMA-BUF file descriptors travel as ancillary data (`SCM_RIGHTS`) on
the same `sendmsg` call as the message that references them. The
payload carries a 32-bit `fd_index` slot mapping into the cmsg's fd
array. The server does not interpret the slot; it maps
`slot -> cmsg_fds[slot]`.

The receiver uses one `recvmsg` per message. The kernel delivers
the cmsg with the `recvmsg` returning the first byte of the
originating `sendmsg`. If a message is split across `recvmsg` calls,
the fds arrive in the first chunk and are stashed in
`pending_fds` until the rest of the message arrives. The stateful
recv path is `isz_proto_recv_state_read` in `isz_protocol.c`.

Layout of the fd-slot descriptor in the payload (from
`struct isz_msg_fd` in `isz_protocol.h`):

```
fd_index (u32 LE)        payload_offset (u32 LE)
```

`fd_index` is the index into the SCM_RIGHTS fd array carried with
this message. `payload_offset` is the byte offset within the payload
(after the 8-byte header) where this fd belongs. Multiple fd slots
may appear in one payload.

Hard limits: at most `ISZ_PROTO_MAX_FDS` (8) fds per message, at
most `ISZ_PROTO_MAX_MESSAGE` (8192) total wire bytes. Exceeding
either is fatal: the receiver closes the fds it received and
disconnects. `MSG_CTRUNC` from `recvmsg` is treated the same way.

Atomicity: fds are sent with the message that references them. If
the kernel send buffer is full, the message is queued internally
(see Queue depth below) and the fds are `dup()`'d into the queue
node so the caller retains ownership of its originals.

## Capabilities bitmask (SPEC §6.2 step 6)

`ISZ_MSG_CAPABILITIES` payload `caps` field, bitwise OR of:

| Bit | Constant                    | Meaning |
|---|---|---|
| 0   | `ISZ_CAP_HDR`               | HDR metadata supported |
| 1   | `ISZ_CAP_VRR`               | Variable refresh rate supported |
| 2   | `ISZ_CAP_TEARING`           | `DRM_MODE_PAGE_FLIP_ASYNC` honoured when requested |
| 3   | `ISZ_CAP_SCREEN_CAPTURE`    | Writeback connector capture path available |
| 4   | `ISZ_CAP_CURSOR_SIZE_MAX`   | `max_cursor_w`/`max_cursor_h` fields carry real limits |

Capabilities are per-server, not per-output. A client should probe
per-output format/modifier support via the `global` event (and a
follow-up query) rather than assuming the capability bits.

The current handshake implementation sends a zero `caps` bitmask and
zero cursor size; real values come from the DRM backend wave. The
payload layout is fixed.

## Versioning

The wire protocol version is independent of the library's ABI semver
(SPEC §4, §6.1). The current wire version is `1` and lives in
`ISZ_PROTOCOL_VERSION`.

Negotiation: the server sends its max supported version; the client
replies with a version `<=` the server max. A reply of zero or a
version greater than the server max closes the connection (SPEC §6.2
step 4). The negotiated version is stored on the connection
(`conn->version`) and the library uses it to gate versioned message
layouts.

Adding a new message ID is a wire-protocol minor change: a client
that does not understand the new ID simply ignores it (the library
never sends unsolicited messages that require a client response).
Adding a new field to an existing message's payload is a breaking
change requiring a version bump.

## Message table

Every `ISZ_MSG_*` ID from `src/protocol/isz_protocol.h`. Direction
is C→S (client-to-server request) or S→C (server-to-client event).
Both directions share one ID space per SPEC §6.1.

Payload layouts marked "v1 design" are not yet concretely encoded
in the protocol header; the per-message dispatch wave wires them
up. Layouts marked "fixed" are concretely defined and tested.

| ID | Constant | Direction | Payload | SPEC |
|---|---|---|---|---|
| 0 | `ISZ_MSG_INVALID` | - | reserved, never sent | §6.1 |
| 1 | `ISZ_MSG_HANDSHAKE_DONE` | S→C | none | §6.2 step 7 |
| 2 | `ISZ_MSG_GLOBAL` | S→C | `u32 kind, u32 object_id` (fixed) | §6.5 |
| 3 | `ISZ_MSG_CAPABILITIES` | S→C | `u32 caps, u32 max_cursor_w, u32 max_cursor_h` (fixed) | §6.2 step 6 |
| 4 | `ISZ_MSG_SURFACE_CREATE` | C→S | none (v1) | §6.4, §7.6 |
| 5 | `ISZ_MSG_SURFACE_DESTROY` | C→S | `u32 surface_id` (v1 design) | §6.4 |
| 6 | `ISZ_MSG_SURFACE_ATTACH_BUFFER` | C→S | `u32 surface_id, u32 fd_index, <isz_buffer_desc fields>` + 1 SCM_RIGHTS fd (v1 design) | §8 |
| 7 | `ISZ_MSG_SURFACE_DAMAGE` | C→S | `u32 surface_id, u32 count, count * isz_rect` (v1 design) | §7.9 |
| 8 | `ISZ_MSG_SURFACE_SET_POSITION` | C→S | `u32 surface_id, i32 x, i32 y` (v1 design) | §7.6 |
| 9 | `ISZ_MSG_SURFACE_SET_SIZE` | C→S | `u32 surface_id, i32 w, i32 h` (v1 design) | §7.6 |
| 10 | `ISZ_MSG_SURFACE_SET_PLANE_TYPE` | C→S | `u32 surface_id, u32 type` (v1 design) | §7.7 |
| 11 | `ISZ_MSG_SURFACE_SET_PLANE_SLOT` | C→S | `u32 surface_id, i32 slot` (v1 design) | §7.7 |
| 12 | `ISZ_MSG_SURFACE_SET_ZPOS` | C→S | `u32 surface_id, i32 zpos` (v1 design) | §7.6 |
| 13 | `ISZ_MSG_SURFACE_SET_TRANSFORM` | C→S | `u32 surface_id, u32 transform` (v1 design) | §7.2 |
| 14 | `ISZ_MSG_SURFACE_CREATE_SUBSURFACE` | C→S | `u32 parent_id` (v1 design) | §6.6 |
| 15 | `ISZ_MSG_SURFACE_CREATE_POPUP` | C→S | `u32 parent_id, i32 x, i32 y` (v1 design) | §6.7 |
| 16 | `ISZ_MSG_SURFACE_CREATE_LAYER` | C→S | `u32 output_id, u32 layer` (v1 design) | §6.7 |
| 17 | `ISZ_MSG_BUFFER_DESTROY` | C→S | `u32 buffer_id` (v1 design) | §8 |
| 18 | `ISZ_MSG_RELEASE` | S→C | `u32 buffer_id` (fixed) | §8 |
| 19 | `ISZ_MSG_SEAT_SET_KEYBOARD_FOCUS` | S→C | `u32 seat_id, u32 surface_id` (v1 design) | §9 |
| 20 | `ISZ_MSG_SEAT_SET_CURSOR_SURFACE` | S→C | `u32 seat_id, u32 surface_id, u32 hot_x, u32 hot_y` (v1 design) | §6.10 |
| 21 | `ISZ_MSG_COMMIT` | C→S | `u32 output_id, u32 flags` (v1 design) | §7.3 |
| 22 | `ISZ_MSG_OUTPUT_ENABLE` | C→S | `u32 output_id, u32 mode_id` (v1 design) | §10 |
| 23 | `ISZ_MSG_OUTPUT_DISABLE` | C→S | `u32 output_id` (v1 design) | §10 |
| 24 | `ISZ_MSG_OUTPUT_SET_DPMS` | C→S | `u32 output_id, u32 state` (v1 design) | §10 |
| 25 | `ISZ_MSG_OUTPUT_SET_GAMMA` | C→S | `u32 output_id, u32 size, size * u16 * 3` (v1 design) | §7.2 |
| 26 | `ISZ_MSG_OUTPUT_SET_DEGAMMA` | C→S | `u32 output_id, u32 size, size * u16 * 3` (v1 design) | §7.2 |
| 27 | `ISZ_MSG_OUTPUT_SET_CTM` | C→S | `u32 output_id, 9 * float` (v1 design) | §7.2 |
| 28 | `ISZ_MSG_OUTPUT_SET_HDR_METADATA` | C→S | `u32 output_id, u32 size, size * u8` (v1 design) | §7.2 |
| 29 | `ISZ_MSG_CAPTURE_START` | C→S | `u32 output_id, u32 fd_index, <isz_buffer_desc fields>` + 1 SCM_RIGHTS fd (v1 design) | §7.11 |
| 30 | `ISZ_MSG_CAPTURE_STOP` | C→S | `u32 output_id` (v1 design) | §7.11 |
| 31 | `ISZ_MSG_CAPTURE_DONE` | S→C | `u32 output_id, u32 fd_index, <isz_buffer_desc fields>` + 1 SCM_RIGHTS fd (v1 design) | §7.11 |
| 32 | `ISZ_MSG_PRESENTED` | S→C | `u32 surface_id, u64 vblank_ns` (v1 design) | §7.3 |
| 33 | `ISZ_MSG_DRAG_START` | C→S | `u32 source_id, u32 icon_id, u32 mime_count, ...` (v1 design) | §6.9 |
| 34 | `ISZ_MSG_DRAG_MOTION` | S→C | `u32 surface_id, i32 x, i32 y, u64 time_ns` (v1 design) | §6.9 |
| 35 | `ISZ_MSG_DRAG_ACCEPT` | C→S | `u32 surface_id, u32 mime_index` (v1 design) | §6.9 |
| 36 | `ISZ_MSG_DRAG_REJECT` | C→S | `u32 surface_id` (v1 design) | §6.9 |
| 37 | `ISZ_MSG_DRAG_DROP` | S→C | `u32 surface_id, u32 fd_index, ...` + 1 SCM_RIGHTS fd (v1 design) | §6.9 |
| 38 | `ISZ_MSG_POPUP_DISMISS` | C→S | `u32 surface_id` (v1 design) | §6.7 |
| 39 | `ISZ_MSG_CLIPBOARD_SET` | C→S | `u32 mime_len, char[mime_len], u32 fd_index, u64 size` + 1 SCM_RIGHTS fd (v1 design) | §6.8 |
| 40 | `ISZ_MSG_CLIPBOARD_REQUEST` | C→S | none (v1 design) | §6.8 |
| 41 | `ISZ_MSG_INPUT_KEYBOARD_KEY` | S→C | `u32 seat_id, u32 keycode, u8 pressed, u64 time_ns` (v1 design) | §9 |
| 42 | `ISZ_MSG_INPUT_KEYBOARD_MODIFIERS` | S→C | `u32 seat_id, u32 depressed, u32 latched, u32 locked, u32 group` (v1 design) | §9 |
| 43 | `ISZ_MSG_INPUT_POINTER_MOTION` | S→C | `u32 seat_id, i32 dx, i32 dy, u8 has_abs, f64 abs_x, f64 abs_y, u64 time_ns` (v1 design) | §9 |
| 44 | `ISZ_MSG_INPUT_POINTER_BUTTON` | S→C | `u32 seat_id, u32 button, u8 pressed, u64 time_ns` (v1 design) | §9 |
| 45 | `ISZ_MSG_INPUT_POINTER_AXIS` | S→C | `u32 seat_id, f64 dx, f64 dy, u32 source, u64 time_ns` (v1 design) | §9 |
| 46 | `ISZ_MSG_INPUT_TOUCH_DOWN` | S→C | `u32 seat_id, i32 id, f64 x, f64 y, u64 time_ns` (v1 design) | §9 |
| 47 | `ISZ_MSG_INPUT_TOUCH_MOTION` | S→C | `u32 seat_id, i32 id, f64 x, f64 y, u64 time_ns` (v1 design) | §9 |
| 48 | `ISZ_MSG_INPUT_TOUCH_UP` | S→C | `u32 seat_id, i32 id, u64 time_ns` (v1 design) | §9 |
| 49 | `ISZ_MSG_INPUT_TOUCH_FRAME` | S→C | `u32 seat_id, u64 time_ns` (v1 design) | §9 |
| 50 | `ISZ_MSG_ERROR` | S→C | `u32 code, u32 msg_len, char[msg_len]` (v1 design), followed by disconnect | §6.12 |

The library does no keymap interpretation (SPEC §9): keycodes are
raw Linux evdev codes, delivered straight from libinput. Modifiers
and group (active layout) ride on `ISZ_MSG_INPUT_KEYBOARD_MODIFIERS`.

## Fault tolerance (SPEC §6.12)

The server is lenient on the wire: it tolerates or recovers from bad
client data where possible and disconnects only on clearly fatal
protocol violations. Specifics:

- A `length` field below 4 (the size of `msg_id`) is a malformed
  message. The stateful recv returns `(size_t)-1` with `errno =
  EBADMSG` and the server closes the connection.
- A `length` that would push the total message past
  `ISZ_PROTO_MAX_MESSAGE` is fatal with `errno = EMSGSIZE`.
- More than `ISZ_PROTO_MAX_FDS` fds in one cmsg is fatal: the
  receiver closes every fd received in that call and disconnects.
- `MSG_CTRUNC` on `recvmsg` means the cmsg buffer was too small;
  the receiver treats this as fatal and disconnects.
- Any message received before `handshake_done` other than the
  version reply is a fatal protocol violation.

Disconnect cleanup (SPEC §6.12) runs identically for clean close
and abrupt broken pipe:

1. All surfaces belonging to the client are removed from any
   outputs and plane slots they held.
2. All buffers imported by the client are destroyed.
3. Any KMS plane-slot assignments held by those surfaces are
   released.
4. `ISZ_EVENT_CLIENT_DISCONNECT` is sent to the Architect once
   cleanup completes.
5. If the disconnected client held keyboard or pointer focus, the
   library does not reassign it. The Architect decides, via
   `isz_seat_set_keyboard_focus(NULL)` or reassignment to another
   surface. Automatic reassignment would be policy.

The current implementation (Wave 3) closes the conn, frees the
outbound queue (closing any carried fds), discards the partial-read
state (closing its fds), and emits `CLIENT_DISCONNECT`. The
surface/buffer/plane-slot release paths (steps 1-3) land with the
per-message dispatch wave.

## Queue depth and slow clients (SPEC §6.13)

Each client socket has `SO_SNDBUF` set to 262144 bytes
(`ISZ_DEFAULT_SNDBUF`). When the kernel send buffer is full, the
library queues outgoing events internally up to
`ISZ_MAX_EVENTS_PER_CLIENT` (default 1024, build-time). Exceeding
the queue disconnects the client with `ISZ_ERR_CLIENT_TOO_SLOW`
rather than risking unbounded memory growth.

The send path (`isz_conn_send` in `isz_conn.c`) tries a direct
`sendmsg` first. On `EAGAIN`/`EWOULDBLOCK` it encodes the message
into a queue node, `dup()`'s the carried fds into the node, and
appends to the tail. If the queue is full, the conn is marked dead
and the call returns `ISZ_ERR_CLIENT_TOO_SLOW`. Order is preserved:
if anything is queued, every subsequent send goes to the queue
behind it.

The dispatch loop drains the queue via `isz_conn_drain` when the
socket is writable. Partial writes are fatal for that connection:
the kernel may have already sent the cmsg fds, so the receiver
cannot safely resume. The send path returns `(ssize_t)-1` with
`errno = EIO` and the caller marks the conn dead.

## Wire format byte-exact examples

All examples are v1. Hex bytes are shown in wire order; multi-byte
fields are little-endian unless noted. `length` counts
`sizeof(msg_id) + payload_len`.

### handshake_done (S→C, ID 1, no payload)

`length` counts the bytes after itself: `sizeof(msg_id) + payload_len`.
For a no-payload message, `length = sizeof(msg_id) = 4`.

```
04 00 00 00   01 00 00 00
^^^^^^^^^^^   ^^^^^^^^^^^
length = 4    msg_id = 1 (HANDSHAKE_DONE)
```

Total: 8 bytes on the wire.

### capabilities (S→C, ID 3, 12-byte payload)

A server with HDR + VRR + screen capture support, no tearing, max
cursor 64x64:

```
10 00 00 00   03 00 00 00   0B 00 00 00   40 00 00 00   40 00 00 00
^^^^^^^^^^^   ^^^^^^^^^^^   ^^^^^^^^^^^   ^^^^^^^^^^^   ^^^^^^^^^^^
length = 16   msg_id = 3    caps = 0x0B   max_w = 64    max_h = 64
                            (HDR|VRR|
                             SCREEN_CAPTURE)
```

`0x0B = 0b1011 = ISZ_CAP_HDR | ISZ_CAP_VRR | ISZ_CAP_SCREEN_CAPTURE`.
Total: 20 bytes on the wire.

### surface_create (C→S, ID 4, no payload)

```
04 00 00 00   04 00 00 00
^^^^^^^^^^^   ^^^^^^^^^^^
length = 4    msg_id = 4 (SURFACE_CREATE)
```

Total: 8 bytes. The server allocates the surface, assigns a
per-connection object_id, and (in the per-message dispatch wave)
replies over the wire. v1's response message layout is not yet
fixed in the protocol header; the current implementation handles
surfaces created via the public `isz_surface_create()` API
(Architect-side), not via the wire.

### surface_attach_buffer with fd (C→S, ID 6, one SCM_RIGHTS fd)

Attaching a 1920x1080 XRGB8888 buffer (stride 7680, offset 0,
modifier INVALID, alpha NONE) to surface 5, with the dmabuf fd in
cmsg slot 0. v1 design payload layout:

```
u32 surface_id    = 5
u32 fd_index      = 0          # index into the SCM_RIGHTS fd array
u32 width         = 1920
u32 height        = 1080
u32 stride        = 7680
u32 offset        = 0
u32 format        = 0x34325258 # DRM_FORMAT_XRGB8888 = 'X','R','2','4'
u64 modifier      = 0xFFFFFFFFFFFFFFFF # DRM_FORMAT_MOD_INVALID
u8  alpha_mode    = 0          # ISZ_ALPHA_NONE
```

Wire bytes (LE), payload only (after the 8-byte header):

```
05 00 00 00   00 00 00 00   80 07 00 00   38 04 00 00
^^ surface   ^^ fd_index   ^^ width=1920 ^^ height=1080

E0 1D 00 00   00 00 00 00   58 52 32 34   FF FF FF FF
^^ stride    ^^ offset     ^^ format     ^^ modifier lo

FF FF FF FF   00
^^ modifier hi   ^^ alpha_mode
```

Payload is 37 bytes (`4*7 + 8 + 1`). Full wire frame (8-byte header +
37-byte payload) = 45 bytes; `length = 4 + 37 = 41 = 0x29`:

```
29 00 00 00   06 00 00 00   05 00 00 00   00 00 00 00
80 07 00 00   38 04 00 00   E0 1D 00 00   00 00 00 00
58 52 32 34   FF FF FF FF   FF FF FF FF   00
```

The same `sendmsg` carries the dmabuf fd as ancillary data:

```
cmsg_level = SOL_SOCKET
cmsg_type  = SCM_RIGHTS
cmsg_data  = <int: the dmabuf fd>
```

### release (S→C, ID 18, 4-byte payload)

Server tells the client that buffer 7 is no longer in flight and
may be reused or destroyed (SPEC §8):

```
08 00 00 00   12 00 00 00   07 00 00 00
^^^^^^^^^^^   ^^^^^^^^^^^   ^^^^^^^^^^^
length = 8    msg_id = 18   buffer_id = 7
              (RELEASE)
```

Total: 12 bytes. If the client has already destroyed the buffer
before the event arrives, the release is a silent no-op server-side.

### presented (S→C, ID 32, 12-byte payload)

Server tells the client that surface 5 was scanned out at vblank
timestamp 1234567890 ns (CLOCK_MONOTONIC). v1 design payload layout
is `u32 surface_id` + `u64 vblank_ns` (12 bytes). `length = 4 + 12
= 16 = 0x10`:

```
10 00 00 00   20 00 00 00   05 00 00 00   D2 02 96 49 00 00 00 00
^^^^^^^^^^^   ^^^^^^^^^^^   ^^^^^^^^^^^   ^^^^^^^^^^^^^^^^^^^^^^^
length = 16   msg_id = 32   surface_id=5  vblank_ns = 1234567890
              (PRESENTED)
```

Total: 20 bytes. Clients use this to pace rendering and avoid
producing frames faster than the refresh rate (SPEC §7.3).

### global output broadcast (S→C, ID 2, 8-byte payload)

Server broadcasts output 1 to the client during handshake:

```
0C 00 00 00   02 00 00 00   00 00 00 00   01 00 00 00
^^^^^^^^^^^   ^^^^^^^^^^^   ^^^^^^^^^^^   ^^^^^^^^^^^
length = 12   msg_id = 2    kind = 0      object_id = 1
              (GLOBAL)      (output)      (per-connection)
```

Total: 16 bytes. A seat broadcast looks identical with `kind = 1`.
