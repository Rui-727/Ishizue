/* SPDX-License-Identifier: MIT */
/*
 * isz_protocol.c - framing, SCM_RIGHTS, magic/version for the Ishizue
 * client wire protocol (SPEC §6.1, §6.2).
 */
#define _DEFAULT_SOURCE 1
#include "isz_protocol.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Byte-order helpers (no host-endian assumption; we always go through
 * these so the wire format is LE/BE exactly as SPEC §6.1 mandates). */
/* ------------------------------------------------------------------ */
static inline void isz_put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static inline uint32_t isz_get_u32_le(const uint8_t *p) {
    return  ((uint32_t)p[0])        |
           ((uint32_t)p[1] << 8)   |
           ((uint32_t)p[2] << 16)  |
           ((uint32_t)p[3] << 24);
}

static inline void isz_put_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static inline uint32_t isz_get_u32_be(const uint8_t *p) {
    return  ((uint32_t)p[0] << 24)  |
           ((uint32_t)p[1] << 16)  |
           ((uint32_t)p[2] << 8)   |
           ((uint32_t)p[3]);
}

/* ------------------------------------------------------------------ */
/* Encode / decode                                                     */
/* ------------------------------------------------------------------ */
size_t isz_proto_encode(void *out_buf, size_t out_buf_len,
                        uint32_t msg_id,
                        const void *payload, size_t payload_len) {
    /* payload_len == 0 with payload != NULL is allowed (payload ignored);
     * payload_len > 0 with payload == NULL is not. */
    if (payload_len > 0 && payload == NULL) {
        errno = EINVAL;
        return 0;
    }
    if (payload_len > ISZ_PROTO_MAX_MESSAGE - ISZ_MSG_HEADER_SIZE) {
        errno = EMSGSIZE;
        return 0;
    }

    size_t total = ISZ_MSG_HEADER_SIZE + payload_len;
    if (out_buf_len < total) {
        errno = EMSGSIZE;
        return 0;
    }

    /* length = sizeof(msg_id) + payload_len = 4 + payload_len.
     * See header doc for the framing convention. */
    uint32_t length = (uint32_t)(sizeof(uint32_t) + payload_len);

    uint8_t *out = (uint8_t *)out_buf;
    isz_put_u32_le(out,     length);
    isz_put_u32_le(out + 4, msg_id);
    if (payload_len > 0) {
        memcpy(out + ISZ_MSG_HEADER_SIZE, payload, payload_len);
    }
    return total;
}

size_t isz_proto_decode(const void *in_buf, size_t len,
                        uint32_t *msg_id,
                        const void **payload,
                        size_t *payload_len) {
    if (len < sizeof(uint32_t)) {
        return 0;  /* need the length field */
    }

    const uint8_t *in = (const uint8_t *)in_buf;
    uint32_t length = isz_get_u32_le(in);

    /* length counts msg_id (4 bytes) + payload. It must be >= 4. */
    if (length < sizeof(uint32_t)) {
        errno = EBADMSG;
        return (size_t)-1;
    }
    if (length > ISZ_PROTO_MAX_MESSAGE - sizeof(uint32_t)) {
        errno = EMSGSIZE;
        return (size_t)-1;
    }

    size_t total = sizeof(uint32_t) + (size_t)length;
    if (len < total) {
        return 0;  /* incomplete */
    }

    *msg_id       = isz_get_u32_le(in + 4);
    *payload      = in + ISZ_MSG_HEADER_SIZE;
    *payload_len  = (size_t)length - sizeof(uint32_t);
    return total;
}

/* ------------------------------------------------------------------ */
/* Socket I/O with SCM_RIGHTS                                          */
/* ------------------------------------------------------------------ */
ssize_t isz_proto_send_frame(int fd, const void *frame, size_t frame_len,
                             const int *fds, size_t n_fds) {
    if (frame_len < ISZ_MSG_HEADER_SIZE ||
        frame_len > ISZ_PROTO_MAX_MESSAGE) {
        errno = EMSGSIZE;
        return -1;
    }
    if (n_fds > ISZ_PROTO_MAX_FDS) {
        errno = EMSGSIZE;
        return -1;
    }
    if (n_fds > 0 && fds == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct iovec iov;
    iov.iov_base = (void *)frame;  /* cast away const; sendmsg is read-only */
    iov.iov_len  = frame_len;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    union {
        struct cmsghdr hdr;
        uint8_t        buf[CMSG_SPACE(ISZ_PROTO_MAX_FDS * (int)sizeof(int))];
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    if (n_fds > 0) {
        msg.msg_control    = cmsg_buf.buf;
        msg.msg_controllen = CMSG_SPACE((size_t)n_fds * sizeof(int));

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_len   = CMSG_LEN((size_t)n_fds * sizeof(int));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        memcpy(CMSG_DATA(cmsg), fds, n_fds * sizeof(int));
    }

    ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
    if ((size_t)n != frame_len) {
        /* Partial write. For UDS small messages this shouldn't happen;
         * if it does, the cmsg fds may already be in flight. We cannot
         * safely resume; caller must disconnect. */
        errno = EIO;
        return -1;
    }
    return (ssize_t)frame_len;
}

ssize_t isz_proto_send_with_fds(int fd, uint32_t msg_id,
                                const void *payload, size_t payload_len,
                                const int *fds, size_t n_fds) {
    if (payload_len > 0 && payload == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (payload_len > ISZ_PROTO_MAX_MESSAGE - ISZ_MSG_HEADER_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    uint8_t buf[ISZ_PROTO_MAX_MESSAGE];
    size_t total = isz_proto_encode(buf, sizeof(buf), msg_id,
                                    payload, payload_len);
    if (total == 0) {
        return -1;  /* errno set by isz_proto_encode */
    }
    return isz_proto_send_frame(fd, buf, total, fds, n_fds);
}

/* Pull fds out of a recvmsg's cmsg. Writes up to `cap` fds into `out`.
 * On overflow (more fds than `cap`), closes every fd received in this
 * call (including ones already written to `out`) and returns -1, so
 * the caller doesn't have to track partial fds on the error path. */
static ssize_t isz_extract_fds(struct msghdr *msg, int *out, size_t cap) {
    size_t count   = 0;
    bool   overflow = false;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type  != SCM_RIGHTS) {
            continue;
        }
        size_t data_len = (size_t)cmsg->cmsg_len - (size_t)CMSG_LEN(0);
        size_t nf = data_len / sizeof(int);
        const uint8_t *p = (const uint8_t *)CMSG_DATA(cmsg);
        for (size_t i = 0; i < nf; i++) {
            int got;
            memcpy(&got, p + i * sizeof(int), sizeof(int));
            if (count < cap) {
                out[count++] = got;
            } else {
                close(got);
                overflow = true;
            }
        }
    }
    if (overflow) {
        for (size_t i = 0; i < count; i++) {
            close(out[i]);
        }
        errno = EMSGSIZE;
        return -1;
    }
    return (ssize_t)count;
}

ssize_t isz_proto_recv_with_fds(int fd, void *out_buf, size_t buf_len,
                                int *out_fds, size_t *out_n_fds) {
    if (buf_len < ISZ_PROTO_MAX_MESSAGE) {
        errno = EMSGSIZE;
        return -1;
    }

    struct iovec iov;
    iov.iov_base = out_buf;
    iov.iov_len  = buf_len;

    union {
        struct cmsghdr hdr;
        uint8_t        buf[CMSG_SPACE(ISZ_PROTO_MAX_FDS * (int)sizeof(int))];
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);

    ssize_t n = recvmsg(fd, &msg, 0);
    if (n < 0) {
        return -1;  /* caller checks errno */
    }
    if (n == 0) {
        *out_n_fds = 0;
        return 0;  /* peer closed */
    }

    if (msg.msg_flags & MSG_CTRUNC) {
        /* cmsg buffer too small; we may have lost fds. Caller treats as
         * fatal. */
        errno = EMSGSIZE;
        return -1;
    }

    ssize_t fdc = isz_extract_fds(&msg, out_fds, ISZ_PROTO_MAX_FDS);
    if (fdc < 0) {
        *out_n_fds = 0;
        return -1;
    }
    *out_n_fds = (size_t)fdc;
    return n;
}

/* ------------------------------------------------------------------ */
/* Stateful recv                                                       */
/* ------------------------------------------------------------------ */
void isz_proto_recv_state_init(struct isz_proto_recv_state *state) {
    memset(state, 0, sizeof(*state));
}

void isz_proto_recv_state_discard(struct isz_proto_recv_state *state) {
    for (size_t i = 0; i < state->n_pending_fds; i++) {
        close(state->pending_fds[i]);
    }
    memset(state, 0, sizeof(*state));
}

ssize_t isz_proto_recv_state_read(int fd, struct isz_proto_recv_state *state,
                                  void *out_buf, size_t buf_len,
                                  uint32_t *out_msg_id,
                                  size_t *out_payload_len,
                                  int *out_fds, size_t *out_n_fds) {
    if (buf_len < ISZ_PROTO_MAX_MESSAGE) {
        errno = EMSGSIZE;
        return -1;
    }

    for (;;) {
        /* Try to parse a complete message from what we already have. */
        if (state->have >= sizeof(uint32_t)) {
            uint32_t length = isz_get_u32_le(state->buf);
            if (length < sizeof(uint32_t)) {
                errno = EBADMSG;
                return -1;
            }
            if (length > ISZ_PROTO_MAX_MESSAGE - sizeof(uint32_t)) {
                errno = EMSGSIZE;
                return -1;
            }
            size_t total = sizeof(uint32_t) + (size_t)length;
            if (state->have >= total) {
                /* Complete message. Hand it to the caller. */
                uint32_t msg_id = isz_get_u32_le(state->buf + 4);
                memcpy(out_buf, state->buf, total);
                for (size_t i = 0; i < state->n_pending_fds; i++) {
                    out_fds[i] = state->pending_fds[i];
                }
                *out_n_fds       = state->n_pending_fds;
                *out_msg_id      = msg_id;
                *out_payload_len = (size_t)length - sizeof(uint32_t);

                /* Shift the remainder down. */
                size_t rest = state->have - total;
                if (rest > 0) {
                    memmove(state->buf, state->buf + total, rest);
                }
                state->have           = rest;
                state->n_pending_fds  = 0;
                state->in_progress    = (rest > 0);
                return (ssize_t)total;
            }
        }

        /* Need more bytes from the socket. */
        size_t cap = ISZ_PROTO_MAX_MESSAGE - state->have;
        if (cap == 0) {
            /* Buffer full but no complete message: message exceeds the
             * protocol max. Fatal. */
            errno = EMSGSIZE;
            return -1;
        }

        struct iovec iov;
        iov.iov_base = state->buf + state->have;
        iov.iov_len  = cap;

        union {
            struct cmsghdr hdr;
            uint8_t        buf[CMSG_SPACE(ISZ_PROTO_MAX_FDS * (int)sizeof(int))];
        } cmsg_buf;
        memset(&cmsg_buf, 0, sizeof(cmsg_buf));

        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = cmsg_buf.buf;
        msg.msg_controllen = sizeof(cmsg_buf.buf);

        ssize_t n = recvmsg(fd, &msg, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;  /* EAGAIN falls through here; caller checks errno */
        }
        if (n == 0) {
            /* Peer closed. Pending state, if any, is abandoned. */
            return -1;
        }

        state->have        += (size_t)n;
        state->in_progress  = true;

        if (msg.msg_flags & MSG_CTRUNC) {
            errno = EMSGSIZE;
            return -1;
        }

        /* Stash any fds delivered with this recvmsg into pending_fds.
         * Per Linux UDS semantics, the cmsg is delivered with the
         * recvmsg returning the first byte of the originating sendmsg.
         * We assume the kernel does not interleave cmsgs across
         * sendmsgs in a way that breaks framing. */
        size_t fd_cap = ISZ_PROTO_MAX_FDS - state->n_pending_fds;
        ssize_t new_count = isz_extract_fds(
            &msg, state->pending_fds + state->n_pending_fds, fd_cap);
        if (new_count < 0) {
            /* Overflow: isz_extract_fds already closed the affected fds.
             * Reset pending_fds so discard() doesn't double-close. */
            state->n_pending_fds = 0;
            errno = EMSGSIZE;
            return -1;
        }
        state->n_pending_fds += (size_t)new_count;

        /* Loop back and try to parse. */
    }
}

/* ------------------------------------------------------------------ */
/* Connection magic + version                                          */
/* ------------------------------------------------------------------ */
/* All four helpers below use blocking send/recv with EINTR retry. The
 * caller is responsible for ensuring the socket is in blocking mode
 * during the handshake (see header doc). */
static ssize_t isz_send_all(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        off += (size_t)n;
    }
    return (ssize_t)len;
}

static ssize_t isz_recv_all(int fd, uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            /* Peer closed before sending the expected bytes. */
            return -1;
        }
        off += (size_t)n;
    }
    return (ssize_t)len;
}

ssize_t isz_proto_send_magic(int fd, uint32_t version) {
    uint8_t buf[8];
    isz_put_u32_be(buf,     ISZ_PROTOCOL_MAGIC);
    isz_put_u32_le(buf + 4, version);
    return isz_send_all(fd, buf, sizeof(buf));
}

ssize_t isz_proto_recv_magic(int fd, uint32_t *version_out) {
    uint8_t buf[8];
    ssize_t r = isz_recv_all(fd, buf, sizeof(buf));
    if (r < 0) {
        return -1;
    }
    uint32_t magic = isz_get_u32_be(buf);
    if (magic != ISZ_PROTOCOL_MAGIC) {
        return -2;
    }
    *version_out = isz_get_u32_le(buf + 4);
    return (ssize_t)sizeof(buf);
}

ssize_t isz_proto_send_version_reply(int fd, uint32_t version) {
    uint8_t buf[4];
    isz_put_u32_le(buf, version);
    return isz_send_all(fd, buf, sizeof(buf));
}

ssize_t isz_proto_recv_version_reply(int fd, uint32_t *version_out) {
    uint8_t buf[4];
    ssize_t r = isz_recv_all(fd, buf, sizeof(buf));
    if (r < 0) {
        return -1;
    }
    *version_out = isz_get_u32_le(buf);
    return (ssize_t)sizeof(buf);
}

/* ------------------------------------------------------------------ */
/* Payload read/write helpers                                          */
/* ------------------------------------------------------------------ */
size_t isz_proto_write_u32(void *buf, size_t off, uint32_t v) {
    isz_put_u32_le((uint8_t *)buf + off, v);
    return off + 4u;
}

size_t isz_proto_write_i32(void *buf, size_t off, int32_t v) {
    isz_put_u32_le((uint8_t *)buf + off, (uint32_t)v);
    return off + 4u;
}

size_t isz_proto_write_u64(void *buf, size_t off, uint64_t v) {
    uint8_t *p = (uint8_t *)buf + off;
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
    return off + 8u;
}

size_t isz_proto_write_u8(void *buf, size_t off, uint8_t v) {
    ((uint8_t *)buf)[off] = v;
    return off + 1u;
}

uint32_t isz_proto_read_u32(const void *buf, size_t off) {
    return isz_get_u32_le((const uint8_t *)buf + off);
}

int32_t isz_proto_read_i32(const void *buf, size_t off) {
    return (int32_t)isz_get_u32_le((const uint8_t *)buf + off);
}

uint64_t isz_proto_read_u64(const void *buf, size_t off) {
    const uint8_t *p = (const uint8_t *)buf + off;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

uint8_t isz_proto_read_u8(const void *buf, size_t off) {
    return ((const uint8_t *)buf)[off];
}

bool isz_proto_read_u32_checked(const void *buf, size_t *off,
                                size_t payload_len, uint32_t *out) {
    if (*off > payload_len || payload_len - *off < 4u)
        return false;
    *out = isz_proto_read_u32(buf, *off);
    *off += 4u;
    return true;
}

bool isz_proto_read_i32_checked(const void *buf, size_t *off,
                                size_t payload_len, int32_t *out) {
    if (*off > payload_len || payload_len - *off < 4u)
        return false;
    *out = isz_proto_read_i32(buf, *off);
    *off += 4u;
    return true;
}

bool isz_proto_read_u64_checked(const void *buf, size_t *off,
                                size_t payload_len, uint64_t *out) {
    if (*off > payload_len || payload_len - *off < 8u)
        return false;
    *out = isz_proto_read_u64(buf, *off);
    *off += 8u;
    return true;
}
