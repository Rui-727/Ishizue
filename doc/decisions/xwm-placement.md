# XWM placement: in-library vs in-bridge

Decision document for Ishizue (W7-A). Settles where the XWM-equivalent
code lives: in `libishizue.so`, in the X11 bridge process, or in a
separate library linked by the bridge.

Status: final. The next implementation wave should treat this as the
authoritative reference for where every XWM responsibility sits.

## Question

SPEC §13 mandates an X11 compat bridge as a separate process that
connects to Ishizue as an ordinary client. The bridge listens on
`/tmp/.X11-unix/X<n>` and translates X11 wire bytes into Ishizue
surface, buffer, and input operations.

In wlroots, the XWM (X Window Manager) is the code that speaks the X11
protocol back to Xwayland in order to honor ICCCM and EWMH: it
subscribes to `MapRequest` and `ConfigureRequest`, synthesizes
`ConfigureNotify` per ICCCM 4.1.5, sets `_NET_WM_STATE_*` and
`_NET_ACTIVE_WINDOW` on the root, drives `WM_TAKE_FOCUS` and
`XSetInputFocus` per the client's ICCCM input model, runs the Xdnd state
machine, and bridges `CLIPBOARD` / `PRIMARY` selections through XFixes
and INCR. In wlroots this code lives in `xwayland/xwm.c` (2941 lines)
inside `libwlroots`, and runs in the compositor process, talking to a
separate Xwayland process over the `wm_fd` socketpair.

Ishizue has no separate Xwayland. The bridge is itself the X11 server.
So the wlroots split (Xwayland in one process, XWM in another) does not
map directly. The question is where the XWM-equivalent code lives in
Ishizue:

- Option A: in `libishizue.so`, exposed as `isz_x11_*` APIs.
- Option B: in the bridge process, as an internal module.
- Option C: in a separate library `libishizue-xwm` linked by the bridge
  (and optionally by an Architect who wants an in-process XWM).

The W6-C research doc recommended Option A: "XWM should live in the
library, not the bridge process. Mirror wlroots' wm_fd channel: bridge
= thin X11 byte-level translator, library = EWMH/ICCCM compliance
owner." That recommendation is in direct tension with SPEC §13 as
written and with W6-A, which concluded the opposite. This document
resolves the tension.

What "XWM" means here is fixed: it is the X11 protocol compliance layer
(ICCCM plus EWMH plus Xdnd plus selection bridging). It is not the
WM policy layer. The Architect decides focus, stacking, placement,
fullscreen, and tiling. The XWM reflects those decisions in X11 protocol
terms. This distinction is load-bearing for the §1 analysis below.

## Option A: in-library

The XWM module lives inside `libishizue.so`. The library grows a public
`isz_x11_*` API surface. The bridge shrinks to a thin X11 byte-level
translator: it parses the X11 wire format, forwards request and reply
bytes to the library over a dedicated channel, and the library owns
EWMH/ICCCM compliance, surface<->window mapping, and ICCCM state
machines.

Arguments for:

- Mirrors wlroots directly. Architects coming from wlroots get the same
  shape: `isz_x11_surface_focus()` is the Ishizue analogue of
  `wlr_xwayland_surface_activate()`.
- One implementation, reused by every Architect. No risk of forks
  diverging on ICCCM compliance.
- The XWM can call directly into library internals without IPC, since
  the surface state, focus state, and seat state all live in the same
  process.
- ICCCM input model selection can be hidden behind one call:
  `isz_x11_surface_focus()` inspects `WM_HINTS.input` and
  `WM_PROTOCOLS` and picks `Passive` / `Local` / `Globally Active` /
  `None` internally, instead of making the Architect choose.

Arguments against:

- Contradicts SPEC §13 as written. The SPEC says the library "provides
  no special-cased APIs for it" and "The v1 library itself doesn't need
  to know X11 exists." Option A makes the library X11-aware and adds
  X11-specific APIs. Adopting Option A requires rewriting §13.
- Couples X11 protocol evolution to the library's ABI. Every EWMH atom
  addition, every ICCCM clarification, every new X11 extension the XWM
  learns to bridge becomes a potential `libishizue.so` ABI bump. The
  SPEC's semver contract (§4) becomes a function of X11 protocol churn,
  not just Ishizue's own evolution.
- Pulls X11 dependencies (libxcb, or a hand-rolled X11 wire marshaler)
  into the library link line. Architects who do not use X11 compat pay
  the dependency cost anyway.
- Fault isolation loss. X11 protocol parsing accepts untrusted bytes
  from any X11 client. An XWM bug in the library crashes the
  compositor process, taking down every native client too.
- Multi-instance complication. Per-app sandboxed bridges (W6-A §11) want
  independent XWM state per bridge. With the XWM in the library, the
  library has to track per-bridge XWM state inside one process, keyed
  by the bridge's client connection. Doable, but it pollutes the
  library's data model with X11-specific per-client state.
- Testability loss. XWM tests now require the full library
  (libseat, libinput, GBM, EGL, even with the headless backend) where
  they could otherwise run against an X11 client simulator with no GPU
  in sight.
- Code size. EWMH/ICCCM is non-trivial: wlroots' `xwm.c` is 2941 lines,
  plus selection (`selection.c`, `incoming.c`, `outgoing.c`, `dnd.c`)
  for another 2500 lines. That is roughly 5k lines added to the library.

## Option B: in-bridge

The XWM lives inside the bridge process as an internal module. The
bridge is both the X11 server and the XWM, which W6-A identified as
unavoidable given Ishizue's single-process bridge model. The library
stays X11-agnostic. The Architect never sees an X11-specific API; they
call `isz_seat_set_keyboard_focus(surf)`, `isz_surface_set_zpos(surf,
n)`, `isz_surface_set_position(surf, x, y)` on whatever surface they
want, and the bridge listens for the corresponding events and translates
them into X11 protocol operations.

Arguments for:

- Matches SPEC §13 as written. No SPEC rewrite needed. The "no
  special-cased APIs" and "library doesn't need to know X11 exists"
  sentences are honored as-is.
- Keeps the library small and focused. The 5k lines of XWM code stay in
  the bridge binary, which is not a versioned library and has no ABI
  contract.
- Fault isolation. An XWM bug crashes the bridge, not the compositor.
  The compositor can detect the bridge exit and surface it to the
  Architect, who can restart it. Native clients keep running.
- Multi-instance is trivial. Each bridge process owns its own XWM
  state. Per-app sandboxed bridges work with zero library-side
  coordination.
- Testability. The XWM module can be unit-tested by linking the bridge
  objects against a stub Ishizue client and a fake X11 client that
  speaks the wire protocol. No DRM, no GPU, no libseat.
- The Architect's surface API is uniform across native and X11. There
  is no `isz_x11_surface_*` family to learn; X11 surfaces are just
  surfaces created by the bridge. That matches SPEC §13's "treated as
  an ordinary Ishizue client, using the same primitives everyone else
  uses."
- W6-A explicitly concluded this: "Since the Ishizue bridge is both
  the X server and the XWM, the bridge must have an internal XWM module
  that drives X11 focus, stacking, and configure state in response to
  Ishizue events. This is unavoidable."

Arguments against:

- Diverges from wlroots' literal pattern. wlroots puts the XWM in
  `libwlroots`, not in a separate process. Architects coming from
  wlroots have to relearn where the XWM lives. (Counter: the wlroots
  pattern exists because wlroots has a separate Xwayland process. The
  situations are not analogous; see the wlroots analysis below.)
- Bridge code size grows. From the current 5k-line scaffold toward
  perhaps 12-15k lines once a real X11 wire parser plus XWM logic
  lands. Still well under Xorg's 322k and GameScope's 54k, but no
  longer the kdrive-sized 5k ceiling W6-D floated.
- Every Architect who wants X11 compat uses the same bridge binary, so
  the XWM is implemented once anyway. (This is also true under Option
  A; it is not a differentiator.)
- The Architect cannot call a direct `isz_x11_surface_focus()` that
  internally picks the ICCCM input model. They call
  `isz_seat_set_keyboard_focus(surf)`; the bridge receives the focus
  event and applies the ICCCM input model itself. This is a minor loss
  of convenience, not a loss of capability.

## Option C: separate library libishizue-xwm

A separate static or shared library that contains the XWM logic. The
bridge links it. The main library stays X11-agnostic. In principle an
Architect could also link `libishizue-xwm` directly if they wanted an
in-process XWM (for example, a single-process kiosk compositor that
talks X11 directly without a bridge).

Arguments for:

- Reuses the XWM code in both the bridge and any in-process consumer.
- Keeps the main library X11-agnostic without forcing the bridge to
  carry all the XWM code inline.
- Lets the XWM be tested in isolation, as a library, with no scaffold.

Arguments against:

- No real consumer for the in-process variant. SPEC §13 fixes the
  bridge as a separate process. No other consumer is on the roadmap.
  Building a library to serve one consumer is ceremony.
- Multiplies the ABI stability surface. If `libishizue-xwm` is a public
  shared library, it needs its own version script, its own soname, its
  own semver. SPEC §4 already commits the project to one stable ABI
  for `libishizue.so`; adding a second one doubles the maintenance
  cost for no benefit.
- A static library avoids the ABI problem but loses the reuse benefit,
  since the only consumer is the bridge and the bridge is built from
  source alongside it.
- The testability benefit is real but already available under Option B
  by linking bridge objects into a test binary.
- Option C is Option B with extra packaging. If a second consumer ever
  appears, the bridge's XWM module can be split out at that point
  without architectural change. YAGNI applies.

## Analysis against SPEC §1

SPEC §1's governing philosophy: "bare minimum, mechanism-only, zero
unilateral policy decisions." The library "does not own tiling logic,
focus policy, hotkey bindings, stacking order policy, or any WM
decision-making."

The XWM is mechanism, not policy. The XWM does not decide which window
has focus, where a window is placed, what stacking order is in effect,
or whether a window is fullscreen. Those decisions are the Architect's,
made via `isz_seat_set_keyboard_focus()`, `isz_surface_set_position()`,
`isz_surface_set_zpos()`, and the surface state the bridge reads back
through events. The XWM reflects those decisions in X11 protocol terms:
it sends `ConfigureNotify` when the Architect moves a window, sets
`_NET_WM_STATE_FULLSCREEN` when the Architect fullscreens a surface,
calls `XSetInputFocus` on the X11 window corresponding to the focused
Ishizue surface.

What the XWM does decide, on its own, is protocol mechanics:

- Whether to send a synthetic `ConfigureNotify` after a configure (ICCCM
  4.1.5 says yes, always).
- Which ICCCM input model to use for a given surface (based on
  `WM_HINTS.input` and `WM_PROTOCOLS`).
- How to translate MIME types to X11 atoms and back.
- How to chunk INCR transfers.
- How to drive the Xdnd state machine.

Those are protocol obligations, not policy choices. They are exactly
analogous to the library's existing "mechanism, not policy" boundary:
the library commits an atomic KMS state when the Architect tells it to,
but does not pick which plane a surface goes on. The XWM sends
`ConfigureNotify` when the Architect moves a surface, but does not pick
where the surface goes.

So SPEC §1 by itself does not forbid putting the XWM in the library.
The XWM is mechanism wherever it lives. The §1 question is largely a
wash between Options A, B, and C. SPEC §1 neither mandates nor forbids
any of them.

## Analysis against SPEC §13

SPEC §13 has two load-bearing sentences:

1. "The library provides no special-cased APIs for it; it's treated as
   a privileged client via the allowlist (§6.3), using the same
   primitives everyone else uses."
2. "The v1 library itself doesn't need to know X11 exists."

Option A contradicts both. To put the XWM in the library, the library
has to know X11 exists (it has to know about `MapRequest`,
`ConfigureNotify`, `_NET_WM_STATE`, ICCCM input models, INCR, Xdnd). It
also has to expose special-cased APIs (`isz_x11_surface_focus`,
`isz_x11_surface_set_state`, etc.) that exist only because X11 compat
exists. Adopting Option A requires rewriting both sentences.

Option B honors both. The library is X11-agnostic; the bridge is a
privileged client that uses the same primitives as any other client;
the XWM is an internal module of the bridge, not a library API.

Option C honors both at the main library level. `libishizue.so` stays
X11-agnostic. `libishizue-xwm` is a separate artifact, linked only by
the bridge. The main library still has no special-cased APIs.

SPEC §13 is the deciding factor. The SPEC was written with Option B in
mind: the bridge is a separate process, the library is X11-agnostic,
the Architect uses one uniform surface API. Option A reverses that
direction. Option C is technically consistent with §13 but introduces
a second library that the SPEC does not call for and that has no
consumer beyond the bridge.

## Analysis against wlroots precedent

W6-C recommendation #1 argued for Option A by appeal to wlroots: mirror
the wm_fd channel, put the XWM in the library, make the bridge a thin
translator.

The wlroots precedent does not stretch that far. wlroots has two
processes: Xwayland (the X11 server) and the compositor (which contains
the XWM). The wm_fd is an inter-process socketpair between them. The
XWM has to live somewhere that can speak X11 to Xwayland over that
socketpair, and libwlroots is where it lives, because the compositor
process is the one that links libwlroots.

Ishizue has one process for X11: the bridge. The bridge IS the X11
server. There is no separate Xwayland. There is no wm_fd to mirror,
because there is no second process to mirror it with. The library and
the bridge are already in separate processes, connected by the native
wire protocol defined in §6. Adding a "dedicated socketpair" between
them, as W6-C proposed, is just adding a second channel for messages
that already fit on the first.

The right way to read the wlroots precedent is: the XWM lives with
whichever side speaks X11. In wlroots, that is the compositor process
(the XWM speaks X11 to Xwayland over wm_fd; the compositor does not
speak X11 to anything else). In Ishizue, that is the bridge process
(the bridge speaks X11 to X11 clients; the library does not speak X11
to anything). W6-A reached the same conclusion: the bridge is both the
X11 server and the XWM, because Ishizue has no second process to split
the roles across.

W6-C and W6-A disagree. W6-A's reading matches SPEC §13. W6-C's
recommendation requires rewriting SPEC §13. When two research notes
conflict and one of them matches the SPEC as written, the SPEC wins
unless there is a concrete reason to override it. No such reason has
been offered.

The wm_fd analogy specifically fails. wlroots' wm_fd exists because the
XWM has to speak X11 to a separate Xwayland process. In Ishizue, an
in-library XWM would be speaking X11 to itself, since the X11 server
would also be in the library. The "dedicated socketpair" W6-C proposed
would carry Ishizue-protocol messages, not X11 wire bytes. It is a
second channel for messages that already fit on the first.

## Engineering trade-offs

Code size. Option A adds roughly 5k lines to `libishizue.so` (xwm.c
plus selection plus dnd, by analogy to wlroots). The library is
currently around 15k LOC; that is a 33% growth in one feature. Option B
adds the same 5k lines to the bridge, which is currently a 5k-line
scaffold and would grow to around 12-15k. W6-D's "5k ceiling" for the
bridge was an aspirational bound based on kdrive's standalone X server,
not a hard limit; the bridge will not stay under 5k once it implements
real X11 wire parsing plus XWM logic. 12-15k is still well under
GameScope's 54k and Xorg's 322k.

ABI stability. Option A couples X11 protocol evolution to
`libishizue.so`'s ABI. Adding a new EWMH atom the XWM understands, or
fixing an ICCCM edge case, can ripple into a minor or major version
bump. The SPEC's semver contract (§4) becomes a function of X11
protocol churn. Option B keeps all X11 churn inside the bridge binary,
which is not versioned and has no ABI contract. Option C moves the
churn into `libishizue-xwm`, which would need its own ABI story.

Fault isolation. X11 protocol parsing is the highest-risk code in the
system. It accepts untrusted bytes from any X11 client that connects to
`/tmp/.X11-unix/X<n>`. A bug in the XWM, the wire parser, or the
selection state machine can corrupt memory, leak fds, or deadloop.
Under Option B, that bug crashes the bridge process; the compositor
detects the bridge exit and surfaces it. Native clients and the display
pipeline keep running. Under Option A, the same bug crashes the
compositor process; everything dies. Under Option C, the bug crashes
the bridge (since the bridge is the only consumer of the XWM library
code), same as Option B.

Testability. Under Option B and Option C, the XWM can be unit-tested
by linking the XWM objects against a stub Ishizue protocol client and a
fake X11 client that speaks the wire protocol. No DRM, no GPU, no
libseat, no libinput. Under Option A, the XWM is woven into the
library's surface and focus state; tests need at least the headless
backend and the full library initialization path.

Performance. Option B has one IPC hop on focus changes: the Architect
calls `isz_seat_set_keyboard_focus()`, the library emits an event, the
bridge receives it and translates to X11 ICCCM focus. That hop is
unavoidable under any option, because the bridge is a separate process
and has to learn about focus changes somehow. Option A does not remove
the hop; it just adds an in-library path that also has to track focus
state. The native-protocol event delivery to the bridge is the
bottleneck either way, and it is already async by design.

Multi-instance support. Per-app sandboxed bridges (W6-A §11) want each
bridge process to own its own XWM state. Under Option B this is
trivial: each bridge process is independent. Under Option A the
library has to track per-bridge XWM state inside one process, keyed by
the bridge's client connection. This pollutes the library's data model
with X11-specific per-client state and adds locking concerns. Under
Option C each bridge links its own copy of `libishizue-xwm` and gets
the same trivial isolation as Option B.

## Decision

Option B. The XWM lives in the bridge process as an internal module.
The library stays X11-agnostic. No `isz_x11_*` public API is added.

Concrete justification:

1. SPEC §13 as written mandates it. The "no special-cased APIs" and
   "library doesn't need to know X11 exists" sentences are
   unambiguous. Option A requires SPEC changes that reverse the
   architectural direction. Option B requires none.
2. The wlroots precedent does not extend to Ishizue's process model.
   wlroots has a separate Xwayland process; Ishizue does not. The
   wm_fd analogy fails because there is no second process to mirror it
   with. The right reading of wlroots is "the XWM lives with whichever
   side speaks X11," and in Ishizue that is the bridge.
3. W6-A and W6-C disagree. W6-A's reading matches the SPEC. W6-C's
   recommendation contradicts SPEC §13. When research notes conflict
   and one matches the SPEC as written, the SPEC wins unless there is
   a concrete reason to override it. None has been offered.
4. Fault isolation. X11 protocol parsing is untrusted-byte handling.
   It belongs in a process that can crash without taking the
   compositor with it. The bridge process is exactly that.
5. Multi-instance. Per-app sandboxed bridges work with zero
   library-side coordination. Under Option A they require per-bridge
   XWM state inside the library.
6. ABI stability. X11 protocol churn stays inside the bridge binary.
   `libishizue.so`'s semver is a function of Ishizue's own evolution,
   not X11's.
7. The Architect's surface API stays uniform. X11 surfaces are
   indistinguishable from native surfaces at the API level, matching
   SPEC §13's "same primitives everyone else uses."

Option C is rejected. It is Option B with extra packaging and no
consumer for the in-process variant. If a second consumer ever appears
(for example, an Architect that wants to embed X11 handling in-process
for a kiosk), the bridge's XWM module can be split out at that point.

Option A is rejected. It contradicts SPEC §13 as written, couples X11
churn to the library's ABI, loses fault isolation, complicates
multi-instance, and rests on a wlroots analogy that does not survive
Ishizue's process model.

## SPEC changes required

None that reverse the architecture. SPEC §13 already says what Option B
needs. The only SPEC work is tightening, not rewriting:

- §13 should add a sentence stating that the bridge owns ICCCM and
  EWMH compliance as an internal module, and that the library does not
  expose any X11-specific public API. This forecloses future
  re-litigation of this question.
- §13 should add a sentence stating that the bridge is the X11 server
  and the XWM in a single process, and that no separate Xwayland-style
  process exists. This makes the process model explicit.
- §6.8 should grow a primary-selection API. W6-A flagged this gap: X11
  has both `PRIMARY` and `CLIPBOARD`, but SPEC §6.8 only defines the
  clipboard. The bridge needs both. This is a pre-existing gap, not
  something Option B introduces; it just becomes load-bearing once
  the bridge implements selection bridging.
- §9 should explicitly mention that the bridge listens for
  `ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED` to drive X11 focus
  translation. This is already implied by the event's existence, but
  spelling it out helps the next implementer.

W6-C's recommendation #2 (a first-class `x11_toplevel` surface role in
§6.4) is not adopted. Under Option B, an X11 window is just an Ishizue
surface created by the bridge as a regular client. The bridge tracks
the X11 window ID internally and associates it with the Ishizue surface
ID. No new surface role is needed, because there is no wire-protocol
association race to fix: the bridge creates both the X11 window and the
Ishizue surface, so the association is in-bridge state, not a
cross-process wire pairing.

## Implementation consequences

Code organization:

- The bridge grows an `xwm` module. Suggested files:
  - `x11bridge/xwm.c`: top-level XWM state, root window property
    maintenance, `WM_S0` ownership, `SUBSTRUCTURE_REDIRECT` install.
  - `x11bridge/xwm_focus.c`: ICCCM input model selection,
    `XSetInputFocus`, `WM_TAKE_FOCUS`, `FocusIn` / `FocusOut`
    synthesis.
  - `x11bridge/xwm_property.c`: per-atom property reading dispatch,
    `_NET_WM_STATE`, `_NET_WM_NAME`, `WM_PROTOCOLS`, `WM_HINTS`,
    `WM_NORMAL_HINTS`.
  - `x11bridge/xwm_configure.c`: `ConfigureRequest` handling,
    synthetic `ConfigureNotify` per ICCCM 4.1.5.
  - `x11bridge/xwm_selection.c`: `CLIPBOARD` and `PRIMARY` bridging,
    XFixes selection notify, INCR state machine, MIME-to-atom
    translation.
  - `x11bridge/xwm_dnd.c`: Xdnd state machine, the
    `XdndAware` proxy window, `XdndEnter` / `Position` / `Status` /
    `Drop` / `Finished`.
  - `x11bridge/ewmh.c`: `_NET_SUPPORTED`, `_NET_CLIENT_LIST`,
    `_NET_ACTIVE_WINDOW`, `_NET_WORKAREA`, `_NET_DESKTOP_VIEWPORT`,
    and friends.
- The bridge's existing `translation.c` becomes the X11 wire parser
  that turns X11 requests into XWM-level calls. The split is: wire
  parser reads bytes and emits typed requests; XWM module handles
  typed requests and drives Ishizue surfaces; `isz_client.c` sends
  Ishizue wire messages.
- The library gains no new files, no new public APIs, no new
  dependencies.

Bridge responsibilities, concretely:

- Listen on `/tmp/.X11-unix/X<n>`, accept X11 client connections, parse
  the X11 wire format, send errors/events/replies.
- Acquire `WM_S0` and `SUBSTRUCTURE_REDIRECT` on itself at startup,
  before any X11 client connects. This is the XWM role from X11's
  perspective.
- For each X11 top-level window, create a corresponding Ishizue
  surface via `ISZ_MSG_SURFACE_CREATE`. Track X11 window ID to Ishizue
  surface ID in an in-memory hash table.
- On `MapRequest`: surface exists and is ready to be placed. The bridge
  does not decide where.
- On `ConfigureRequest` from the X11 client: surface the requested
  geometry to the Architect (placement is policy; the bridge cannot
  grant it unilaterally). If the Architect accepts, synthesize the
  synthetic `ConfigureNotify` per ICCCM 4.1.5.
- On `ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED` for an X11 surface:
  drive the X11 side. Pick the ICCCM input model from the client's
  `WM_HINTS.input` and `WM_PROTOCOLS`. Call `XSetInputFocus` or send
  `WM_TAKE_FOCUS` accordingly. Send `FocusIn` / `FocusOut`.
- On `ISZ_EVENT_CLIPBOARD_REQUEST`: drive the ICCCM `XConvertSelection`
  dance against the X11 owner. Handle INCR. Pipe to the fd the library
  provided.
- On X11 client selection ownership (via XFixes): set Ishizue
  clipboard ownership, offering the X11 targets translated to MIME
  types.
- Maintain `_NET_CLIENT_LIST` in map-time order from day one (W6-C
  finding #3: wlroots gets this wrong with only a creation-order
  list). Keep two lists.
- Translate Xdnd to Ishizue drag-and-drop (§6.9) and back. Use a
  single source of truth for "what surface is under the pointer" on
  the Ishizue side; do not let the X11 client's `XdndPosition`-based
  notion of the target diverge.
- DRI3 DMA-BUF for X11 client buffers, not `ShmPutImage`. The bridge
  maps X11 DRI3 requests onto the existing Ishizue DMA-BUF import
  path (§8). `ShmPutImage` and `PutImage` are slow paths for non-GL
  X11 clients.
- The bridge must not grow a DIX (W6-D finding #4). The XWM module is
  protocol-level (EWMH, ICCCM), not a device-independent X server
  core. The bridge translates; it does not implement X11 drawing
  primitives.

Library responsibilities, concretely:

None new. The library treats the bridge as any other client. The bridge
uses existing APIs (`isz_seat_set_keyboard_focus`,
`isz_surface_set_position`, `isz_surface_set_zpos`,
`isz_surface_set_size`, `isz_surface_set_plane_type`,
`isz_surface_set_plane_slot`, `isz_surface_attach_buffer`,
`isz_commit`, etc.) and listens for existing events
(`ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED`,
`ISZ_EVENT_CLIPBOARD_REQUEST`, `ISZ_EVENT_CLIENT_CONNECT`,
`ISZ_EVENT_CLIENT_DISCONNECT`).

Architect responsibilities, concretely:

- None new for X11 specifically. The Architect allowlists the bridge
  binary (`isz_allowlist_add_binary`, §6.3), launches the bridge
  process, and treats X11 surfaces exactly like native surfaces for
  focus, placement, and stacking decisions.
- The Architect does not call any `isz_x11_*` function. None exists.

Multi-instance:

- The Architect can launch multiple bridge processes, each with its
  own display number and X11 socket. Each bridge is an independent
  Ishizue client. Per-app sandboxed bridges work without library
  coordination.
- Inter-bridge X11 communication (one X11 app on bridge A talking to
  another on bridge B) is out of scope, per W6-A §11. If it ever
  matters, it is the Architect's problem.

## Open questions

- ICCCM input model selection: the bridge can inspect the X11 client's
  `WM_HINTS` and `WM_PROTOCOLS` on its own when it receives
  `ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED`. No library change needed.
  Confirm during bridge focus implementation.
- Whether to expose the X11 window ID to the Architect as an opaque
  surface property (diagnostics only, no policy effect). Decide when
  the surface-property API is designed.
- EWMH urgency hints (`_NET_WM_STATE_DEMANDS_ATTENTION`): surface as a
  generic surface property if SPEC grows one, otherwise defer.
- Idle-inhibit translation (W6-A §12): `XScreenSaverSuspend(True)`
  needs an Ishizue-side equivalent. SPEC gap to resolve when
  idle-inhibit lands.
- Pointer barriers (W6-A §12): W6-A recommends declaring unsupported.
  Confirm in the bridge implementation.
- Whether to ship `libishizue-xwm` later, if a second consumer
  appears. Not now. Track as a deferred item, not an open question.
