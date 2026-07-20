/* SPDX-License-Identifier: MIT */
/*
 * isz_protocol.h - Ishizue client wire protocol, framing and constants.
 *
 * Implements the binary wire format defined in SPEC §6.1:
 *
 *   [ length (u32 LE) ][ msg_id (u32 LE) ][ payload (length - 4 bytes) ]
 *
 * The 32-bit length field counts the bytes that follow it (msg_id + payload),
 * so total wire bytes for a message are 4 + length. payload_len is therefore
 * length - 4. A message with no payload has length == 4.
 *
 * All multi-byte fields are little-endian on the wire, including length and
 * msg_id. The single exception is the 4-byte connection magic "ISZH"
 * (0x49535A48), transmitted big-endian so the protocol can be identified by
 * a byte-order-independent signature, then followed by a 32-bit LE protocol
 * version. See SPEC §6.2.
 *
 * DMA-BUF file descriptors travel as ancillary data (SCM_RIGHTS) on the same
 * sendmsg call as the message that references them. The payload carries a
 * 32-bit fd_index slot that maps into the cmsg's fd array; the server does
 * not interpret the slot, it just maps slot -> cmsg_fds[slot].
 *
 * This is an internal header. Public API surface lives in <ishizue/isz.h>.
 */
#ifndef ISHIZUE_PROTOCOL_H
#define ISHIZUE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "../util/isz_compiler.h"

/* Big-endian wire magic: bytes on the wire are 'I','S','Z','H' (0x49 0x53
 * 0x5A 0x48). Stored as a host-order u32 for comparison convenience. */
#define ISZ_PROTOCOL_MAGIC     0x49535A48u
#define ISZ_PROTOCOL_MAGIC_STR "ISZH"

/* Current wire protocol version (SPEC §6.1, separate from library semver). */
#define ISZ_PROTOCOL_VERSION   1u

/* Upper bound on a single wire message (length + msg_id + payload).
 * v1 messages are small: surface state, plane slots, clipboard metadata
 * (real clipboard bytes travel in a passed fd, not the payload). 8 KiB is
 * generous headroom over the largest expected v1 message. */
#define ISZ_PROTO_MAX_MESSAGE  8192u

/* Upper bound on fds carried in a single message's SCM_RIGHTS cmsg.
 * The kernel limit (SCM_MAX_FD, typically 253) is the hard ceiling; v1
 * uses one fd per buffer attach / clipboard transfer, 8 is plenty. */
#define ISZ_PROTO_MAX_FDS      8u

/* Bytes on the wire for [length|msg_id]. */
#define ISZ_MSG_HEADER_SIZE    8u

/* ------------------------------------------------------------------ */
/* Message IDs                                                         */
/* ------------------------------------------------------------------ */
/* Direction comments: S2C = server-to-client event, C2S = client-to-
 * server request. Both directions share one id space per SPEC §6.1. */
enum isz_msg_id {
    ISZ_MSG_INVALID = 0,

    /* Handshake (S2C). */
    ISZ_MSG_HANDSHAKE_DONE = 1,
    ISZ_MSG_GLOBAL         = 2,  /* output or seat broadcast, see payload kind */
    ISZ_MSG_CAPABILITIES   = 3,

    /* Surface lifecycle (C2S unless noted). */
    ISZ_MSG_SURFACE_CREATE            = 4,
    ISZ_MSG_SURFACE_DESTROY           = 5,
    ISZ_MSG_SURFACE_ATTACH_BUFFER     = 6,  /* carries a dmabuf fd */
    ISZ_MSG_SURFACE_DAMAGE            = 7,
    ISZ_MSG_SURFACE_SET_POSITION      = 8,
    ISZ_MSG_SURFACE_SET_SIZE          = 9,
    ISZ_MSG_SURFACE_SET_PLANE_TYPE    = 10,
    ISZ_MSG_SURFACE_SET_PLANE_SLOT    = 11,
    ISZ_MSG_SURFACE_SET_ZPOS          = 12,
    ISZ_MSG_SURFACE_SET_TRANSFORM     = 13,
    ISZ_MSG_SURFACE_CREATE_SUBSURFACE = 14,
    ISZ_MSG_SURFACE_CREATE_POPUP      = 15,
    ISZ_MSG_SURFACE_CREATE_LAYER      = 16,

    /* Buffer lifecycle. RELEASE is S2C: server tells client a previously
     * attached buffer is no longer in use and may be reused or destroyed. */
    ISZ_MSG_BUFFER_DESTROY = 17,
    ISZ_MSG_RELEASE        = 18,

    /* Seat (C2S for set-focus requests from the Architect path; S2C as
     * the wire delivery of focus/cursor changes to clients). */
    ISZ_MSG_SEAT_SET_KEYBOARD_FOCUS = 19,
    ISZ_MSG_SEAT_SET_CURSOR_SURFACE = 20,

    /* Commit a surface's pending state to the active output (C2S). */
    ISZ_MSG_COMMIT = 21,

    /* Output control (C2S: the Architect drives these via the public
     * API; on the wire they reach the server's protocol layer too). */
    ISZ_MSG_OUTPUT_ENABLE          = 22,
    ISZ_MSG_OUTPUT_DISABLE         = 23,
    ISZ_MSG_OUTPUT_SET_DPMS        = 24,
    ISZ_MSG_OUTPUT_SET_GAMMA       = 25,
    ISZ_MSG_OUTPUT_SET_DEGAMMA     = 26,
    ISZ_MSG_OUTPUT_SET_CTM         = 27,
    ISZ_MSG_OUTPUT_SET_HDR_METADATA = 28,

    /* Screen capture (C2S start/stop, S2C done). */
    ISZ_MSG_CAPTURE_START = 29,
    ISZ_MSG_CAPTURE_STOP  = 30,
    ISZ_MSG_CAPTURE_DONE  = 31,

    /* Presentation feedback (S2C). */
    ISZ_MSG_PRESENTED = 32,

    /* Drag-and-drop. START is C2S, MOTION/DROP are S2C, ACCEPT/REJECT
     * are C2S from the surface under the pointer. */
    ISZ_MSG_DRAG_START  = 33,
    ISZ_MSG_DRAG_MOTION = 34,
    ISZ_MSG_DRAG_ACCEPT = 35,
    ISZ_MSG_DRAG_REJECT = 36,
    ISZ_MSG_DRAG_DROP   = 37,

    /* Popup grab release (C2S). */
    ISZ_MSG_POPUP_DISMISS = 38,

    /* Clipboard (C2S set/request, both may carry an fd). */
    ISZ_MSG_CLIPBOARD_SET     = 39,
    ISZ_MSG_CLIPBOARD_REQUEST = 40,

    /* Input events (S2C). Raw keycodes per SPEC §6.3; no keymap
     * interpretation by the library. */
    ISZ_MSG_INPUT_KEYBOARD_KEY       = 41,
    ISZ_MSG_INPUT_KEYBOARD_MODIFIERS = 42,
    ISZ_MSG_INPUT_POINTER_MOTION     = 43,
    ISZ_MSG_INPUT_POINTER_BUTTON     = 44,
    ISZ_MSG_INPUT_POINTER_AXIS       = 45,
    ISZ_MSG_INPUT_TOUCH_DOWN         = 46,
    ISZ_MSG_INPUT_TOUCH_MOTION       = 47,
    ISZ_MSG_INPUT_TOUCH_UP           = 48,
    ISZ_MSG_INPUT_TOUCH_FRAME        = 49,

    /* Generic fatal error notification (S2C), followed by disconnect. */
    ISZ_MSG_ERROR = 50,

    /* W8-B protocol additions (SPEC §6.4, §6.8, §6.15, §6.16, §7.2). */

    /* S2C: preferred fractional scale for a surface (§7.2).
     * Payload: u32 surface_id + u32 numerator + u32 denominator. */
    ISZ_MSG_SURFACE_PREFERRED_SCALE = 51,

    /* C2S: claim selection ownership for a (slot, timestamp) pair
     * (§6.8). Replaces the older CLIPBOARD_SET message for the
     * ownership-transfer step; the data-fd still travels via
     * SCM_RIGHTS as in §6.8. Payload: u32 slot + u64 timestamp_ns. */
    ISZ_MSG_SET_SELECTION_OWNER = 52,

    /* S2C: preedit text from the active input method (§6.16).
     * Payload: u32 text-input id + i32 cursor_begin + i32 cursor_end
     * + UTF-8 text bytes (no NUL terminator; length derived from
     * payload_len). */
    ISZ_MSG_TEXT_INPUT_PREEDIT = 53,

    /* S2C: committed text from the active input method (§6.16).
     * Payload: u32 text-input id + UTF-8 text bytes. */
    ISZ_MSG_TEXT_INPUT_COMMIT = 54,

    /* S2C: an output entered the idle-inhibit-active state (§6.15).
     * Payload: u32 output_id. */
    ISZ_MSG_IDLE_INHIBIT_ACTIVE = 55,

    /* S2C: an output returned to the idle-inhibit-inactive state
     * (§6.15). Payload: u32 output_id. */
    ISZ_MSG_IDLE_INHIBIT_INACTIVE = 56,
};

/* ------------------------------------------------------------------ */
/* Capabilities bitmask: payload of ISZ_MSG_CAPABILITIES              */
/* ------------------------------------------------------------------ */
/* Per SPEC §6.2 step 6. Per-server, not per-output. */
enum isz_capability {
    ISZ_CAP_HDR             = 1u << 0,
    ISZ_CAP_VRR             = 1u << 1,
    ISZ_CAP_TEARING         = 1u << 2,
    ISZ_CAP_SCREEN_CAPTURE  = 1u << 3,
    ISZ_CAP_CURSOR_SIZE_MAX = 1u << 4,  /* payload also carries max w/h */
};

/* ------------------------------------------------------------------ */
/* Wire layout structs (for reference; do not memcpy. Use the        */
/* encode/decode helpers, which handle byte order).                  */
/* ------------------------------------------------------------------ */
struct isz_msg_header {
    uint32_t length;   /* LE on wire: sizeof(msg_id) + payload_len */
    uint32_t msg_id;   /* LE on wire: enum isz_msg_id */
};

/* Payload-side slot for an fd passed via SCM_RIGHTS. Multiple of these
 * may appear in a single payload; the server resolves each fd_index
 * against the cmsg fd array carried with the message. payload_offset is
 * relative to the start of the payload (after the 8-byte header). */
struct isz_msg_fd {
    uint32_t fd_index;        /* index into the SCM_RIGHTS fd array */
    uint32_t payload_offset;  /* where in the payload this fd belongs */
};

/* ------------------------------------------------------------------ */
/* Per-connection partial-read state                                   */
/* ------------------------------------------------------------------ */
/* A single recvmsg may return fewer bytes than a complete message
 * (especially under load or when the kernel coalesces multiple sendmsg
 * calls). The receiver accumulates bytes here until it has at least
 * 4 + length, then parses one message and shifts the remainder down.
 *
 * SCM_RIGHTS fds are delivered by the kernel with the recvmsg that
 * returns the first byte of the message that carried them. If a message
 * is split across recvmsg calls, the fds arrive in the first chunk and
 * are stashed in pending_fds until the rest of the message arrives. */
struct isz_proto_recv_state {
    uint8_t  buf[ISZ_PROTO_MAX_MESSAGE];
    size_t   have;                          /* valid bytes in buf */
    int      pending_fds[ISZ_PROTO_MAX_FDS];
    size_t   n_pending_fds;                 /* fds awaiting completion */
    bool     in_progress;                   /* mid-message (have > 0) */
};

/* ------------------------------------------------------------------ */
/* Encode / decode                                                     */
/* ------------------------------------------------------------------ */
/* Encode a message into out_buf as [length|msg_id|payload]. out_buf
 * must hold at least ISZ_MSG_HEADER_SIZE + payload_len bytes. Returns
 * total bytes written (header + payload), or 0 if out_buf is too small
 * or payload_len would exceed ISZ_PROTO_MAX_MESSAGE. */
size_t isz_proto_encode(void *out_buf, size_t out_buf_len,
                        uint32_t msg_id,
                        const void *payload, size_t payload_len)
    ISZ_INTERNAL
    __attribute__((nonnull(1)));

/* Parse one message from in_buf. On success, *msg_id and *payload_len
 * are set, *payload points into in_buf (no copy), and the return value
 * is the number of bytes consumed (header + payload). Returns 0 if
 * in_buf does not yet contain a complete message (caller should read
 * more). Returns (size_t)-1 on a malformed length (message would exceed
 * ISZ_PROTO_MAX_MESSAGE). */
size_t isz_proto_decode(const void *in_buf, size_t len,
                        uint32_t *msg_id,
                        const void **payload,
                        size_t *payload_len)
    ISZ_INTERNAL
    __attribute__((nonnull(1, 3, 4, 5)));

/* ------------------------------------------------------------------ */
/* Socket I/O with SCM_RIGHTS                                          */
/* ------------------------------------------------------------------ */
/* Send one message + ancillary fds atomically via sendmsg. Returns
 * total bytes written on success (== ISZ_MSG_HEADER_SIZE + payload_len),
 * 0 on EAGAIN/EWOULDBLOCK (caller should retry when writable), or
 * (ssize_t)-1 on hard error (caller should treat as disconnect). fds
 * may be NULL iff n_fds == 0. */
ssize_t isz_proto_send_with_fds(int fd, uint32_t msg_id,
                                const void *payload, size_t payload_len,
                                const int *fds, size_t n_fds)
    ISZ_INTERNAL;

/* Send a pre-encoded frame (already in [length|msg_id|payload] layout)
 * with ancillary fds. Same return contract as isz_proto_send_with_fds.
 * Useful for draining an outbound queue that stores encoded frames. */
ssize_t isz_proto_send_frame(int fd, const void *frame, size_t frame_len,
                             const int *fds, size_t n_fds)
    ISZ_INTERNAL
    __attribute__((nonnull(2)));

/* Perform one recvmsg into out_buf. out_buf must be at least
 * ISZ_PROTO_MAX_MESSAGE bytes. out_fds (capacity ISZ_PROTO_MAX_FDS)
 * receives any fds delivered by this recvmsg; *out_n_fds is the count.
 * The return value is the number of bytes written into out_buf by this
 * call, which may be a partial message (the kernel can split a single
 * sendmsg across recvmsg calls). The caller parses out_buf with
 * isz_proto_decode; if 0 (incomplete), the caller must preserve the
 * partial bytes and fds itself; this function is stateless.
 *
 * Returns 0 if the peer closed the connection. Returns (ssize_t)-1 on
 * error; check errno for EAGAIN/EWOULDBLOCK (no data available yet)
 * versus a real error. For non-blocking sockets, the stateful variant
 * below handles partial assembly across calls. */
ssize_t isz_proto_recv_with_fds(int fd, void *out_buf, size_t buf_len,
                                int *out_fds, size_t *out_n_fds)
    ISZ_INTERNAL
    __attribute__((nonnull(2, 4, 5)));

/* Stateful variant: accumulates partial reads in *state across calls.
 * On a complete message, returns total bytes in out_buf (header +
 * payload), with *out_msg_id and *out_payload_len set. out_buf receives
 * the full wire frame [length|msg_id|payload]; the caller can derive
 * payload as (uint8_t *)out_buf + ISZ_MSG_HEADER_SIZE. out_fds (capacity
 * ISZ_PROTO_MAX_FDS) receives any fds carried with this message;
 * *out_n_fds is the count. Returns 0 if no complete message is yet
 * available (EAGAIN). Returns (ssize_t)-1 on hard error or EOF. */
ssize_t isz_proto_recv_state_read(int fd, struct isz_proto_recv_state *state,
                                  void *out_buf, size_t buf_len,
                                  uint32_t *out_msg_id,
                                  size_t *out_payload_len,
                                  int *out_fds, size_t *out_n_fds)
    ISZ_INTERNAL
    __attribute__((nonnull(2, 3, 5, 6, 7, 8)));

void isz_proto_recv_state_init(struct isz_proto_recv_state *state)
    ISZ_INTERNAL
    __attribute__((nonnull(1)));

/* Close any fds stashed in *state (e.g. on disconnect mid-message) and
 * reset to the initial state. */
void isz_proto_recv_state_discard(struct isz_proto_recv_state *state)
    ISZ_INTERNAL
    __attribute__((nonnull(1)));

/* ------------------------------------------------------------------ */
/* Connection magic + version                                          */
/* ------------------------------------------------------------------ */
/* All four helpers below assume the socket is in blocking mode (or at
 * least has enough send/recv buffer to complete a small fixed-size
 * exchange without EAGAIN). The handshake runs synchronously at
 * connection start, before the socket is moved into epoll and switched
 * to non-blocking for normal protocol traffic. After handshake_done,
 * the caller sets O_NONBLOCK and uses the stateful recv path. */
/* Server side of SPEC §6.2 step 3: send 8 bytes (BE magic, then LE
 * version). Returns 8 on success, (ssize_t)-1 on error (errno set). */
ssize_t isz_proto_send_magic(int fd, uint32_t version)
    ISZ_INTERNAL;

/* Counterpart: read 8 bytes (BE magic, LE version). Validates the
 * magic. Returns 8 on success (writes the version to *version_out),
 * (ssize_t)-2 on magic mismatch (caller treats as protocol violation
 * and disconnects), (ssize_t)-1 on error or EOF (errno set). */
ssize_t isz_proto_recv_magic(int fd, uint32_t *version_out)
    ISZ_INTERNAL
    __attribute__((nonnull(2)));

/* SPEC §6.2 step 4: the client replies with its chosen version (must
 * be ≤ the server's max). The reply is a bare 4-byte LE version, no
 * magic; the protocol identity is already established by the server's
 * magic header. These helpers send/recv that reply. Both return 4 on
 * success, (ssize_t)-1 on error or EOF. */
ssize_t isz_proto_send_version_reply(int fd, uint32_t version)
    ISZ_INTERNAL;

ssize_t isz_proto_recv_version_reply(int fd, uint32_t *version_out)
    ISZ_INTERNAL
    __attribute__((nonnull(2)));

/* ------------------------------------------------------------------ */
/* Payload read/write helpers (SPEC §6.1: little-endian on the wire)  */
/* ------------------------------------------------------------------ */
/* The dispatcher builds and parses message payloads with these. They
 * take a payload buffer plus an offset and return the new offset, so
 * callers can chain: off = isz_proto_write_u32(p, off, x); Reading past
 * the end of payload_len is the caller's responsibility; these helpers
 * do not bounds-check, they only do the byte-order conversion. */

size_t isz_proto_write_u32(void *buf, size_t off, uint32_t v)
    ISZ_INTERNAL;
size_t isz_proto_write_i32(void *buf, size_t off, int32_t v)
    ISZ_INTERNAL;
size_t isz_proto_write_u64(void *buf, size_t off, uint64_t v)
    ISZ_INTERNAL;
size_t isz_proto_write_u8(void *buf, size_t off, uint8_t v)
    ISZ_INTERNAL;

uint32_t isz_proto_read_u32(const void *buf, size_t off)
    ISZ_INTERNAL;
int32_t  isz_proto_read_i32(const void *buf, size_t off)
    ISZ_INTERNAL;
uint64_t isz_proto_read_u64(const void *buf, size_t off)
    ISZ_INTERNAL;
uint8_t  isz_proto_read_u8(const void *buf, size_t off)
    ISZ_INTERNAL;

/* Bounds-checked helpers: if off + len would exceed payload_len, return
 * false (parse failure). Otherwise read the value through *out and
 * return the new offset in *off. */
bool isz_proto_read_u32_checked(const void *buf, size_t *off,
                                size_t payload_len, uint32_t *out)
    ISZ_INTERNAL
    __attribute__((nonnull(1, 2, 4)));
bool isz_proto_read_i32_checked(const void *buf, size_t *off,
                                size_t payload_len, int32_t *out)
    ISZ_INTERNAL
    __attribute__((nonnull(1, 2, 4)));
bool isz_proto_read_u64_checked(const void *buf, size_t *off,
                                size_t payload_len, uint64_t *out)
    ISZ_INTERNAL
    __attribute__((nonnull(1, 2, 4)));

#endif /* ISHIZUE_PROTOCOL_H */
