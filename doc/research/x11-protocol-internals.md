# X11 wire protocol internals

Research notes for Ishizue's X11 bridge (SPEC §13). The current bridge
scaffold (`x11bridge/x11_proto.h`) only knows eleven opcodes and never
sends a reply. This document is the wire-level reference a future wave
needs to turn that scaffold into a real X server. The Ishizue bridge is
not a translator on top of libX11 or libXCB; it speaks raw bytes on
`/tmp/.X11-unix/X<n>` and has to parse every request, reply, event and
error itself. There is no shortcut.

Sources are cited inline as `[N]` and listed in the References section.
Wire layouts follow the X11 spec convention: the leading number on each
line is the field size in bytes (1, 2, 4, 8, n, p, m, ...); the byte
offset is the running sum of previous field sizes. The Setup section in
particular lists explicit byte offsets `N..M` because the Setup messages
have several 2-byte fields packed together and the offsets are easier to
follow than a running sum. Lengths in 4-byte units are written as
`<length>` and mean the count of 32-bit words after the first 4 bytes
of the message, matching the on-the-wire field. Plain byte counts are
written as "bytes".

The primary source is the X11R7.7 core protocol specification [1]. The
ICCCM [2] defines client/WM conventions. The EWMH spec [3] defines the
`_NET_*` atoms. Extension specs: MIT-SHM [4], X Generic Extension [5],
Present [6], DRI3 v1.0 [7], DRI3 v1.2/v1.3 [8].

## 1. Connection setup

X11 connections run over any reliable byte stream. In practice the
transport is a Unix socket (`/tmp/.X11-unix/X<n>`) or TCP port `6000 + n`
[1]. The bridge binds the Unix socket and accepts connections; TCP is
optional and the scaffold does not enable it.

The client speaks first. Byte 0 of the connection is the byte-order
byte: ASCII `'l'` (0x6C) for little-endian or `'B'` (0x42) for
big-endian [1]. The chosen byte order applies to every 16-bit and 32-bit
field in both directions for the life of the connection. The bridge's
`x11_proto.h` already defines `X11_BYTE_ORDER_LSB` and
`X11_BYTE_ORDER_MSB`.

The full `SetupRequest` is 12 bytes plus the auth strings [1]:

```
0   1   byte-order           0x42 = MSB-first, 0x6C = LSB-first
1       unused
2..3    CARD16 protocol-major-version     (client expects, e.g. 11)
4..5    CARD16 protocol-minor-version     (client expects, e.g. 0)
6..7    CARD16 n = length of authorization-protocol-name
8..9    CARD16 d = length of authorization-protocol-data
10..11  unused
12..    STRING8 authorization-protocol-name, padded to 4-byte boundary
        STRING8 authorization-protocol-data, padded to 4-byte boundary
```

The server replies with one of three messages [1]:

- `SetupFailure` (status 0): connection refused.
- `SetupAuthenticate` (status 2): more auth negotiation needed.
- `SetupSuccess` (status 1): connection accepted.

`SetupFailure` is 8 bytes plus a reason string:

```
0       0 Failed
1       CARD8 n = length of reason
2..3    CARD16 protocol-major-version
4..5    CARD16 protocol-minor-version
6..7    CARD16 (n+p)/4   length in 4-byte units of additional data
8..     STRING8 reason, padded to 4
```

`SetupAuthenticate` is 8 bytes plus a reason:

```
0       2 Authenticate
1..5    unused
6..7    CARD16 (n+p)/4   length in 4-byte units of additional data
8..     STRING8 reason, padded to 4
```

`SetupSuccess` is the one the bridge must produce. The wire layout is
shown below; `x11_proto.h` already has the matching struct but emits a
stub reply with one screen, one pixmap format, no visuals [1]. Field
sizes on the left, byte offsets for the first 8 bytes shown explicitly
because they are tightly packed:

```
1  1 Success              (byte 0)
1  unused                 (byte 1)
2  CARD16 protocol-major-version     (bytes 2..3, server supports, 11)
2  CARD16 protocol-minor-version     (bytes 4..5, server supports, 0)
2  CARD16 8+2n+(v+p+m)/4             (bytes 6..7, length of additional
                                     data in 4-byte units)
        where n = number of pixmap formats
              v = length of vendor string
              m = bytes of screen data (always a multiple of 4)
4  CARD32 release-number              (bytes 8..11)
4  CARD32 resource-id-base            (bytes 12..15)
4  CARD32 resource-id-mask            (bytes 16..19)
4  CARD32 motion-buffer-size          (bytes 20..23)
2  CARD16 v = length of vendor        (bytes 24..25)
2  CARD16 maximum-request-length      (bytes 26..27, in 4-byte units, min 4096)
1  CARD8  number of SCREENs in roots  (byte 28)
1  CARD8  n = number of FORMATs in pixmap-formats   (byte 29)
1  image-byte-order           0 LSBFirst, 1 MSBFirst   (byte 30)
1  bitmap-format-bit-order    0 LeastSignificant, 1 MostSignificant   (byte 31)
1  CARD8  bitmap-format-scanline-unit
1  CARD8  bitmap-format-scanline-pad
1  KEYCODE min-keycode         (>= 8)
1  KEYCODE max-keycode         (<= 255)
4  unused
v  STRING8 vendor, padded to 4
8*n  LISTofFORMAT pixmap-formats
m  LISTofSCREEN roots
```

Each FORMAT is 8 bytes [1]:

```
0  CARD8 depth
1  CARD8 bits-per-pixel      {1, 4, 8, 16, 24, 32}
2  CARD8 scanline-pad        {8, 16, 32}
3..7 unused
```

Each SCREEN is 40 bytes plus variable depth data [1]:

```
0..3   WINDOW  root
4..7   COLORMAP default-colormap
8..11  CARD32  white-pixel
12..15 CARD32  black-pixel
16..19 SETofEVENT current-input-masks
20..21 CARD16  width-in-pixels
22..23 CARD16  height-in-pixels
24..25 CARD16  width-in-millimeters
26..27 CARD16  height-in-millimeters
28..29 CARD16  min-installed-maps
30..31 CARD16  max-installed-maps
32..35 VISUALID root-visual
36     backing-stores   0 Never, 1 WhenMapped, 2 Always
37     BOOL save-unders
38     CARD8 root-depth
39     CARD8 number of DEPTHs in allowed-depths
40..   LISTofDEPTH allowed-depths
```

Each DEPTH is 8 bytes plus 24 bytes per visual [1]:

```
0  CARD8 depth
1  unused
2..3 CARD16 n = number of VISUALTYPEs in visuals
4..7 unused
8..   LISTofVISUALTYPE visuals (24 bytes each)
```

Each VISUALTYPE is 24 bytes [1]:

```
0..3   VISUALID visual-id
4      class   0 StaticGray, 1 StaticColor, 2 TrueColor,
                  3 PseudoColor, 4 GrayScale, 5 DirectColor
5      unused
6..7   CARD16 colormap-entries
8..11  CARD32 red-mask
12..15 CARD32 green-mask
16..19 CARD32 blue-mask
20     CARD8 bits-per-rgb-value
21..23 unused
```

Authorisation. The core protocol deliberately does not specify auth
mechanisms [1]. The de-facto default is MIT-MAGIC-COOKIE-1: a 16-byte
random cookie stored in `~/.Xauthority`, sent by the client in the
`authorization-protocol-name` (the string "MIT-MAGIC-COOKIE-1") and
`authorization-protocol-data` (the 16 bytes) fields of `SetupRequest`.
The server compares byte-for-byte and either accepts or sends
`SetupFailure`. Other historical mechanisms: XDM-AUTHORIZATION-1
(DES-encrypted challenge/response), SUN-DES-1 (DES via secure RPC),
MIT-KERBEROS-5, SECURID, and the host-based fallback (no name, no data)
which the server may honour based on the client host [1]. The Xsecurity
man page documents the wire formats of each [9].

What the bridge must do on accept: read 12 bytes, decode the byte order,
read `n` and `d`, read `pad4(n) + pad4(d)` more bytes, validate the
cookie (or skip validation entirely, as the scaffold does today), then
emit `SetupSuccess` advertising at least one screen with one visual of
depth 24 (TrueColor, 8/8/8/8) so that real X11 clients can pick a
visual and call `CreateWindow` with a non-default visual. The scaffold's
current `setup_success` has `depths_len = 0`, which fails any client
that asks for the visual list.

## 2. Core request format

Every request is a 4-byte header followed by data [1]:

```
0  CARD8 major-opcode
1  CARD8 data          (minor opcode for extension requests, else request-specific)
2..3  CARD16 length    in 4-byte units, includes this header
4..   additional data
```

The length field is the total request length in 4-byte units, including
the 4-byte header. A request with no payload has length 1. The server
must read exactly `4 * length` bytes. If `length` is smaller or larger
than required, the server replies with a `Length` error and discards the
request [1]. `maximum-request-length` (from `SetupSuccess`) is at least
4096 (16384 bytes); BigRequests extends this to a 32-bit length [1].

Opcodes 128 through 255 are reserved for extensions [1]. Extension
requests use the major opcode returned by `QueryExtension` and put a
minor opcode in byte 1 of the header.

Every request on a connection is implicitly assigned a sequence number,
starting at 1 [1]. Replies, errors and events carry the low 16 bits of
the sequence number of the request that triggered them. The bridge must
track this counter per connection.

The core protocol defines 119 request types. The full opcode table
follows, derived from the X11R7.7 protocol encoding appendix [1]. The
"action" column describes what the Ishizue bridge needs to do when it
sees that opcode: handle, stub (reply with a synthetic answer), or
reject with an `Implementation` error.

| Op | Name | Bridge action |
|---:|---|---|
| 0 | (reserved) | reject |
| 1 | CreateWindow | handle: allocate surface, set position/size/output |
| 2 | ChangeWindowAttributes | handle: event-mask, cursor, override-redirect |
| 3 | GetWindowAttributes | reply: synthesise visual, map-state, event-mask |
| 4 | DestroyWindow | handle: destroy surface, free XID |
| 5 | DestroySubwindows | handle: iterate children |
| 6 | ChangeSaveSet | stub: no-op (Ishizue has no save-set) |
| 7 | ReparentWindow | handle: restack, send ReparentNotify |
| 8 | MapWindow | handle: surface.set_output + commit |
| 9 | MapSubwindows | handle: iterate children |
| 10 | UnmapWindow | handle: surface.clear_output |
| 11 | UnmapSubwindows | handle: iterate children |
| 12 | ConfigureWindow | handle: set_position/set_size/set_zpos |
| 13 | CirculateWindow | stub or handle: restack children |
| 14 | GetGeometry | reply: surface dimensions |
| 15 | QueryTree | reply: parent + children list |
| 16 | InternAtom | handle: maintain atom table |
| 17 | GetAtomName | handle: reverse lookup |
| 18 | ChangeProperty | handle: store on surface (WM_PROTOCOLS, _NET_WM_*) |
| 19 | DeleteProperty | handle |
| 20 | GetProperty | reply: stored value (multi-reply for large data) |
| 21 | ListProperties | reply: atom list for window |
| 22 | SetSelectionOwner | handle: clipboard / DnD ownership |
| 23 | GetSelectionOwner | reply |
| 24 | ConvertSelection | handle: emit SelectionRequest to owner |
| 25 | SendEvent | handle: deliver to target window's event mask |
| 26 | GrabPointer | handle: route pointer events exclusively |
| 27 | UngrabPointer | handle |
| 28 | GrabButton | stub: passive grab table |
| 29 | UngrabButton | stub |
| 30 | ChangeActivePointerGrab | stub: update cursor / event-mask |
| 31 | GrabKeyboard | handle: route keyboard events exclusively |
| 32 | UngrabKeyboard | handle |
| 33 | GrabKey | stub: passive keyboard grab table |
| 34 | UngrabKey | stub |
| 35 | AllowEvents | stub: release queued events |
| 36 | GrabServer | stub: no-op (single-threaded bridge) |
| 37 | UngrabServer | stub |
| 38 | QueryPointer | reply: synthesise from seat state |
| 39 | GetMotionEvents | reply: empty list (bridge does not queue history) |
| 40 | TranslateCoordinates | reply: simple offset |
| 41 | WarpPointer | handle: seat warp (if supported) |
| 42 | SetInputFocus | handle: seat.set_keyboard_focus |
| 43 | GetInputFocus | reply: current focus |
| 44 | QueryKeymap | reply: 32 bytes of pressed keys |
| 45 | OpenFont | stub: return font XID (legacy) |
| 46 | CloseFont | stub |
| 47 | QueryFont | reply: stub font info |
| 48 | QueryTextExtents | reply: stub extents |
| 49 | ListFonts | reply: empty list |
| 50 | ListFontsWithInfo | reply: single terminating reply |
| 51 | SetFontPath | stub |
| 52 | GetFontPath | reply: empty |
| 53 | CreatePixmap | handle: import dmabuf or allocate CPU pixmap |
| 54 | FreePixmap | handle: release |
| 55 | CreateGC | handle: allocate GC XID, store state |
| 56 | ChangeGC | handle: update GC state |
| 57 | CopyGC | handle |
| 58 | SetDashes | stub |
| 59 | SetClipRectangles | stub or handle |
| 60 | FreeGC | handle |
| 61 | ClearArea | handle: damage region |
| 62 | CopyArea | handle: blit |
| 63 | CopyPlane | handle: blit with plane mask |
| 64 | PolyPoint | handle: draw into pixmap |
| 65 | PolyLine | handle |
| 66 | PolySegment | handle |
| 67 | PolyRectangle | handle |
| 68 | PolyArc | handle |
| 69 | FillPoly | handle |
| 70 | PolyFillRectangle | handle |
| 71 | PolyFillArc | handle |
| 72 | PutImage | handle: CPU upload to pixmap |
| 73 | GetImage | reply: CPU readback from pixmap |
| 74 | PolyText8 | handle: server-side text rendering |
| 75 | PolyText16 | handle |
| 76 | ImageText8 | handle |
| 77 | ImageText16 | handle |
| 78 | CreateColormap | stub: colormap XID |
| 79 | FreeColormap | stub |
| 80 | CopyColormapAndFree | stub |
| 81 | InstallColormap | stub |
| 82 | UninstallColormap | stub |
| 83 | ListInstalledColormaps | reply: empty |
| 84 | AllocColor | reply: synthesize RGB |
| 85 | AllocNamedColor | reply |
| 86 | AllocColorCells | reply |
| 87 | AllocColorPlanes | reply |
| 88 | FreeColors | stub |
| 89 | StoreColors | stub |
| 90 | StoreNamedColor | stub |
| 91 | QueryColors | reply: identity |
| 92 | LookupColor | reply |
| 93 | CreateCursor | handle: cursor surface |
| 94 | CreateGlyphCursor | handle: cursor from font |
| 95 | FreeCursor | handle |
| 96 | RecolorCursor | handle |
| 97 | QueryBestSize | reply: size as-is |
| 98 | QueryExtension | handle: dispatch to extension table |
| 99 | ListExtensions | reply: supported extension list |
| 100 | ChangeKeyboardMapping | handle: update keymap |
| 101 | GetKeyboardMapping | reply: from libxkbcommon state |
| 102 | ChangeKeyboardControl | stub: led/auto-repeat ignored |
| 103 | GetKeyboardControl | reply: defaults |
| 104 | Bell | stub |
| 105 | ChangePointerControl | stub: acceleration/threshold ignored |
| 106 | GetPointerControl | reply: defaults |
| 107 | SetScreenSaver | stub |
| 108 | GetScreenSaver | reply: defaults |
| 109 | ChangeHosts | stub: deny or ignore |
| 110 | ListHosts | reply: local only |
| 111 | SetAccessControl | stub |
| 112 | SetCloseDownMode | stub: retain/destroy resource policy |
| 113 | KillClient | stub: destroy resources for XID |
| 114 | RotateProperties | handle: rearrange property atoms |
| 115 | ForceScreenSaver | stub |
| 116 | SetPointerMapping | stub |
| 117 | GetPointerMapping | reply: identity 1..N |
| 118 | SetModifierMapping | handle: update XKB modifier map |
| 119 | GetModifierMapping | reply: from XKB state |
| 120 | NoOperation | stub: discard bytes |

The bridge must at minimum handle the opcodes that real X11 clients
block on: `CreateWindow` (1), `ChangeWindowAttributes` (2),
`GetWindowAttributes` (3), `DestroyWindow` (4), `MapWindow` (8),
`UnmapWindow` (10), `ConfigureWindow` (12), `GetGeometry` (14),
`QueryTree` (15), `InternAtom` (16), `GetAtomName` (17),
`ChangeProperty` (18), `GetProperty` (20), `ListProperties` (21),
`SetSelectionOwner` (22), `GetSelectionOwner` (23), `ConvertSelection`
(24), `SendEvent` (25), `QueryPointer` (38), `TranslateCoordinates`
(40), `SetInputFocus` (42), `GetInputFocus` (43), `QueryKeymap` (44),
`CreatePixmap` (53), `FreePixmap` (54), `CreateGC` (55), `ChangeGC`
(56), `FreeGC` (60), `CopyArea` (62), `PutImage` (72), `GetImage` (73),
`CreateCursor` (93), `FreeCursor` (95), `QueryExtension` (98),
`ListExtensions` (99), `GetKeyboardMapping` (101), `GetModifierMapping`
(119). A client that issues any reply-bearing request and gets no reply
will block in `XReply()` forever, so every reply-bearing opcode must at
least emit a stub reply.

`CreateWindow` is the canonical example of a multi-field request. The
wire layout [1]:

```
0  1 opcode
1  CARD8 depth
2  8+n request length
4  WINDOW wid
4  WINDOW parent
2  INT16 x
2  INT16 y
2  CARD16 width
2  CARD16 height
2  CARD16 border-width
2  class   0 CopyFromParent, 1 InputOutput, 2 InputOnly
4  VISUALID visual   0 CopyFromParent
4  BITMASK value-mask   (n bits set to 1)
4n LISTofVALUE value-list   (one 4-byte slot per set bit, low bits first)
```

The value-mask bits, in order [1]:

```
0x00000001 background-pixmap        PIXMAP (0 None, 1 ParentRelative)
0x00000002 background-pixel         CARD32
0x00000004 border-pixmap            PIXMAP (0 CopyFromParent)
0x00000008 border-pixel             CARD32
0x00000010 bit-gravity              BITGRAVITY
0x00000020 win-gravity              WINGRAVITY
0x00000040 backing-store            {0 NotUseful, 1 WhenMapped, 2 Always}
0x00000080 backing-planes           CARD32
0x00000100 backing-pixel            CARD32
0x00000200 override-redirect        BOOL
0x00000400 save-under               BOOL
0x00000800 event-mask               SETofEVENT
0x00001000 do-not-propagate-mask    SETofDEVICEEVENT
0x00002000 colormap                 COLORMAP (0 CopyFromParent)
0x00004000 cursor                   CURSOR (0 None)
```

For InputOnly windows, only `win-gravity`, `event-mask`,
`do-not-propagate-mask`, `override-redirect` and `cursor` are legal;
anything else is a `Match` error [1]. The bridge only needs to track
`event-mask` (which events the client wants on this window),
`override-redirect` (whether to bypass WM behaviour), `cursor`, and the
geometry. Background, border, backing-store and colormap are all
relevant to a real X server that maintains window pixel contents, which
the bridge does not; they can be stored and ignored.

## 3. Events

Events are 32 bytes [1]. Byte 0 is the event type. Bit 0x80 of byte 0
is set when the event was generated by a `SendEvent` request rather than
by the server itself, so the effective type is `byte0 & 0x7F` [1]. The
core event types occupy 1..35; event codes 64..127 are reserved for
extension events delivered via the legacy (pre-XGE) mechanism [1].

Every core event except `KeymapNotify` carries the low 16 bits of the
sequence number of the last request the server processed for the
client, in bytes 2..3 [1]. The bridge must fill this in.

The full core event table, derived from the protocol encoding [1]:

| Type | Name | Generated by | Bridge role |
|---:|---|---|---|
| 1 | (reserved) | - | - |
| 2 | KeyPress | server, from input | generate from ISZ keyboard |
| 3 | KeyRelease | server, from input | generate |
| 4 | ButtonPress | server, from input | generate from ISZ pointer |
| 5 | ButtonRelease | server, from input | generate |
| 6 | MotionNotify | server, from input | generate |
| 7 | EnterNotify | server, from pointer | generate (or stub) |
| 8 | LeaveNotify | server, from pointer | generate (or stub) |
| 9 | FocusIn | server, from SetInputFocus | generate on focus change |
| 10 | FocusOut | server | generate |
| 11 | KeymapNotify | server, after FocusIn/EnterNotify | stub (32 bytes of zero) |
| 12 | Expose | server, when window region needs repaint | generate on damage |
| 13 | GraphicsExposure | server, from CopyArea with no-exposure off | stub |
| 14 | NoExposure | server, from CopyArea | stub |
| 15 | VisibilityNotify | server | stub |
| 16 | CreateNotify | server, on CreateWindow | generate |
| 17 | DestroyNotify | server, on DestroyWindow | generate |
| 18 | UnmapNotify | server, on UnmapWindow | generate |
| 19 | MapNotify | server, on MapWindow | generate |
| 20 | MapRequest | server to WM, on MapWindow with override-redirect false | generate to itself (bridge is WM) |
| 21 | ReparentNotify | server, on ReparentWindow | generate |
| 22 | ConfigureNotify | server, on ConfigureWindow | generate |
| 23 | ConfigureRequest | server to WM, on ConfigureWindow | generate to itself |
| 24 | GravityNotify | server, after resize | stub |
| 25 | ResizeRequest | server to WM, when client resizes itself | generate |
| 26 | CirculateNotify | server | stub |
| 27 | CirculateRequest | server to WM | generate |
| 28 | PropertyNotify | server, on ChangeProperty/DeleteProperty | generate |
| 29 | SelectionClear | server, when selection owner changes | generate |
| 30 | SelectionRequest | server to owner, on ConvertSelection | generate to owner |
| 31 | SelectionNotify | server to requestor, after transfer | generate |
| 32 | ColormapNotify | server | stub |
| 33 | ClientMessage | client, via SendEvent; or server | deliver |
| 34 | MappingNotify | server, after modifier/keyboard mapping change | generate |
| 35 | GenericEvent (XGE) | extension | see section 11 |

Wire layout of the input events (KeyPress through MotionNotify), which
are the only events the scaffold currently emits [1]:

```
0  CARD8 code      (event type)
1  detail          KEYCODE for KeyPress/KeyRelease
                   BUTTON for ButtonPress/ButtonRelease
                   0 Normal, 1 Hint for MotionNotify
2  CARD16 sequence number
4  TIMESTAMP time
4  WINDOW  root
4  WINDOW  event    (window that has the event mask)
4  WINDOW  child    0 None
2  INT16  root-x
2  INT16  root-y
2  INT16  event-x
2  INT16  event-y
2  SETofKEYBUTMASK state
1  BOOL   same-screen
1  unused
```

`SETofKEYBUTMASK` is a 16-bit field: bits 0..7 are button states
(button 1 = bit 0, ... button 5 = bit 4), bits 8..12 are Shift, Lock,
Control, Mod1..Mod5 [1]. The bridge fills this from its XKB modifier
state plus the currently-pressed buttons.

`ConfigureNotify` (event type 22) is the load-bearing WM event [1]:

```
0  22 code
1  unused
2  CARD16 sequence number
4  WINDOW event   (the window receiving the event, often the client top-level)
4  WINDOW window  (the configured window)
4  WINDOW above-sibling   0 None if the window is at the bottom
2  INT16  x
2  INT16  y
2  CARD16 width
2  CARD16 height
2  CARD16 border-width
1  BOOL   override-redirect
5  unused
```

`ClientMessage` (event type 33) is the vehicle for EWMH client messages
and the ICCCM `WM_DELETE_WINDOW` protocol [1][2][3]:

```
0  33 code
1  CARD8 format   {8, 16, 32}   (bits per element)
2  CARD16 sequence number
4  WINDOW window
4  ATOM   type
20 data           (20 bytes; 20 CARD8 if format=8, 10 CARD16 if 16, 5 CARD32 if 32)
```

The bridge generates events: it converts Ishizue input events
(`ISZ_EVENT_INPUT_KEYBOARD_KEY`, `ISZ_EVENT_INPUT_POINTER_MOTION`,
`ISZ_EVENT_INPUT_POINTER_BUTTON`) into the matching X11 input events,
fills in the destination window based on the seat's current focus, and
honors each client's `event-mask`. The bridge consumes events it sent to
itself (`MapRequest`, `ConfigureRequest`, `CirculateRequest`,
`ResizeRequest`) to drive WM decisions; in a normal X server these go to
the WM client, but the bridge is the WM.

## 4. Errors

Errors are 32 bytes [1]:

```
0  0      Error indicator (always 0; distinguishes from reply byte 0 = 1)
1  CARD8  code         (the error code)
2  CARD16 sequence number   (of the failing request)
4  4 bytes additional data  (resource id, atom, value, or unused)
8  CARD16 minor opcode
10 CARD8  major opcode
11..31 unused
```

The 17 standard error codes [1]:

| Code | Name | Additional data (bytes 4..7) |
|---:|---|---|
| 1 | Request | unused |
| 2 | Value | the bad value (CARD32) |
| 3 | Window | the bad WINDOW id |
| 4 | Pixmap | the bad PIXMAP id |
| 5 | Atom | the bad ATOM id |
| 6 | Cursor | the bad CURSOR id |
| 7 | Font | the bad FONT id |
| 8 | Match | unused |
| 9 | Drawable | the bad DRAWABLE id |
| 10 | Access | unused |
| 11 | Alloc | unused |
| 12 | Colormap | the bad COLORMAP id |
| 13 | GContext | the bad GCONTEXT id |
| 14 | IDChoice | the bad resource id |
| 15 | Name | unused |
| 16 | Length | unused |
| 17 | Implementation | unused |

Codes 128..255 are reserved for extension errors [1]. The bridge will
need to allocate codes for XKB, XInput2, RANDR, DRI3 and Present if it
claims those extensions in `QueryExtension`.

Errors flow back to the client ahead of any reply or event caused by
subsequent requests on the same connection [1]. The protocol guarantees
that errors caused by a given request arrive before the reply of any
later request on the same connection (Chapter 12, Flow Control and
Concurrency). The bridge must respect this: it cannot reorder errors to
the end of a queue. The scaffold never sends errors today, which means
misbehaving clients hang waiting for a reply that the server thought it
had skipped; the bridge must emit at least an `Implementation` error for
every opcode it does not handle.

## 5. Replies

Replies are 32 bytes plus optional additional data [1]:

```
0  1      Reply indicator (always 1)
1  CARD8  (request-specific, often a bool or count)
2  CARD16 sequence number
4  CARD32 length       in 4-byte units, of additional data after first 32 bytes
8..31   24 bytes of fixed fields (request-specific)
32..    additional data (length * 4 bytes)
```

A request either produces one reply, multiple replies (a "list" reply
stream), or no reply. Requests with no reply either succeed silently or
produce an error. The bridge must know which is which for every opcode.

Single-reply requests: `GetWindowAttributes`, `GetGeometry`,
`QueryTree`, `InternAtom`, `GetAtomName`, `GetProperty`,
`ListProperties`, `GetSelectionOwner`, `QueryPointer`,
`GetMotionEvents`, `TranslateCoordinates`, `GetInputFocus`,
`QueryKeymap`, `QueryFont`, `QueryTextExtents`, `ListFonts`,
`SetFontPath`, `GetFontPath`, `ListInstalledColormaps`, `AllocColor`,
`AllocNamedColor`, `AllocColorCells`, `AllocColorPlanes`, `QueryColors`,
`LookupColor`, `QueryBestSize`, `QueryExtension`, `ListExtensions`,
`GetKeyboardMapping`, `GetKeyboardControl`, `GetPointerControl`,
`GetScreenSaver`, `ListHosts`, `GetPointerMapping`,
`GetModifierMapping` [1].

Multi-reply requests: `ListFontsWithInfo` produces a series of replies
terminated by one with `name-length = 0`; `GetProperty` returns a single
reply but can be called repeatedly with `long-offset` / `long-length`
to page through values larger than the maximum reply size [1]. The
scaffold returns no replies today, which is the main reason no real
client gets past `InternAtom` and `QueryExtension`.

A reply is distinguished from an error by byte 0: `1` for reply, `0`
for error. Both carry the low 16 bits of the request sequence number in
bytes 2..3, so the client matches a reply or error to its request by
sequence number.

`InternAtom` reply, the one the bridge will send most often [1]:

```
0  1 Reply
1  unused
2  CARD16 sequence number
4  0 reply length   (always 0)
4  ATOM atom        0 None if only-if-exists was true and the name is unknown
20 unused
```

`QueryExtension` reply [1]:

```
0  1 Reply
1  unused
2  CARD16 sequence number
4  0 reply length
1  BOOL   present
1  CARD8  major-opcode     (0 if not present or has no opcode)
1  CARD8  first-event      (0 if no events)
1  CARD8  first-error      (0 if no errors)
20 unused
```

`GetProperty` reply [1]:

```
0  1 Reply
1  CARD8 format    {0, 8, 16, 32}; 0 means the property does not exist
2  CARD16 sequence number
4  (n+p)/4 reply length
4  ATOM type       0 None if property does not exist
4  CARD32 bytes-after   bytes remaining for subsequent GetProperty calls
4  CARD32 length-of-value  in format units
12 unused
n  LISTofBYTE value, padded to 4
```

## 6. Window management model

The X11 window tree has one root per screen. Each top-level window is a
direct child of root unless the WM reparents it into a frame window
[1]. The window manager is the client that has selected
`SubstructureRedirectMask` on the root window; when any other client
calls `MapWindow`, `ConfigureWindow`, or `CirculateWindow` on a child
of root, the server intercepts the request and delivers a `MapRequest`,
`ConfigureRequest`, or `CirculateRequest` event to the WM instead of
executing it [1]. The WM then performs its own `MapWindow` /
`ConfigureWindow` on the window (which goes through, because the WM
owns the redirect mask) and may reparent the window into a frame.

This is the part the bridge has to internalise: the bridge is the WM.
There is no separate WM client. When a client calls `MapWindow`, the
bridge must generate a `MapRequest` to itself, decide what to do (in
Ishizue terms: create a surface, assign it to an output, set its
position based on whatever tiling the Architect has chosen), then issue
the actual `MapWindow` action by sending `ISZ_MSG_SURFACE_SET_OUTPUT`
and `ISZ_MSG_COMMIT` to Ishizue. The same applies to `ConfigureRequest`
(translate to `isz_surface_set_position` / `isz_surface_set_size` /
`isz_surface_set_zpos`) and `CirculateRequest`.

`override-redirect` windows (set via `ChangeWindowAttributes` or in
`CreateWindow`'s value-list) bypass this interception [1]. Popups,
menus, tooltips, and splash screens use this flag to avoid WM
involvement. The bridge must honour it: pass them straight through to
the surface without `MapRequest`.

Reparenting. The bridge can either reparent top-level windows into a
frame window (the classic Xorg WM approach, used to draw decorations)
or skip reparenting entirely and let the Ishizue surface be the
top-level window itself. Skipping reparenting is simpler and matches
how Xwayland works under Wayland compositors: each top-level X window
becomes a Wayland `xdg_toplevel`, no frame window is created. The
bridge should do the same.

Window-manager properties. The ICCCM [2] defines the properties clients
set on their top-level windows; the WM reads and updates them. The
minimum set:

- `WM_NAME` (type STRING): the window title in Latin-1.
- `WM_ICON_NAME` (type STRING): icon title.
- `WM_CLASS` (type STRING): two null-terminated strings, the instance
  and class names, used for resource matching.
- `WM_PROTOCOLS` (type ATOM): list of atoms the client understands,
  typically `WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`, `_NET_WM_PING`.
- `WM_DELETE_WINDOW` (atom, listed in `WM_PROTOCOLS`): client agrees to
  be asked to close via a `ClientMessage` rather than being killed.
- `WM_TAKE_FOCUS` (atom, listed in `WM_PROTOCOLS`): client agrees to
  accept focus via a `ClientMessage`.
- `WM_NORMAL_HINTS` (type WM_SIZE_HINTS): min/max size, base size,
  resize increments, aspect ratio, window gravity.
- `WM_HINTS` (type WM_HINTS): icon pixmap, icon window, initial state
  (NormalState / IconicState), input flag, window group leader.
- `WM_STATE` (type WM_STATE, set by the WM): two CARD32s, state
  (0 Withdrawn, 1 Normal, 3 Iconic) and icon window [2].
- `WM_TRANSIENT_FOR` (type WINDOW): the parent window of a transient
  dialog.

EWMH [3] extends ICCCM with `_NET_*` atoms:

- `_NET_SUPPORTED` (ATOM[], on root): the list of EWMH atoms the WM
  supports. The bridge must set this on root at startup.
- `_NET_WM_NAME` (UTF8_STRING): UTF-8 title, preferred over `WM_NAME`.
- `_NET_WM_PID` (CARDINAL): PID of the client process. Paired with
  `WM_CLIENT_MACHINE`.
- `_NET_WM_WINDOW_TYPE` (ATOM[]): one or more of `_NET_WM_WINDOW_TYPE_*
  (DESKTOP, DOCK, TOOLBAR, MENU, UTILITY, SPLASH, DIALOG, DROPDOWN_MENU,
  POPUP_MENU, TOOLTIP, NOTIFICATION, COMBO, DND, NORMAL)`.
- `_NET_WM_STATE` (ATOM[]): one or more of `_NET_WM_STATE_*` (MODAL,
  STICKY, MAXIMIZED_VERT, MAXIMIZED_HORZ, SHADED, SKIP_TASKBAR,
  SKIP_PAGER, HIDDEN, FULLSCREEN, ABOVE, BELOW, DEMANDS_ATTENTION,
  FOCUSED).
- `_NET_WM_DESKTOP` (CARDINAL): the virtual desktop the window is on.
- `_NET_ACTIVE_WINDOW` (WINDOW, on root): the currently focused
  top-level window.

The bridge can ignore virtual desktops (the Architect does tiling, not
workspaces) but should still answer `_NET_SUPPORTED` so EWMH-aware
clients (pagers, taskbars) do not assume the WM is broken. `_NET_WM_PID`
is useful for the bridge to map X11 windows back to client processes,
which the Ishizue allowlist (SPEC §6.3) already tracks.

Map state. A window is in one of three states: `Unmapped`,
`Unviewable` (mapped but an ancestor is unmapped), `Viewable` (mapped
and all ancestors mapped) [1]. `GetWindowAttributes` returns this in
the `map-state` field. The bridge tracks only `Unmapped` and `Viewable`
because it does not have a real window hierarchy.

## 7. The X resource model

Every X resource is identified by a 32-bit XID. XIDs are
client-allocated, not server-allocated [1]. The server hands each
client a `resource-id-base` and `resource-id-mask` in `SetupSuccess`;
the client allocates XIDs by picking values of the form
`(base | (counter & mask))` where `counter` increments. The bridge
already exposes `resource_id_base` and `resource_id_mask` in its
`x11_setup_success` struct but currently hardcodes them to 1.

The bridge, playing server, hands each connecting client a
`resource-id-base` and `resource-id-mask`. A typical scheme: base
`0x40000000`, mask `0x003fffff`, giving the client just over a million
XIDs before it has to ask for more (which is done via the XC-MISC
extension; the bridge can ignore this and just disconnect clients that
exhaust their range).

Resource types and their XIDs [1]:

- WINDOW: created by `CreateWindow` (opcode 1).
- PIXMAP: created by `CreatePixmap` (53) or `DRI3PixmapFromBuffer`.
- GC (GCONTEXT): created by `CreateGC` (55).
- CURSOR: created by `CreateCursor` (93) or `CreateGlyphCursor` (94).
- FONT: created by `OpenFont` (45).
- COLORMAP: created by `CreateColormap` (78).
- ATOM: created by `InternAtom` (16). Atoms are global across the
  server, not per-client, and never freed.
- DRAWABLE: union of WINDOW and PIXMAP; both can be the destination of
  graphics requests.
- FONTABLE: union of FONT and GCONTEXT; both can be passed to
  `QueryFont` (47) and `QueryTextExtents` (48).
- FENCE: created by the Sync extension's `SyncCreateFence` and by
  `DRI3FenceFromFD`.
- SYNCOBJ: created by `DRI3ImportSyncobj` (DRI3 v1.2).
- REGION: created by the XFixes extension.

The bridge must keep a per-connection table mapping XID to a bridge
struct for that resource. On `DestroyWindow`, `FreePixmap`, `FreeGC`,
`FreeCursor`, `CloseFont`, `FreeColormap`, `FreeColors`, the resource
is destroyed. On `KillClient` and on connection close, all of the
client's resources are destroyed [1]. The close-down mode
(`SetCloseDownMode`, opcode 112) decides whether resources are
destroyed on disconnect (`Destroy`, the default), kept until explicitly
freed (`RetainPermanent`), or kept until `KillClient AllTemporary`
(`RetainTemporary`) [1]; the bridge can implement only `Destroy` and
treat the others as `Destroy`.

Lifetime invariant: a resource lives from its creation request until
either an explicit Free request or the owning client's connection
closes (in `Destroy` mode). The bridge must never let a client refer to
another client's resource by guessing XIDs; the XID range allocation
makes that impossible by construction.

Atoms are special: they are global, never freed, and predefined atoms
1..68 exist without `InternAtom` [1]. See section 12.

## 8. Rendering paths

The bridge must support three rendering paths because real X11 clients
use all three.

### 8.1 Core X11 rendering

Core rendering is CPU-based and pixmap-backed. The relevant opcodes
[1]:

- `CreatePixmap` (53): allocate a pixmap of given width, height, depth.
- `FreePixmap` (54): free.
- `CreateGC` (55), `ChangeGC` (56), `CopyGC` (57), `SetDashes` (58),
  `SetClipRectangles` (59), `FreeGC` (60): manage graphics state.
- `ClearArea` (61): clear a region of a window to its background.
- `CopyArea` (62): blit a rectangle from one drawable to another.
- `CopyPlane` (63): blit a single plane.
- `PolyPoint` (64), `PolyLine` (65), `PolySegment` (66),
  `PolyRectangle` (67), `PolyArc` (68), `FillPoly` (69),
  `PolyFillRectangle` (70), `PolyFillArc` (71): vector drawing.
- `PutImage` (72): upload client-format pixel data to a drawable.
- `GetImage` (73): read pixel data back from a drawable.
- `PolyText8` (74), `PolyText16` (75), `ImageText8` (76),
  `ImageText16` (77): server-side text rendering using a font.

The bridge has two strategies for these: implement them in software
against a CPU-side pixmap for each window (the Xorg dix/mi approach),
or implement only `CopyArea`, `PutImage`, `GetImage` and reject the
vector primitives with an `Implementation` error. The latter is what
Xwayland effectively does today: it routes everything through glamor
(GPU acceleration) for the vector ops, and falls back to pixman for
pure-CPU paths. Ishizue, which has no GPU compositing in the library
itself (SPEC §2 excludes "GPU compositing / plane sharing" from the
library scope), would need to do CPU rendering in the bridge process
and upload via `isz_surface_attach_buffer` with a SHM dma-buf (SPEC §11
permits SHM as a fallback). The minimum viable bridge handles
`PutImage` (clients write pixels) and `CopyArea` (clients composite
between their own pixmaps), and rejects the vector primitives.

### 8.2 MIT-SHM

The MIT-SHM extension [4] lets a client and server share a SysV shared
memory segment (`shmget`/`shmat`) so image data does not have to be
copied through the socket. The protocol adds four requests:
`ShmQueryVersion`, `ShmAttach`, `ShmDetach`, `ShmPutImage`,
`ShmGetImage`, `ShmCreatePixmap`, and the `ShmCompletion` event [4].

`ShmPutImage` is the most-used: the client writes pixels into the
shared segment and tells the server to copy from segment to drawable.
The server sends a `ShmCompletion` event when it is done so the client
can mutate the segment again.

Xwayland handles MIT-SHM by translating `ShmPutImage` into a glamor
`put_image` that uploads to a GL texture, and `ShmCreatePixmap` into a
backing-store pixmap that lives in shared memory and is uploaded on
demand. The bridge can take the simpler path: keep MIT-SHM segments in
the bridge process, copy their bytes into a CPU pixmap, and on commit
upload that pixmap to Ishizue as a SHM dma-buf. SHM pixmaps in
particular translate badly to scanout (they are not dmabufs and have
arbitrary stride/format), so the bridge should always copy through.

### 8.3 DRI3 + Present

The modern path. Clients allocate a dmabuf on the GPU (via Mesa/GBM or
Vulkan WSI), import it into the X server as a pixmap via
`DRI3PixmapFromBuffer`, and then call `PresentPixmap` to display that
pixmap on a window at a specific vblank, synchronised by Sync fences
[6][7]. This is the path used by GLX direct rendering, Vulkan WSI,
every modern browser, every game.

The bridge must support DRI3 + Present for any modern X11 app to be
usable. Sections 9 and 10 cover the wire format.

## 9. DRI3 in depth

DRI3 [7] is built on POSIX file descriptor passing over the X socket.
On Linux, the file descriptors are dmabuf fds. The extension has six
requests in v1.0 [7] and four more in v1.2 [8].

Extension name: "DRI3". The bridge returns its major opcode from
`QueryExtension`. The minor opcode in byte 1 of each request
distinguishes the requests.

| Minor | Name | Direction | Notes |
|---:|---|---|---|
| 0 | DRI3QueryVersion | req + reply | version negotiation |
| 1 | DRI3Open | req + reply | returns render-node fd for a drawable |
| 2 | DRI3PixmapFromBuffer | req (with fd) | wrap client dmabuf as server pixmap |
| 3 | DRI3BufferFromPixmap | req + reply (with fd) | export server pixmap as dmabuf |
| 4 | DRI3FenceFromFD | req (with fd) | wrap client sync_file fd as Sync Fence |
| 5 | DRI3FDFromFence | req + reply (with fd) | export Sync Fence as sync_file fd |
| 6 | DRI3GetSupportedModifiers | req + reply | (v1.2) format modifiers the server accepts |
| 7 | DRI3PixmapFromBuffers | req (with N fds) | (v1.2) multi-plane dmabuf, with modifier |
| 8 | DRI3BuffersFromPixmap | req + reply (with N fds) | (v1.2) export as multi-plane dmabuf |
| 9 | DRI3ImportSyncobj | req (with fd) | (v1.2) wrap client drm_syncobj fd as server Syncobj |
| 10 | DRI3FreeSyncobj | req | (v1.2) release server Syncobj |

`DRI3QueryVersion` [7]:

```
0  CARD8 major opcode
1  0 DRI3 opcode
2  3 length
4  CARD32 major version    (client supports)
4  CARD32 minor version    (client supports)
```

Reply:

```
0  1 Reply
1  unused
2  CARD16 sequence number
4  0 reply length
4  CARD32 major version    (server supports)
4  CARD32 minor version
16 unused
```

`DRI3PixmapFromBuffer` (v1.0, single plane, no modifier) [7]:

```
0  CARD8 major opcode
1  2 DRI3 opcode
2  6 length
4  PIXMAP pixmap          (client-allocated XID for the new pixmap)
4  DRAWABLE drawable      (screen reference)
4  CARD32 size            (bytes; at least height*stride)
2  CARD16 width
2  CARD16 height
2  CARD16 stride
1  CARD8  depth
1  CARD8  bpp
   1 fd (the dmabuf), passed as ancillary data via SCM_RIGHTS
```

`DRI3PixmapFromBuffers` (v1.2, multi-plane, with modifier) [8]:

```
0  CARD8 major opcode
1  7 DRI3 opcode
2  length
4  PIXMAP pixmap
4  WINDOW window
1  CARD8  num_buffers     (1..4; one fd per plane)
2  CARD16 width
2  CARD16 height
4  CARD32 stride0, offset0
4  CARD32 stride1, offset1
4  CARD32 stride2, offset2
4  CARD32 stride3, offset3
1  CARD8  depth
1  CARD8  bpp
8  CARD64 modifier        (DRM_FORMAT_MOD_*; DRM_FORMAT_MOD_INVALID for implicit)
   num_buffers fds passed as ancillary data
```

`DRI3BufferFromPixmap` (v1.0 export) [7]:

```
0  CARD8 major opcode
1  3 DRI3 opcode
2  2 length
4  PIXMAP pixmap
```

Reply:

```
0  1 Reply
1  1 nfd       (always 1)
2  CARD16 sequence number
4  0 reply length
4  CARD32 size
2  CARD16 width
2  CARD16 height
2  CARD16 stride
1  CARD8  depth
1  CARD8  bpp
12 unused
   1 fd (the dmabuf), passed as ancillary data
```

`DRI3ImportSyncobj` (v1.2) [8]:

```
0  CARD8 major opcode
1  9 DRI3 opcode
2  length
4  SYNCOBJ syncobj        (client-allocated XID for the server-side syncobj)
4  DRAWABLE drawable
   1 fd (a drm_syncobj file descriptor), passed as ancillary data
```

`DRI3FreeSyncobj` (v1.2) [8]:

```
0  CARD8 major opcode
1  10 DRI3 opcode
2  2 length
4  SYNCOBJ syncobj
```

The fd-passing mechanism. X11 fds are sent as ancillary data on the
Unix socket, via `SCM_RIGHTS` in a `sendmsg` control message [7]. The
number of fds in a request is implied by the request type (or, for
v1.2, the `num_buffers` field for `DRI3PixmapFromBuffers` and the `nfd`
field for replies). The bridge must use `recvmsg`/`sendmsg` instead of
`read`/`write` for any request or reply that carries fds, and must
consume the fds atomically with the message bytes. This is the same
mechanism Ishizue uses for buffer transport (SPEC §6.8, §8), so the
bridge can reuse the Ishizue `isz_conn` framing primitives for the
fd-passing part.

What the bridge can copy from DRI3. DRI3's wire format is the closest
existing analog to Ishizue's §8 buffer model: dmabuf fds flow from
client to server, the server wraps them as opaque pixmaps, the client
tells the server when to display them via Present, the server tells the
client when the buffer is idle again. Ishizue's `isz_surface_attach_buffer`
takes a dmabuf_fd plus a descriptor (width, height, stride, offset,
format, modifier, alpha_mode) [SPEC §8]. The DRI3 v1.2
`DRI3PixmapFromBuffers` request carries exactly this information except
for alpha_mode, which the bridge can derive from the format (for
example, `DRM_FORMAT_ARGB8888` is premultiplied by Mesa convention).
The mapping is one-to-one. The bridge's task is: on
`DRI3PixmapFromBuffers`, take the fd(s) and descriptor, call
`isz_surface_attach_buffer` on the surface associated with the window's
pixmap, store the returned Ishizue buffer_id keyed by the X pixmap XID.
On Present, attach the pixmap's buffer to the window's surface and call
`isz_commit`. On the Ishizue `release` event, send a `PresentIdleNotify`
back to the client.

## 10. Present extension in depth

Present [6] is the mechanism for vsync-synchronized presentation of a
pixmap to a window. It uses XGE for its events (see section 11).

Extension name: "Present". Minor opcodes:

| Minor | Name | Direction | Notes |
|---:|---|---|---|
| 0 | PresentQueryVersion | req + reply | version |
| 1 | PresentPixmap | req | present a pixmap to a window |
| 2 | PresentNotifyMSC | req | notify at a specific frame |
| 3 | PresentSelectInput | req | subscribe to Present events |
| 4 | PresentQueryCapabilities | req + reply | query CRTC caps |
| 5 | PresentQueryValidated | req + reply | (v1.2) validated modifiers for a window |

`PresentPixmap` is the main request [6]:

```
0  CARD8 major opcode
1  1 Present opcode
2  18+2n length
4  WINDOW  window
4  PIXMAP  pixmap
4  CARD32  serial          (client's choice; returned in CompleteNotify)
4  REGION  valid-area      (0 None for whole-pixmap)
4  REGION  update-area     (0 None for whole-window)
2  INT16   x-off
2  INT16   y-off
4  CRTC    target-crtc     (0 None)
4  SyncFence wait-fence     (0 None)
4  SyncFence idle-fence     (0 None)
4  CARD32  options         (bitmask: 1 Async, 2 Copy, 4 UST)
4  unused
8  CARD64  target-msc      (target frame; 0 = next frame)
8  CARD64  divisor         (0 = single-shot)
8  CARD64  remainder       (target = next msc where msc % divisor == remainder)
8n LISTofPRESENTNOTIFY notifies   (extra windows to be notified)
```

Each PRESENTNOTIFY is 8 bytes [6]:

```
4  WINDOW window
4  CARD32 serial
```

`PresentNotifyMSC` (request a callback at a future frame) [6]:

```
0  CARD8 major opcode
1  2 Present opcode
2  10 length
4  WINDOW window
4  CARD32 serial
4  unused
8  CARD64 target-msc
8  CARD64 divisor
8  CARD64 remainder
```

`PresentSelectInput` [6]:

```
0  CARD8 major opcode
1  3 Present opcode
2  4 length
4  PRESENTEVENTID event-id   (client-allocated XID)
4  WINDOW window
4  SETofPRESENTEVENTMASK event-mask   (1 ConfigureNotify, 2 CompleteNotify, 4 IdleNotify)
```

Present events are XGE events, type 35 [5][6]. Their layout:

```
0  35 GenericEvent
1  CARD8 Present extension opcode (the major opcode from QueryExtension)
2  CARD16 sequence number
4  CARD32 length       (in 4-byte units after first 32 bytes; 0 for 32-byte events)
2  CARD16 evtype       (0 ConfigureNotify, 1 CompleteNotify, 2 IdleNotify)
... event-specific
```

`PresentConfigureNotify` (evtype 0) [6]:

```
... (header above)
2  0 PresentConfigureNotify
2  unused
4  CARD32 event id
4  WINDOW window
2  INT16  x
2  INT16  y
2  CARD16 width
2  CARD16 height
2  INT16  off x
2  INT16  off y
2  CARD16 pixmap width
2  CARD16 pixmap height
4  CARD32 pixmap flags
```

`PresentCompleteNotify` (evtype 1) [6]:

```
... (header above)
2  1 PresentCompleteNotify
1  CARD8  kind       (0 Pixmap, 1 MSCNotify)
1  CARD8  mode       (0 Copy, 1 Flip, 2 Skip)
4  CARD32 event id
4  WINDOW window
4  CARD32 serial     (from PresentPixmap / PresentNotifyMSC)
8  CARD64 ust        (unadjusted system time in microseconds)
8  CARD64 msc        (media stream counter, i.e. frame number)
```

`PresentIdleNotify` (evtype 2) [6]:

```
... (header above)
2  2 PresentIdleNotify
2  unused
4  CARD32 event id
4  WINDOW window
4  CARD32 serial
4  PIXMAP pixmap
4  SyncFence idle-fence
```

Mapping to Ishizue §7.3 (frame scheduling) and the `presented` event
[ SPEC §7.3 ]:

- `PresentPixmap` from the client = "please display this pixmap at the
  next vblank on this window's output." The bridge translates this to
  `isz_surface_attach_buffer(surface, dmabuf_fd, &desc)` followed by
  `isz_commit(output, ISZ_COMMIT_NORMAL)` (or `_ASYNC` if the
  `PresentOptionAsync` bit is set).
- `target-msc`, `divisor`, `remainder` let the client schedule
  presentation at a specific future vblank. The bridge can ignore
  these initially and present on the next vblank; full MSC scheduling
  requires the bridge to track a per-CRTC frame counter incremented on
  each vblank event.
- Ishizue's `presented` event [ SPEC §7.3 ] carries the vblank
  `CLOCK_MONOTONIC` timestamp. The bridge converts this to UST
  (microseconds since some epoch; Xorg uses `CLOCK_MONOTONIC` in
  microseconds) and emits `PresentCompleteNotify` with mode `Flip` if
  the surface was scanned out, or `Copy` if the Architect composited
  it.
- Ishizue's `release` event [ SPEC §8 ] maps directly to
  `PresentIdleNotify`. The client must not reuse the dmabuf until it
  receives `IdleNotify`, exactly as the SPEC requires the client not
  to reuse a buffer until the `release` event arrives.

The idle-fence field lets the client provide a Sync Fence that the
server triggers when the pixmap is idle. This is the implicit-sync
hook; the bridge should prefer the explicit-sync path via
`DRI3ImportSyncobj` (v1.2) and the wait-fence / idle-fence fields of
`PresentPixmap`. Ishizue's §7.5 buffer synchronisation is
`drm_syncobj`-based explicit sync, so the bridge maps Sync Fences
(via `DRI3FenceFromFD` / `DRI3FDFromFence`) and Syncobjs (via
`DRI3ImportSyncobj` / `DRI3FreeSyncobj`) onto Ishizue's
`isz_syncobj_*` primitives directly.

## 11. XGE (X Generic Extension)

Pre-XGE, every extension event type had to fit in 32 bytes and consume
one of the 64 event codes 64..127 [5]. There are not enough codes for
all extensions, and 32 bytes is too small for events that need to carry
fd lists, modifiers, or large structures. XGE fixes both.

XGE is itself an extension, named "Generic Event Extension". A client
must `QueryExtension` it first to make sure the server supports XGE,
and only then may the server send events of type 35 to that client [5].
A server that sends a >32-byte GenericEvent to a client that has not
negotiated XGE corrupts the client's stream because the client will not
read the trailing bytes [5].

The GenericEvent wire layout [5]:

```
0  35 type       (always GenericEvent)
1  CARD8 extension   (major opcode of the originating extension)
2  CARD16 sequence number
4  CARD32 length   (in 4-byte units after the first 32 bytes; 0 if 32-byte event)
8  CARD16 evtype   (extension-specific event type)
10 ... event-specific data, up to 32 + length*4 bytes
```

The high bit of byte 0 (bit 0x80) is still the SendEvent flag, so a
client should mask it before checking for type 35 [1].

Present, DRI3, XInput2, RandR, XKB, and DRI2 all use XGE for their
events. The bridge must support XGE; it cannot meaningfully implement
Present without it. The bridge's XGE implementation: maintain a flag
per connection, set when the client successfully `QueryExtension`s the
"Generic Event Extension" extension; only emit events of type 35 to
clients with the flag set; refuse to send a GenericEvent with length >
0 to a client without the flag, falling back to sending nothing (and
logging) instead.

## 12. Atoms and properties

Atoms are 32-bit integers that name strings. Atoms are global across
the server and never freed [1]. The predefined atoms (1..68) exist
without any client calling `InternAtom` [1]:

| Atom | Name |
|---:|---|
| 1 | PRIMARY |
| 2 | SECONDARY |
| 3 | ARC |
| 4 | ATOM |
| 5 | BITMAP |
| 6 | CARDINAL |
| 7 | COLORMAP |
| 8 | CURSOR |
| 9..16 | CUT_BUFFER0..CUT_BUFFER7 |
| 17 | DRAWABLE |
| 18 | FONT |
| 19 | INTEGER |
| 20 | PIXMAP |
| 21 | POINT |
| 22 | RECTANGLE |
| 23 | RESOURCE_MANAGER |
| 24 | RGB_COLOR_MAP |
| 25 | RGB_BEST_MAP |
| 26 | RGB_BLUE_MAP |
| 27 | RGB_DEFAULT_MAP |
| 28 | RGB_GRAY_MAP |
| 29 | RGB_GREEN_MAP |
| 30 | RGB_RED_MAP |
| 31 | STRING |
| 32 | VISUALID |
| 33 | WINDOW |
| 34 | WM_COMMAND |
| 35 | WM_HINTS |
| 36 | WM_CLIENT_MACHINE |
| 37 | WM_ICON_NAME |
| 38 | WM_ICON_SIZE |
| 39 | WM_NAME |
| 40 | WM_NORMAL_HINTS |
| 41 | WM_SIZE_HINTS |
| 42 | WM_ZOOM_HINTS |
| 43 | MIN_SPACE |
| 44 | NORM_SPACE |
| 45 | MAX_SPACE |
| 46 | END_SPACE |
| 47 | SUPERSCRIPT_X |
| 48 | SUPERSCRIPT_Y |
| 49 | SUBSCRIPT_X |
| 50 | SUBSCRIPT_Y |
| 51 | UNDERLINE_POSITION |
| 52 | UNDERLINE_THICKNESS |
| 53 | STRIKEOUT_ASCENT |
| 54 | STRIKEOUT_DESCENT |
| 55 | ITALIC_ANGLE |
| 56 | X_HEIGHT |
| 57 | QUAD_WIDTH |
| 58 | WEIGHT |
| 59 | POINT_SIZE |
| 60 | RESOLUTION |
| 61 | COPYRIGHT |
| 62 | NOTICE |
| 63 | FONT_NAME |
| 64 | FAMILY_NAME |
| 65 | FULL_NAME |
| 66 | CAP_HEIGHT |
| 67 | WM_CLASS |
| 68 | WM_TRANSIENT_FOR |

Atoms above 68 are allocated by `InternAtom` (opcode 16). The wire
format [1]:

```
0  16 opcode
1  BOOL only-if-exists   (1: return None if name not already defined;
                          0: create a new atom)
2  2+(n+p)/4 request length
2  n length of name
2  unused
n  STRING8 name, padded to 4-byte boundary
```

Reply:

```
0  1 Reply
1  unused
2  CARD16 sequence number
4  0 reply length
4  ATOM atom    (0 None if only-if-exists was true and the name was not found)
20 unused
```

`GetAtomName` (opcode 17) does the reverse. Property values are typed
by an atom that names the type. Common type atoms (not predefined;
must be `InternAtom`'d if used): `STRING` (predefined, 31, Latin-1),
`UTF8_STRING` (not predefined; used by `_NET_WM_NAME`),
`COMPOUND_TEXT` (not predefined), `ATOM` (predefined, 4),
`WINDOW` (33), `PIXMAP` (20), `INTEGER` (19), `CARDINAL` (6),
`WM_STATE` (the type, named after the property; not predefined).

The bridge must maintain an atom table: a bidirectional map between
atom numbers and string names. It can pre-populate atoms 1..68 and
allocate new atoms starting at 69. Atoms are global across all
clients, so the table is per-server, not per-connection.

Properties. A property is a (window, atom) -> (type, format, value)
tuple. `ChangeProperty` (opcode 18) sets one [1]:

```
0  18 opcode
1  mode    0 Replace, 1 Prepend, 2 Append
2  6+(n+p)/4 request length
4  WINDOW  window
4  ATOM    property
4  ATOM    type
1  CARD8   format   {8, 16, 32}   (bits per element)
3  unused
4  CARD32  length-of-data    in format units
n  LISTofBYTE data, padded to 4
```

`GetProperty` (opcode 20) reads one, optionally deleting it [1]. The
bridge stores properties on its per-window state. The minimum
properties it must honour for WM purposes: `WM_NAME`, `WM_CLASS`,
`WM_PROTOCOLS`, `WM_NORMAL_HINTS`, `WM_HINTS`, `WM_STATE` (set by the
bridge itself), `WM_TRANSIENT_FOR`, `WM_CLIENT_MACHINE`,
`_NET_WM_NAME`, `_NET_WM_PID`, `_NET_WM_WINDOW_TYPE`, `_NET_WM_STATE`,
`_NET_WM_DESKTOP`.

## 13. Selections and cut buffers

Selections are the X11 clipboard mechanism. There are two relevant
selection atoms: `PRIMARY` (atom 1, the X11 selection - copy on
select, paste on middle-click) and `CLIPBOARD` (not predefined; the
modern Ctrl-C / Ctrl-V clipboard) [2]. Both are routed through the
same protocol.

The flow [2]:

1. Owner calls `SetSelectionOwner(selection, owner_window, time)`.
   The bridge records (selection -> owner_window, time) and sends a
   `SelectionClear` event to the previous owner if there was one.
2. Requestor calls `ConvertSelection(selection, target, property,
   requestor_window, time)`.
3. The bridge sends a `SelectionRequest` event to the current owner,
   containing (time, owner, requestor, selection, target, property).
4. The owner converts the selection into the requested `target` type,
   stores the result on the `property` of the `requestor_window` via
   `ChangeProperty`, then sends a `SelectionNotify` event to the
   requestor.
5. The requestor reads the property via `GetProperty` and deletes it.

Standard targets [2]:

- `TARGETS`: the owner replies with a list of atoms representing the
  formats it can produce.
- `TIMESTAMP`: the owner replies with the timestamp it acquired
  ownership.
- `MULTIPLE`: the requestor supplies a list of (target, property)
  pairs in a single ConvertSelection; the owner replies to each.
- `SAVE_TARGETS`: used by clipboard managers to ask the owner to
  persist the selection.

Large transfers: `INCR` [2]. If the data is larger than the server's
max request size (or larger than the owner wants to send in one
`ChangeProperty`), the owner replies to the `ConvertSelection` with a
property of type `INCR` whose value is a CARD32 lower bound on the
byte count. The requestor deletes the property, which signals the
owner to send the first chunk; the requestor reads it and deletes it
again, which signals the owner to send the next chunk; this continues
until the owner sends a zero-length property, which terminates the
transfer. The bridge must implement INCR if it wants to support
clipboard transfers of large images or large text.

Mapping to Ishizue §6.8 (clipboard). Ishizue uses fd-passing
(`SCM_RIGHTS`) of `memfd` (for text) or `dmabuf` (for images) tagged
with a MIME type, never copies the data, and lets the Architect filter
via `ISZ_EVENT_CLIPBOARD_REQUEST`. The X11 selection protocol, in
contrast, funnels everything through server properties. The bridge
sits in the middle: when an X11 client acquires `CLIPBOARD` (or
`PRIMARY`), the bridge calls the Ishizue clipboard-ownership API with
the available targets as MIME types; when an Ishizue client requests
the clipboard, the bridge sends `SelectionRequest` events to the X11
owner, reads the resulting property, and pipes the bytes through an
`memfd` or `dmabuf` to Ishizue. For text selections, the bridge
allocates a memfd, writes the property bytes to it, and passes the fd
to Ishizue.

Cut buffers. The ICCCM mentions eight cut buffers (`CUT_BUFFER0` ..
`CUT_BUFFER7`, atoms 9..16) on the root window, a legacy pre-selection
clipboard mechanism [1][2]. Modern clients do not use them. The bridge
can store them as ordinary root-window properties and ignore them
otherwise.

## 14. Grab model

A grab routes input events exclusively to one client [1].

- Active grab: `GrabPointer` (26) or `GrabKeyboard` (31) takes the
  device exclusively for the calling client until `UngrabPointer` (27)
  or `UngrabKeyboard` (32), or until the grab is released via
  `AllowEvents` (35).
- Passive grab: `GrabButton` (28) or `GrabKey` (33) registers a grab
  that activates when the specified button/key + modifier combination
  is pressed. `UngrabButton` (29) and `UngrabKey` (34) cancel them.
- `owner-events` flag: if true, events still go to the window that
  would have received them normally (subject to the grab); if false,
  all events go to the grab-window.
- `pointer-mode` / `keyboard-mode`: `Synchronous` (events queued until
  `AllowEvents`) or `Asynchronous` (events delivered immediately).
- `confine-to`: a window the pointer cannot leave while the grab is
  active.
- `cursor`: a cursor to display during the grab (0 None = no cursor
  change).

`GrabPointer` reply status: 0 Success, 1 AlreadyGrabbed, 2 InvalidTime,
3 NotViewable, 4 Frozen [1].

Conflict with compositor-level grabs. Ishizue's popup grabs (SPEC
§6.7) automatically grab keyboard and pointer until `popup_dismiss`.
X11's passive grabs are explicit per-client and per-button. The
translation:

- `GrabPointer` from an X11 client: the bridge routes pointer events
  to that client's focused window exclusively. The Architect is not
  involved; the bridge just filters events.
- `GrabButton` with modifier: the bridge registers the passive grab
  in its per-client table. When the corresponding ISZ input event
  arrives with the right modifier and button, the bridge activates
  the grab and starts routing to the grab-window.
- `confine-to`: the bridge must clamp pointer motion to the window's
  bounds. Ishizue has no direct confine API in v1, so the bridge must
  implement it by inspecting each `ISZ_EVENT_INPUT_POINTER_MOTION`
  event and adjusting the absolute coordinates before generating the
  X11 `MotionNotify`.

There is no conflict in the strict sense because Ishizue does not do
its own pointer/keyboard grabbing at the library level. The bridge
owns grabbing. The only subtlety is that an Ishizue popup grab (SPEC
§6.7) and an X11 active grab can coexist: the popup grab is at the
seat level (the bridge's grab), the X11 grab is at the client level
(the bridge's per-client routing logic).

## 15. Window classes

`CreateWindow` takes a `class` field [1]:

- `CopyFromParent` (0): inherit from parent. For a child of root, this
  is `InputOutput`.
- `InputOutput` (1): the window has pixel contents (a backing pixmap),
  can be drawn to, can have children, generates graphics events. The
  default for top-level windows.
- `InputOnly` (2): the window has no pixel contents, cannot be drawn
  to, cannot have an InputOutput child, but can receive input events
  and can have InputOnly children. Used for invisible input regions
  and toolit-only overlay windows.

Mapping to Ishizue surface types [SPEC §6.6, §6.7]:

- `InputOutput` top-level window: `isz_surface_create()` followed by
  `isz_surface_set_plane_type(...)` and `isz_surface_set_plane_slot(...)`,
  then `isz_surface_attach_buffer` on commit. This is the normal case.
- `InputOutput` child window: either fold into the parent's surface
  (Xwayland's approach: child windows are not separate Wayland
  surfaces, they are subregions of the parent's surface), or create
  an Ishizue subsurface (SPEC §6.6) with `isz_surface_create_subsurface`.
  Folding is simpler and matches what Xwayland does.
- `InputOnly` window: no surface at all. The bridge tracks it only for
  input event routing: events whose `event` window is an InputOnly
  window must be delivered there, but the window never has a buffer.
- `override-redirect` `InputOutput` top-level window: Ishizue popup
  (SPEC §6.7). Created with `isz_surface_create_popup(parent, x, y)`,
  gets an automatic grab until `popup_dismiss`.

`CopyFromParent` is only valid when the parent is `InputOutput`; for a
child of an `InputOnly` window, `CopyFromParent` is `InputOnly` [1].

## 16. What a real Ishizue X11 bridge needs to implement

Concrete table of opcode handling, categorised by what kind of X11
client depends on it.

### Required for any X11 app (terminal, xterm, xclock, xeyes)

| Opcode | Name | Action |
|---:|---|---|
| 1 | CreateWindow | Allocate surface; store class, geometry, event-mask, override-redirect |
| 2 | ChangeWindowAttributes | Update event-mask, override-redirect, cursor |
| 3 | GetWindowAttributes | Reply with synthesised visual, map-state, event-mask |
| 4 | DestroyWindow | Destroy surface; free XID |
| 8 | MapWindow | Surface set_output + commit; emit MapNotify |
| 10 | UnmapWindow | Surface clear_output; emit UnmapNotify |
| 12 | ConfigureWindow | Surface set_position/set_size/set_zpos; emit ConfigureNotify |
| 14 | GetGeometry | Reply with surface dimensions |
| 15 | QueryTree | Reply with parent + children |
| 16 | InternAtom | Atom table lookup or allocation |
| 17 | GetAtomName | Atom table reverse lookup |
| 18 | ChangeProperty | Store property on window |
| 19 | DeleteProperty | Remove property |
| 20 | GetProperty | Reply with stored value |
| 21 | ListProperties | Reply with atom list |
| 22 | SetSelectionOwner | Update selection table; emit SelectionClear to old owner |
| 23 | GetSelectionOwner | Reply |
| 24 | ConvertSelection | Emit SelectionRequest to owner |
| 25 | SendEvent | Deliver event to destination |
| 38 | QueryPointer | Reply from seat state |
| 40 | TranslateCoordinates | Reply with translated coords |
| 42 | SetInputFocus | seat.set_keyboard_focus |
| 43 | GetInputFocus | Reply |
| 44 | QueryKeymap | Reply with 32-byte pressed-key bitmap |
| 53 | CreatePixmap | Allocate CPU pixmap or import dmabuf |
| 54 | FreePixmap | Free |
| 55 | CreateGC | Allocate GC |
| 56 | ChangeGC | Update GC |
| 60 | FreeGC | Free GC |
| 72 | PutImage | CPU upload to pixmap |
| 73 | GetImage | CPU readback from pixmap |
| 93 | CreateCursor | Allocate cursor surface |
| 95 | FreeCursor | Free |
| 96 | RecolorCursor | Update cursor colors |
| 97 | QueryBestSize | Reply with same size |
| 98 | QueryExtension | Look up extension table; reply |
| 99 | ListExtensions | Reply with supported extensions |
| 101 | GetKeyboardMapping | Reply from XKB state |
| 119 | GetModifierMapping | Reply from XKB state |

### Required for modern apps (browsers, GL apps, games)

| Opcode / Extension | Name | Action |
|---:|---|---|
| ext | Generic Event Extension | Negotiate XGE; gate GenericEvent delivery |
| ext | XKEYBOARD (XKB) | Minimally: reply to `XkbQueryExtension`,
| | | `XkbGetState`, `XkbGetNames`. Optional: `XkbSelectEvents`. |
| ext | XInput2 (XI2) | Reply to `XIQueryVersion`, `XIQueryDevice`; route events. |
| ext | RANDR | Reply to `RRGetScreenResourcesCurrent`, `RRGetScreenInfo`, `RRSelectInput`. The bridge advertises one CRTC matching the Ishizue output. |
| ext | DRI3 | All v1.0 + v1.2 requests; fd passing; `DRI3ImportSyncobj` for explicit sync. |
| ext | Present | `PresentPixmap`, `PresentNotifyMSC`, `PresentSelectInput`, `PresentQueryCapabilities`; emit CompleteNotify / IdleNotify / ConfigureNotify as XGE events. |
| ext | Sync | `SyncCreateFence`, `SyncDestroyFence`, `SyncTriggerFence`, `SyncResetFence` for DRI3 fence support. |
| ext | BIG-REQUESTS | Allow 32-bit request length. |
| ext | XC-MISC | Allocate more XIDs when the client exhausts its range. |
| ext | MIT-SHM | `ShmAttach`, `ShmPutImage`, `ShmGetImage`, `ShmCreatePixmap`; emit `ShmCompletion`. |
| 62 | CopyArea | Blit between pixmaps (used by GLX back buffer composition) |
| 18 | ChangeProperty | Store `WM_PROTOCOLS` including `_NET_WM_PING` |

### Required for legacy apps (core rendering, fonts)

| Opcode | Name | Action |
|---:|---|---|
| 45 | OpenFont | Stub: return font XID, no real font loaded |
| 46 | CloseFont | Stub |
| 47 | QueryFont | Reply with stub font info |
| 48 | QueryTextExtents | Reply with stub extents |
| 49 | ListFonts | Reply with empty list |
| 50 | ListFontsWithInfo | Reply with single terminating reply |
| 51 | SetFontPath | Stub |
| 52 | GetFontPath | Reply with empty list |
| 57 | CopyGC | Copy GC state |
| 58 | SetDashes | Store GC dashes |
| 59 | SetClipRectangles | Store GC clip |
| 61 | ClearArea | Damage region |
| 63 | CopyPlane | Blit with plane mask |
| 64..71 | Poly* primitives | Either implement via pixman or reject with Implementation |
| 74..77 | PolyText/ImageText | Server-side text via a stub font |

### Required for WM duties (the bridge is the WM)

| Opcode | Name | Action |
|---:|---|---|
| 7 | ReparentWindow | Restack; emit ReparentNotify |
| 9 | MapSubwindows | Iterate children |
| 11 | UnmapSubwindows | Iterate children |
| 13 | CirculateWindow | Restack children |
| 26..34 | Grab* family | Per-client grab table; route input |
| 35 | AllowEvents | Release queued events |
| 41 | WarpPointer | seat warp (if supported) |
| 100 | ChangeKeyboardMapping | Update XKB keymap |
| 102 | ChangeKeyboardControl | Stub (led, auto-repeat) |
| 104 | Bell | Stub |
| 105 | ChangePointerControl | Stub |
| 109 | ChangeHosts | Stub or reject |
| 112 | SetCloseDownMode | Stub (only honour Destroy) |
| 113 | KillClient | Destroy resources for XID |
| 114 | RotateProperties | Rearrange property atoms |
| 116 | SetPointerMapping | Stub |
| 118 | SetModifierMapping | Update XKB modifier map |

### Can be stubbed (return Success, no-op)

`GrabServer` (36), `UngrabServer` (37), `SetScreenSaver` (107),
`GetScreenSaver` (108 with default reply), `ForceScreenSaver` (115),
`SetAccessControl` (111), `ChangeHosts` (109), `ListHosts` (110 with
local-only reply), `NoOperation` (120).

### Must reject with Implementation error if seen and not implemented

`OpenFont` (45) without a font backend, `Poly*` (64..71) without a
renderer, `ImageText*` (76..77) without a font. Rejecting these gives
the client a clear signal to fall back. Never silently swallow a
request without either a reply (if the protocol expects one) or an
error; doing so hangs the client.

### Out of scope for v1 of the bridge

Glx (rendering via GLX protocol is deprecated in favour of DRI3 +
Present); DRI2 (superseded by DRI3); XVideo (Xv); XPrint; XTest
(testing injection, useful but not required); XACE / SELinux;
XEvIE (event interception, deprecated); TOG-CUP (colormap usage
policy, deprecated); EVI (extended visual information, deprecated).

## References

[1] X.Org Foundation. "X Window System Protocol." X11R7.7. 
<https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html>

[2] David Rosenthal et al. "Inter-Client Communication Conventions
Manual (ICCCM)." Version 2.0, April 1994. X11R7.7 packaging.
<https://www.x.org/releases/X11R7.7/doc/xorg-docs/icccm/icccm.html>

[3] Freedesktop.org. "Extended Window Manager Hints." Latest
single-page edition.
<https://specifications.freedesktop.org/wm/latest/> (root window
properties: ar01s03; application window properties: ar01s05)

[4] Jonathan Corbet, edited by Keith Packard. "MIT-SHM: The MIT Shared
Memory Extension." X11R7.7 xextproto.
<https://www.x.org/releases/X11R7.7/doc/xextproto/shm.html>

[5] Peter Hutterer. "X Generic Event Extension." X11R7.7 xextproto.
Version 1.0.
<https://www.x.org/releases/X11R7.7/doc/xextproto/geproto.html>

[6] Keith Packard. "The Present Extension." Version 1.0, 2013-6-6.
xorgproto. <https://cgit.freedesktop.org/xorg/proto/presentproto/plain/presentproto.txt>

[7] Keith Packard. "The DRI3 Extension." Version 1.0, 2013-6-4.
xorgproto. <https://cgit.freedesktop.org/xorg/proto/dri3proto/plain/dri3proto.txt>
(also at <https://www.x.org/releases/X11R7.7/doc/>)

[8] Keith Packard, James Jones, Rodrigo Vivi et al. "The DRI3
Extension." Draft version adding `GetSupportedModifiers`,
`PixmapFromBuffers`, `BuffersFromPixmap`, `ImportSyncobj`,
`FreeSyncobj`, `SetDRMDeviceInUse` (DRI3 v1.2 / v1.3). 
<https://raw.githubusercontent.com/rwiggins/xorgproto/explicit-sync/dri3proto.txt>
See also: "DRI3: add DRI3ImportSyncobj and DRI3FreeSyncobj",
<https://cgit.freedesktop.org/xorg/proto/xorgproto/commit/?id=bf661c1c34afb32d8c73b471c17c5bc5912fb346>
and "xcb_dri3_import_syncobj(3)", Arch manual pages,
<https://man.archlinux.org/man/xcb_dri3_import_syncobj.3.en>

[9] X.Org Foundation. "Xsecurity: X display authorization." xorg-docs.
<https://www.x.org/releases/X11R7.7/doc/xorg-docs/security/index.html>

[10] X.Org Foundation. "X Keyboard Extension (XKB) protocol
specification." X11R7.7 kbproto.
<https://www.x.org/releases/X11R7.7/doc/kbproto/xkbproto.html>

[11] Owen Taylor et al. "RandR Extension." X11R7.7 randrproto.
<https://www.x.org/releases/X11R7.7/doc/randrproto/randrproto.txt>

[12] Peter Hutterer et al. "X Input Extension Version 2.0 (XI2)."
X11R7.7 inputproto.
<https://www.x.org/releases/X11R7.7/doc/inputproto/XI2proto.txt>

[13] Keith Packard. "The X Sync Extension." X11R7.7 syncproto.
<https://www.x.org/releases/X11R7.7/doc/syncproto/sync.txt>

[14] Ishizue project. "Ishizue Window Server Library Specification."
SPEC.md, sections 6.6, 6.7, 7.3, 7.5, 8.
<https://github.com/Rui-727/Ishizue/blob/main/SPEC.md>

[15] Freedesktop.org. "Extended Window Manager Hints: Root Window
Properties (and Related Messages)."
<https://specifications.freedesktop.org/wm/latest/ar01s03.html>

[16] Freedesktop.org. "Extended Window Manager Hints: Application
Window Properties." <https://specifications.freedesktop.org/wm/latest/ar01s05.html>

The x.org spec pages were reachable; the freedesktop cgit raw-text URLs
for `presentproto` and `dri3proto` were also reachable as of writing.
The DRI3 v1.2 / v1.3 specification is not yet in an x.org release; the
most authoritative source is the explicit-sync development branch of
xorgproto [8], which matches the libxcb 1.17 man-page description of
`xcb_dri3_import_syncobj` [8]. The wlroots / Xwayland source code
(covered in the companion `xwayland-architecture.md` and
`xserver-implementations-comparison.md` research notes) corroborates
the wire formats described here for the extension requests; this
document only cites the protocol specs to keep the references
authoritative.
