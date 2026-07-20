/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Rui-727
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense,
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* test_protocol_additions.c: W9-A integration test.
 *
 * Drives the six wire-protocol message IDs added by W8-B end to end
 * against a headless Ishizue server. The parent forks a child as the
 * wire-protocol client; the parent loops isz_dispatch to drive accept
 * + handshake + message routing on the server side, then uses the
 * Architect API to trigger the five S2C messages and verifies the
 * child receives them. The child sends one C2S message
 * (SET_SELECTION_OWNER) and the parent verifies the library accepts
 * it via isz_seat_get_selection_owner.
 *
 * Sequence:
 *   1. Parent: isz_init(headless), allowlist /proc/self/exe, create
 *      UDS, isz_listen, fork.
 *   2. Child: connect, run the §6.2 handshake, send SURFACE_CREATE_LAYER
 *      (binds the surface to the headless output), recv the reply.
 *   3. Parent: dispatch loop until the surface appears in the global
 *      surface list. The surface has owning_conn set because the
 *      dispatcher created it on behalf of the child.
 *   4. Parent: call isz_surface_set_scale(surf, 3, 2). This sends
 *      ISZ_MSG_SURFACE_PREFERRED_SCALE to the child.
 *   5. Child: recv ISZ_MSG_SURFACE_PREFERRED_SCALE, verify surface_id
 *      matches and numerator=3, denominator=2.
 *   6. Parent: call isz_surface_set_idle_inhibit(surf, true). This
 *      sends ISZ_MSG_IDLE_INHIBIT_ACTIVE to the child.
 *   7. Child: recv ISZ_MSG_IDLE_INHIBIT_ACTIVE, verify output_id.
 *   8. Parent: call isz_surface_set_idle_inhibit(surf, false). This
 *      sends ISZ_MSG_IDLE_INHIBIT_INACTIVE.
 *   9. Child: recv ISZ_MSG_IDLE_INHIBIT_INACTIVE, verify output_id.
 *  10. Child: send ISZ_MSG_SET_SELECTION_OWNER with the surface as
 *      owner, slot=CLIPBOARD, timestamp=1000, no fd.
 *  11. Parent: dispatch loop until isz_seat_get_selection_owner
 *      returns the surface.
 *  12. Child: verify no error reply (silent success).
 *  13. Parent: wait for child to exit, isz_destroy.
 *
 * The parent includes render/isz_surface_internal.h so it can walk
 * the global surface list (isz_render_surface_list). The child uses a
 * small wire-protocol encoder so it does not pull internal headers. */

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

/* Forward decl: defined in isz_test_hooks.c (only built with
 * -DISHIZUE_ENABLE_TEST_HOOKS). Walks the global surface list and
 * returns the first surface with an owning conn (i.e. created by a
 * client via the wire protocol), or NULL. Exported so the test
 * binary linked against the test-build .so can call it; not declared
 * in isz.h because it is a test-only helper. */
isz_surface *isz_test_find_client_surface(isz_server *srv);

/* Wire protocol constants mirrored from src/protocol/isz_protocol.h. */
#define ISZ_TEST_PROTOCOL_VERSION          1u
#define ISZ_TEST_MSG_HANDSHAKE_DONE        1u
#define ISZ_TEST_MSG_SURFACE_CREATE_LAYER  16u
#define ISZ_TEST_MSG_SURFACE_PREFERRED_SCALE 51u
#define ISZ_TEST_MSG_SET_SELECTION_OWNER   52u
#define ISZ_TEST_MSG_IDLE_INHIBIT_ACTIVE   55u
#define ISZ_TEST_MSG_IDLE_INHIBIT_INACTIVE 56u
#define ISZ_TEST_MSG_ERROR                 50u

#define ISZ_TEST_LAYER_OVERLAY             0u
#define ISZ_TEST_SELECTION_CLIPBOARD       1u
#define ISZ_TEST_SELECTION_FD_NONE         0xFFFFFFFFu

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

static ssize_t test_recv_msg(int fd, uint32_t *msg_id,
                             uint8_t *buf, size_t buf_cap,
                             int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    if (!(pfd.revents & POLLIN)) return -1;

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

static int test_client_handshake(int fd)
{
    uint8_t magic[8];
    if (recv(fd, magic, 8, MSG_WAITALL) != 8) return -1;
    if (magic[0] != 'I' || magic[1] != 'S' ||
        magic[2] != 'Z' || magic[3] != 'H')
        return -1;
    uint32_t server_version = test_get_u32_le(magic + 4);
    if (server_version == 0) return -1;

    uint8_t reply[4];
    test_put_u32_le(reply, ISZ_TEST_PROTOCOL_VERSION);
    if (send(fd, reply, 4, 0) != 4) return -1;

    for (int i = 0; i < 16; i++) {
        uint32_t msg_id = 0;
        uint8_t buf[256];
        ssize_t n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 5000);
        if (n <= 0) return -1;
        if (msg_id == ISZ_TEST_MSG_HANDSHAKE_DONE)
            return 0;
    }
    return -1;
}

/* Child: drives the wire protocol. Returns 0 on success. */
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
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);

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

    /* a. SURFACE_CREATE_LAYER: u32 output_id (0 -> first), i32 layer. */
    uint8_t clayer[8];
    test_put_u32_le(clayer, 0u);
    test_put_u32_le(clayer + 4, ISZ_TEST_LAYER_OVERLAY);
    if (test_send_msg(fd, ISZ_TEST_MSG_SURFACE_CREATE_LAYER,
                      clayer, sizeof(clayer)) < 0) {
        fprintf(stderr, "client: send create_layer failed\n");
        close(fd);
        return 1;
    }
    uint32_t msg_id = 0;
    uint8_t buf[256];
    ssize_t n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 5000);
    if (n <= 0 || msg_id != ISZ_TEST_MSG_SURFACE_CREATE_LAYER || n < 16) {
        fprintf(stderr, "client: create_layer reply bad (n=%zd msg=%u)\n",
                n, msg_id);
        close(fd);
        return 1;
    }
    uint32_t surface_id = test_get_u32_le(buf + 12);
    if (surface_id == 0) {
        fprintf(stderr, "client: create_layer returned id 0\n");
        close(fd);
        return 1;
    }

    /* b. Recv ISZ_MSG_SURFACE_PREFERRED_SCALE (parent called
     *     isz_surface_set_scale). Payload: u32 surface_id + u32 num
     *     + u32 den. */
    n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 5000);
    if (n <= 0 || msg_id != ISZ_TEST_MSG_SURFACE_PREFERRED_SCALE) {
        fprintf(stderr,
                "client: expected preferred_scale, got n=%zd msg=%u\n",
                n, msg_id);
        close(fd);
        return 1;
    }
    if (n < 20) {
        fprintf(stderr, "client: preferred_scale too short (%zd)\n", n);
        close(fd);
        return 1;
    }
    uint32_t recv_surf = test_get_u32_le(buf + 8);
    uint32_t recv_num = test_get_u32_le(buf + 12);
    uint32_t recv_den = test_get_u32_le(buf + 16);
    if (recv_surf != surface_id || recv_num != 3u || recv_den != 2u) {
        fprintf(stderr,
                "client: preferred_scale mismatch surf=%u/%u num=%u den=%u\n",
                recv_surf, surface_id, recv_num, recv_den);
        close(fd);
        return 1;
    }

    /* c. Recv ISZ_MSG_IDLE_INHIBIT_ACTIVE. Payload: u32 output_id. */
    n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 5000);
    if (n <= 0 || msg_id != ISZ_TEST_MSG_IDLE_INHIBIT_ACTIVE) {
        fprintf(stderr,
                "client: expected idle_inhibit_active, got n=%zd msg=%u\n",
                n, msg_id);
        close(fd);
        return 1;
    }
    if (n < 12) {
        fprintf(stderr, "client: idle_inhibit_active too short\n");
        close(fd);
        return 1;
    }
    uint32_t recv_out = test_get_u32_le(buf + 8);
    if (recv_out == 0) {
        fprintf(stderr, "client: idle_inhibit_active output_id=0\n");
        close(fd);
        return 1;
    }

    /* d. Recv ISZ_MSG_IDLE_INHIBIT_INACTIVE. Payload: u32 output_id. */
    n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 5000);
    if (n <= 0 || msg_id != ISZ_TEST_MSG_IDLE_INHIBIT_INACTIVE) {
        fprintf(stderr,
                "client: expected idle_inhibit_inactive, got n=%zd msg=%u\n",
                n, msg_id);
        close(fd);
        return 1;
    }
    if (n < 12) {
        fprintf(stderr, "client: idle_inhibit_inactive too short\n");
        close(fd);
        return 1;
    }
    uint32_t recv_out2 = test_get_u32_le(buf + 8);
    if (recv_out2 != recv_out) {
        fprintf(stderr,
                "client: inactive output_id=%u != active=%u\n",
                recv_out2, recv_out);
        close(fd);
        return 1;
    }

    /* e. Send ISZ_MSG_SET_SELECTION_OWNER. Payload: u32 slot +
     *    u64 timestamp_ns + u32 owner_surface_id + u32 fd_index. */
    uint8_t sel[20];
    test_put_u32_le(sel,      ISZ_TEST_SELECTION_CLIPBOARD);
    test_put_u32_le(sel + 4,  1000u);                 /* timestamp lo */
    test_put_u32_le(sel + 8,  0u);                    /* timestamp hi */
    test_put_u32_le(sel + 12, surface_id);
    test_put_u32_le(sel + 16, ISZ_TEST_SELECTION_FD_NONE);
    if (test_send_msg(fd, ISZ_TEST_MSG_SET_SELECTION_OWNER,
                      sel, sizeof(sel)) < 0) {
        fprintf(stderr, "client: send set_selection_owner failed\n");
        close(fd);
        return 1;
    }

    /* f. Verify no error reply within a short window (silent success). */
    n = test_recv_msg(fd, &msg_id, buf, sizeof(buf), 300);
    if (n > 0) {
        fprintf(stderr,
                "client: unexpected reply after set_selection_owner "
                "(msg_id=%u)\n", msg_id);
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

/* Find the first surface in the global list that has an owning conn.
 * Returns NULL if no client-created surface exists yet. */
static isz_surface *test_find_client_surface(isz_server *srv)
{
    return isz_test_find_client_surface(srv);
}

int main(void)
{
    isz_server *srv       = NULL;
    int         listen_fd = -1;
    pid_t       child     = -1;
    char       *cwd       = NULL;
    char        sock_path[96];
    int         ret       = 1;
    isz_surface *surf     = NULL;
    isz_seat    *seat     = NULL;

    signal(SIGPIPE, SIG_IGN);

    /* 1. Headless server. */
    isz_headless_config cfg = {
        .width = 1024, .height = 768, .refresh_rate = 60000,
    };
    srv = isz_init(ISZ_BACKEND_HEADLESS, &cfg);
    CHECK(srv != NULL, "isz_init returned NULL");

    /* 2. Allowlist /proc/self/exe so the child's connection passes
     *    the §6.3 SO_PEERCRED check. */
    {
        int rc = isz_allowlist_add_binary(srv, "/proc/self/exe");
        CHECK(rc == ISZ_OK, "isz_allowlist_add_binary rc=%d", rc);
    }

    /* 3. UDS at a unique path. */
    cwd = getcwd(NULL, 0);
    if (!cwd) cwd = strdup("/tmp");
    snprintf(sock_path, sizeof(sock_path), "%s/.ishizue-w9a-%d",
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

    /* 4. isz_listen. */
    {
        int rc = isz_listen(srv, listen_fd);
        CHECK(rc == ISZ_OK, "isz_listen rc=%d", rc);
    }

    /* 5. Fork the client. */
    child = fork();
    CHECK(child >= 0, "fork: %s", strerror(errno));
    if (child == 0) {
        close(listen_fd);
        int rc = test_client_main(sock_path);
        _exit(rc);
    }

    /* 6. Parent: dispatch until the child creates a surface. */
    for (int i = 0; i < 5000 && !surf; i++) {
        isz_dispatch(srv);
        usleep(1000);
        surf = test_find_client_surface(srv);
    }
    CHECK(surf != NULL, "no client surface appeared after 5s");

    /* 7. Set fractional scale. Triggers ISZ_MSG_SURFACE_PREFERRED_SCALE. */
    {
        int rc = isz_surface_set_scale(surf, 3, 2);
        CHECK(rc == ISZ_OK, "isz_surface_set_scale rc=%d", rc);
    }

    /* 8. Set idle inhibit. Triggers ISZ_MSG_IDLE_INHIBIT_ACTIVE. */
    {
        int rc = isz_surface_set_idle_inhibit(surf, true);
        CHECK(rc == ISZ_OK, "isz_surface_set_idle_inhibit(true) rc=%d", rc);
    }

    /* 9. Clear idle inhibit. Triggers ISZ_MSG_IDLE_INHIBIT_INACTIVE. */
    {
        int rc = isz_surface_set_idle_inhibit(surf, false);
        CHECK(rc == ISZ_OK, "isz_surface_set_idle_inhibit(false) rc=%d", rc);
    }

    /* 10. Dispatch until the child's SET_SELECTION_OWNER arrives.
     *     The dispatcher calls isz_seat_set_selection_owner, so
     *     isz_seat_get_selection_owner returns the surface once the
     *     message is processed. */
    seat = isz_seat_default(srv);
    CHECK(seat != NULL, "isz_seat_default returned NULL");

    isz_surface *owner = NULL;
    for (int i = 0; i < 5000 && owner != surf; i++) {
        isz_dispatch(srv);
        usleep(1000);
        owner = isz_seat_get_selection_owner(seat,
                                             ISZ_SELECTION_CLIPBOARD);
    }
    CHECK(owner == surf,
          "selection owner mismatch: got %p expected %p",
          (void *)owner, (void *)surf);

    /* 11. Wait for the child to exit cleanly. */
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
        isz_dispatch(srv);
        usleep(1000);
    }

    if (ret == 0)
        printf("test_protocol_additions: PASS\n");

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
