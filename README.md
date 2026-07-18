# Ishizue (礎)

Ishizue is a mechanism layer for tiling window managers on Linux: a dynamic
library (`.so`) that owns display output (DRM/KMS), input (libinput),
buffer/GPU management, and a custom client protocol transport. It is
analogous in role to wlroots, but built around a fully custom, non-Wayland
client protocol. There is no WM daemon and no bundled tiling logic.

The **Architect** is whoever links Ishizue and writes the actual tiling WM:
same process, direct function calls, no IPC boundary between mechanism and
policy. Ishizue is the foundation; the Architect is what's built on it.

## Status

Pre-implementation. The target-complete v1 specification lives in
[`SPEC.md`](SPEC.md). The codebase is being assembled section by section;
until §1–§15 of the spec are covered, the library does not work.

## Governing philosophy

Bare minimum, mechanism-only, zero unilateral policy decisions. At every
point where the library could be tempted to "help" by making a decision
(cursor fallback, memory eviction, crash recovery, plane sharing), it
exposes the mechanism and leaves the decision to the Architect instead of
making it silently.

## Build

Requirements: a C11 compiler, libdrm, libinput, libseat, libxkbcommon, and
pkg-config. Atomic KMS is a hard runtime requirement (§3); the build does
not enforce it, but the library fails fast at backend-init if unavailable.

```
make
make test      # headless-backend integration tests
make install   # defaults to /usr/local
```

Build-time resource limits (§4) and feature flags are overridable:

```
make ISZ_MAX_SURFACES_PER_CLIENT=128 ENABLE_HDR=0
```

## Layout

```
include/ishizue/   Public API. The contract; never break ABI within a major.
src/               Implementation, organised by subsystem.
src/backend/       DRM, headless, and (post-v1) nested backends.
src/protocol/      Custom wire protocol: framing, handshake, object model.
src/render/        GLES pass-through, atomic KMS commits, plane slots.
src/input/         libinput wrapper, keymap cache, event dispatch.
src/buffer/        DMA-BUF import, release tracking, recycling.
src/util/          Logging, lists, threads, test hooks.
doc/               api.md, protocol.md, getting_started.md.
tests/             Headless-backed unit and integration tests.
SPEC.md            The spec. Authoritative.
```

## License

MIT. See [`LICENSE`](LICENSE).
