/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Rui-727
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* test_client_protocol.c: W5-A integration test.
 *
 * Drives the full server-side dispatch path against a real Unix domain
 * socket. The parent forks a child as the client; the parent loops
 * isz_dispatch to drive accept + handshake + message routing on the
 * server side. The child speaks the raw wire protocol by hand.
 *
 * Sequence:
 *   1. Parent: isz_init(headless), allowlist /proc/self/exe, create UDS,
 *      isz_listen, fork.
 *   2. Child: connect, run client side of the §6.2 handshake (recv magic
 *      + version reply, recv capabilities, recv handshake_done), then:
 *        a. send SURFACE_CREATE; verify the reply carries a non-zero
 *           surface_id.
 *        b. send SURFACE_SET_POSITION for that surface; verify no reply
 *           arrives within a short window (success path is silent).
 *        c. send COMMIT for an output that has no surface with a plane
 *           slot; verify the ISZ_MSG_ERROR reply carries
 *           ISZ_ERR_SURFACE_NO_PLANE_SLOT.
 *   3. Parent: isz_dispatch in a loop until the child exits, then
 *      isz_destroy.
 *
 * The child uses a small wire-protocol encoder so we do not pull the
 * internal isz_protocol.h into the test binary. The framing matches
 * SPEC §6.1: [u32 LE length][u32 LE msg_id][payload]. */

#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <ishizue/isz.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Protocol constants mirrored from src/protocol/isz_protocol.h. Inlined
 * here so the test stays off internal headers. */
#define ISZ_TEST_PROTOCOL_VERSION 1u
#define ISZ_TEST_MSG_HANDSHAKE_DONE 1u
#define ISZ_TEST_MSG_GLOBAL         2u
#define ISZ_TEST_MSG_CAPABILITIES   3u
#define ISZ_TEST_MSG_SURFACE_CREATE            4u
#define ISZ_TEST_MSG_SURFACE_DESTROY           5u
#define ISZ_TEST_MSG_SURFACE_SET_POSITION      8u
#define ISZ_TEST_MSG_SURFACE_SET_PLANE_TYPE   10u
#define ISZ_TEST_MSG_SURFACE_CREATE_LAYER     16u
#define ISZ_TEST_MSG_COMMIT                   21u
#define ISZ_TEST_MSG_ERROR                    50u

#define ISZ_TEST_ERR_SURFACE_NO_PLANE_SLOT (-4)

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
            goto fail; \
        } \
    } while (0)

/* Little-endian encoders. */
static void test_put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t test_get_u32_le(const uint8_t *p) {
    return  ((uint32_t)p[0])        |
           ((uint32_t)p[1] << 8)   |
           ((uint32_t)p[2] << 16)  |
           ((uint32_t)p[3] << 24);
}

static int32_t test_get_i32_le(const uint8_t *p) {
    return (int32_t)test_get_u32_le(p);
}

/* Send one framed message over the socket. Returns 0 on success, -1 on
 * hard error. */
static int test_send_msg(int fd, uint32_t msg_id,
                         const void *payload, size_t payload_len)
{
    uint8_t hdr[8];
    uint32_t length = (uint32_t)(4u + payload_len);
    test_put_u32_le(hdr, length);
    test_put_u32_le(hdr + 4, msg_id);

    if (send(fd, hdr, 8, 0) != 8)
        return -1;
    if (payload_len > 0) {
        if (send(fd, payload, payload_len, 0) != (ssize_t)payload_len)
            return -1;
    }
    return 0;
}

/* Receive one framed message with a timeout. Returns:
 *   >0  message bytes consumed (header + payload)
 *    0  timeout with no data
 *   -1  hard error or peer closed
 * On success, msg_id and payload_len are filled in; payload bytes are
 * left in buf (capacity must be >= 8 + payload_len). */
static ssize_t test_recv_msg(int fd, uint32_t *msg_id,
                             uint8_t *buf, size_t buf_cap,
                             int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    if (!(pfd.revents & POLLIN)) return -1;

    /* Read the 8-byte header first. */
    ssize_t n = recv(fd, buf, 8, MSG_WAITALL);
    if (n <= 0) return -1;
    if (n != 8) return -1;
    uint32_t length = test_get_u32_le(buf);
    if (length < 4 || length > 8192 - 4) return -1;
    size_t payload_len = (size_t)length - 4u;
    if (8u + payload_len > buf_cap) return -1;
    *msg_id = test_get_u32_le(buf + 4);

    if (payload_len > 0) {
        n = recv(fd, buf + 8, payload_len, MSG_WAITALL);
        if (n != (ssize_t)payload_len) return -1;
    }
    return (ssize_t)(8u + payload_len);
}

/* Run the client side of the §6.2 handshake. Returns 0 on success. */
static int test_client_handshake(int fd)
{
    /* Server sends 8 bytes: BE magic, LE version. */
    uint8_t magic[8];
    if (recv(fd, magic, 8, MSG_WAITALL) != 8) return -1;
    /* magic[0..3] should be 'I','S','Z','H'. */
    if (magic[0] != 'I' || magic[1] != 'S' ||
        magic[2] != 'Z' || magic[3] != 'H') {
        fprintf(stderr, "client: bad magic %c%c%c%c\n",
                magic[0], magic[1], magic[2], magic[3]);
        return -1;
    }
    uint32_t server_version = test_get_u32_le(magic + 4);
    if (server_version == 0) return -1;

    /* Reply with our chosen version. */
    uint8_t reply[4];
    test_put_u32_le(reply, ISZ_TEST_PROTOCOL_VERSION);
    if (send(fd, reply, 4, 0) != 4) return -1;

    /* Drain server-to-client handshake events until we see
     * HANDSHAKE_DONE. The server may send GLOBAL, CAPABILITIES, then
     * HANDSHAKE_DONE. Tolerate ordering either way; bail after 16
     * messages so a server bug doesn't loop forever. */
    for (int i = 0; i < 16; i++) {
        uint32_t msg_id = 0;
        uint8_t buf[256];
        ssize_t n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 5000);
        if (n <= 0) {
            fprintf(stderr, "client: handshake recv rc=%zd\n", n);
            return -1;
        }
        if (msg_id == ISZ_TEST_MSG_HANDSHAKE_DONE)
            return 0;
        /* GLOBAL/CAPABILITIES: continue. */
    }
    fprintf(stderr, "client: handshake_done never arrived\n");
    return -1;
}

/* Child process: drive the wire protocol end-to-end. Returns 0 on
 * success, non-zero on failure. */
static int test_client_main(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("client socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* memcpy is safer than strncpy here: sock_path is already bounded
     * to fit sun_path (see main), and we want exact bytes without
     * -Wstringop-truncation noise. */
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);

    /* Retry connect briefly so the parent's isz_listen has time to
     * install the fd into its epoll set. */
    int rc = -1;
    for (int i = 0; i < 100 && rc < 0; i++) {
        rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (rc < 0) {
            if (errno == ECONNREFUSED || errno == ENOENT)
                usleep(10000);
            else {
                perror("client connect");
                close(fd);
                return 1;
            }
        }
    }
    if (rc < 0) {
        perror("client connect (final)");
        close(fd);
        return 1;
    }

    if (test_client_handshake(fd) < 0) {
        fprintf(stderr, "client: handshake failed\n");
        close(fd);
        return 1;
    }

    /* a. SURFACE_CREATE: no payload. */
    if (test_send_msg(fd, ISZ_TEST_MSG_SURFACE_CREATE, NULL, 0) < 0) {
        fprintf(stderr, "client: send surface_create failed\n");
        close(fd);
        return 1;
    }
    uint32_t msg_id = 0;
    uint8_t buf[256];
    ssize_t n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 5000);
    if (n <= 0) {
        fprintf(stderr, "client: surface_create reply rc=%zd\n", n);
        close(fd);
        return 1;
    }
    if (msg_id != ISZ_TEST_MSG_SURFACE_CREATE) {
        fprintf(stderr, "client: surface_create reply msg_id=%u\n", msg_id);
        close(fd);
        return 1;
    }
    /* Payload is u32 surface_id. */
    if (n < 12) {
        fprintf(stderr, "client: surface_create reply too short\n");
        close(fd);
        return 1;
    }
    uint32_t surface_id = test_get_u32_le(buf + 8);
    if (surface_id == 0) {
        fprintf(stderr, "client: surface_create returned id 0\n");
        close(fd);
        return 1;
    }

    /* b. SURFACE_SET_POSITION: u32 surface_id, i32 x, i32 y. */
    uint8_t setpos[12];
    test_put_u32_le(setpos, surface_id);
    test_put_u32_le(setpos + 4, (uint32_t)100);
    test_put_u32_le(setpos + 8, (uint32_t)200);
    if (test_send_msg(fd, ISZ_TEST_MSG_SURFACE_SET_POSITION,
                      setpos, sizeof(setpos)) < 0) {
        fprintf(stderr, "client: send set_position failed\n");
        close(fd);
        return 1;
    }

    /* Verify no error reply within a short window. The success path is
     * silent (no reply). 200 ms is enough for the parent's isz_dispatch
     * loop to tick and route the message. */
    n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 200);
    if (n > 0) {
        /* Any reply here is a failure; the setter should not produce
         * one on success. */
        fprintf(stderr,
                "client: unexpected reply after set_position (msg_id=%u)\n",
                msg_id);
        close(fd);
        return 1;
    }

    /* c. COMMIT for a surface that has no plane slot assigned.
     *
     * The surface created in (a) is not bound to any output, so a
     * COMMIT on the headless output would see no surfaces and succeed.
     * To exercise the no-plane-slot path we need a surface that IS
     * bound to the output but lacks plane_slot. SURFACE_CREATE_LAYER
     * binds the surface to an output automatically; we then set its
     * plane_type (mandatory before validate_surface even checks the
     * slot) and leave plane_slot unset. The next COMMIT must fail with
     * ISZ_ERR_SURFACE_NO_PLANE_SLOT. */

    /* c.1 SURFACE_CREATE_LAYER: u32 output_id, i32 layer. */
    uint8_t clayer[8];
    test_put_u32_le(clayer, 0u);           /* output_id 0 → first output */
    test_put_u32_le(clayer + 4, 0u);       /* ISZ_LAYER_OVERLAY = 0 */
    if (test_send_msg(fd, ISZ_TEST_MSG_SURFACE_CREATE_LAYER,
                      clayer, sizeof(clayer)) < 0) {
        fprintf(stderr, "client: send create_layer failed\n");
        close(fd);
        return 1;
    }
    n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 5000);
    if (n <= 0) {
        fprintf(stderr, "client: create_layer reply rc=%zd\n", n);
        close(fd);
        return 1;
    }
    if (msg_id != ISZ_TEST_MSG_SURFACE_CREATE_LAYER) {
        fprintf(stderr, "client: create_layer reply msg_id=%u\n", msg_id);
        close(fd);
        return 1;
    }
    if (n < 16) {
        fprintf(stderr, "client: create_layer reply too short\n");
        close(fd);
        return 1;
    }
    uint32_t layer_id = test_get_u32_le(buf + 12);
    if (layer_id == 0) {
        fprintf(stderr, "client: create_layer returned id 0\n");
        close(fd);
        return 1;
    }

    /* c.2 SURFACE_SET_PLANE_TYPE: u32 surface_id, i32 type (overlay=1). */
    uint8_t spt[8];
    test_put_u32_le(spt, layer_id);
    test_put_u32_le(spt + 4, 1u);  /* ISZ_PLANE_OVERLAY */
    if (test_send_msg(fd, ISZ_TEST_MSG_SURFACE_SET_PLANE_TYPE,
                      spt, sizeof(spt)) < 0) {
        fprintf(stderr, "client: send set_plane_type failed\n");
        close(fd);
        return 1;
    }
    /* Setter is silent on success. */
    n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 200);
    if (n > 0) {
        fprintf(stderr,
                "client: unexpected reply after set_plane_type (msg_id=%u)\n",
                msg_id);
        close(fd);
        return 1;
    }

    /* c.3 COMMIT: u32 output_id (0 → fallback), u32 flags (0). */
    uint8_t commit[8];
    test_put_u32_le(commit, 0u);
    test_put_u32_le(commit + 4, 0u);
    if (test_send_msg(fd, ISZ_TEST_MSG_COMMIT, commit, sizeof(commit)) < 0) {
        fprintf(stderr, "client: send commit failed\n");
        close(fd);
        return 1;
    }
    n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 5000);
    if (n <= 0) {
        fprintf(stderr, "client: commit reply rc=%zd\n", n);
        close(fd);
        return 1;
    }
    if (msg_id != ISZ_TEST_MSG_ERROR) {
        fprintf(stderr, "client: expected ERROR, got msg_id=%u\n", msg_id);
        close(fd);
        return 1;
    }
    if (n < 16) {
        fprintf(stderr, "client: error reply too short\n");
        close(fd);
        return 1;
    }
    uint32_t orig_msg_id = test_get_u32_le(buf + 8);
    int32_t err_code = test_get_i32_le(buf + 12);
    if (orig_msg_id != ISZ_TEST_MSG_COMMIT) {
        fprintf(stderr,
                "client: error orig_msg_id=%u (expected COMMIT=%u)\n",
                orig_msg_id, ISZ_TEST_MSG_COMMIT);
        close(fd);
        return 1;
    }
    if (err_code != ISZ_TEST_ERR_SURFACE_NO_PLANE_SLOT) {
        fprintf(stderr,
                "client: error code=%d (expected %d)\n",
                err_code, ISZ_TEST_ERR_SURFACE_NO_PLANE_SLOT);
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

int main(void)
{
    int              ret     = 1;
    isz_server      *srv     = NULL;
    int              listen_fd = -1;
    pid_t            child   = -1;
    char            *cwd     = NULL;
    char             sock_path[96];

    /* Ignore SIGPIPE so a peer-closed send returns EPIPE/EAGAIN instead
     * of killing the test. */
    signal(SIGPIPE, SIG_IGN);

    /* 1. Headless server with a known geometry. */
    isz_headless_config cfg = {
        .width = 1024, .height = 768, .refresh_rate = 60000,
    };
    srv = isz_init(ISZ_BACKEND_HEADLESS, &cfg);
    CHECK(srv != NULL, "isz_init returned NULL");

    /* 2. Allowlist our own binary so the child's connection passes the
     *    §6.3 SO_PEERCRED check. /proc/self/exe resolves to this test
     *    binary's inode; the child inherits the same exe. */
    {
        int rc = isz_allowlist_add_binary(srv, "/proc/self/exe");
        CHECK(rc == ISZ_OK, "isz_allowlist_add_binary rc=%d", rc);
    }

    /* 3. Create a UDS at a unique path under /tmp. sun_path is 108 bytes
     * max; keep sock_path well under that to silence -Wformat-truncation. */
    cwd = getcwd(NULL, 0);
    if (!cwd) cwd = strdup("/tmp");
    snprintf(sock_path, sizeof(sock_path), "%s/.ishizue-test-%d",
             cwd, (int)getpid());
    free(cwd);
    (void)unlink(sock_path);

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listen_fd >= 0, "socket: %s", strerror(errno));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);
    CHECK(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0,
          "bind: %s", strerror(errno));
    CHECK(listen(listen_fd, 4) == 0,
          "listen: %s", strerror(errno));

    /* 4. Hand the listen fd to the library. */
    {
        int rc = isz_listen(srv, listen_fd);
        CHECK(rc == ISZ_OK, "isz_listen rc=%d", rc);
    }

    /* 5. Fork the client. */
    child = fork();
    CHECK(child >= 0, "fork: %s", strerror(errno));
    if (child == 0) {
        /* Child. Close the listen fd; the parent owns it. */
        close(listen_fd);
        int rc = test_client_main(sock_path);
        _exit(rc);
    }

    /* 6. Parent: drive isz_dispatch until the child exits. The child
     *    does the handshake + three round trips; a few seconds is
     *    plenty. */
    for (;;) {
        int status = 0;
        pid_t w = waitpid(child, &status, WNOHANG);
        if (w == child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                ret = 0;
            } else {
                fprintf(stderr, "child exited status=%d\n", status);
            }
            break;
        }
        if (w < 0 && errno != EINTR) {
            fprintf(stderr, "waitpid: %s\n", strerror(errno));
            break;
        }
        /* Tick the server's epoll loop. */
        isz_dispatch(srv);
        usleep(1000);
    }

    if (ret == 0)
        printf("test_client_protocol: PASS\n");

    /* Cleanup. */
    isz_destroy(srv);
    (void)unlink(sock_path);
    return ret;

fail:
    if (srv) isz_destroy(srv);
    if (listen_fd >= 0) close(listen_fd);
    (void)unlink(sock_path);
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
    }
    return 1;
}
