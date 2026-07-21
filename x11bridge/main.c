/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Rui-727
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* main.c: X11 bridge entry point (W4-C).
 *
 * The bridge is a separate process. It connects to the Ishizue UDS,
 * runs the §6.2 handshake from the client side, opens the X11
 * listening socket (/tmp/.X11-unix/X<display>), and runs an epoll
 * loop that:
 *   - accepts X11 client connections and feeds them through
 *     x11_client_drain (which does the X11 setup handshake and
 *     parses subsequent requests, routing the ones it understands
 *     to translation.c),
 *   - reads Ishizue wire messages from the server and forwards the
 *     input events to the appropriate X11 client via translation.c.
 *
 * Configuration via environment:
 *   ISZ_SOCKET        path to the Ishizue UDS (default /tmp/.ishizue-0)
 *   ISZ_X11_DISPLAY   X11 display number (default 0) */

#define _GNU_SOURCE 1  /* accept4, EPOLL_CLOEXEC, signalfd */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "isz_client.h"
#include "isz_protocol.h"
#include "translation.h"
#include "x11_client.h"

#define ISZ_DEFAULT_SOCKET "/tmp/.ishizue-0"
#define X11_DIR            "/tmp/.X11-unix"
#define X11_MAX_CLIENTS    32

static volatile sig_atomic_t g_exit = 0;
static volatile sig_atomic_t g_last_signo = 0;

/* Log level: 0=error, 1=warn, 2=info, 3=debug. Read once from
 * ISZ_LOG_LEVEL env var. Default is info (2) so the bridge doesn't
 * flood the terminal with per-epoll_wait debug lines. */
static int g_log_level = 2;

static void init_log_level(void) {
    const char *s = getenv("ISZ_LOG_LEVEL");
    if (!s || !s[0]) return;
    if (strcmp(s, "error") == 0) g_log_level = 0;
    else if (strcmp(s, "warn") == 0) g_log_level = 1;
    else if (strcmp(s, "info") == 0) g_log_level = 2;
    else if (strcmp(s, "debug") == 0) g_log_level = 3;
}

static int level_num(const char *level) {
    if (strcmp(level, "error") == 0) return 0;
    if (strcmp(level, "warn") == 0) return 1;
    if (strcmp(level, "info") == 0) return 2;
    if (strcmp(level, "debug") == 0) return 3;
    return 2;
}

static void on_signal(int signo) {
    g_last_signo = signo;
    g_exit = 1;
}

static void log_msg(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void log_msg(const char *level, const char *fmt, ...) {
    if (level_num(level) > g_log_level) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "x11bridge: %s: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Parse the ISZ_X11_DISPLAY env var. Returns 0 on success, -1 on
 * error. */
static int parse_display(const char *s, int *out) {
    if (s == NULL || s[0] == '\0') {
        *out = 0;
        return 0;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        return -1;
    }
    if (v < 0 || v > 99) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

/* Create, bind, and listen on the X11 socket /tmp/.X11-unix/X<display>.
 * Returns the listening fd, or -1 on error. */
static int open_x11_listen(int display) {
    /* Ensure the directory exists with the conventional permissions.
     * The bridge may not have permission to create it if /tmp/.X11-unix
     * already exists with root ownership; in that case we use the
     * existing directory and rely on its permissions. */
    if (mkdir(X11_DIR, 0777) < 0 && errno != EEXIST) {
        log_msg("error", "mkdir(%s): %s", X11_DIR, strerror(errno));
        return -1;
    }
    (void)chmod(X11_DIR, 0777);

    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int n = snprintf(path, sizeof(path), "%s/X%d", X11_DIR, display);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        log_msg("error", "X11 socket path too long for display %d", display);
        return -1;
    }

    /* Remove any stale socket file. */
    (void)unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        log_msg("error", "socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* path is already bounded to sizeof(addr.sun_path); memcpy the
     * null-terminated string including the trailing '\0'. */
    memcpy(addr.sun_path, path, (size_t)n + 1u);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg("error", "bind(%s): %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        log_msg("error", "listen(%s): %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    log_msg("info", "X11 listening on %s (fd=%d)", path, fd);
    return fd;
}

/* Epoll tag kinds. */
enum fd_kind {
    FD_ISZ = 1,
    FD_X11_LISTEN,
    FD_X11_CLIENT,
};

struct fd_tag {
    enum fd_kind kind;
    void        *opaque;  /* struct x11_client * for FD_X11_CLIENT */
};

/* Slot registry: the bridge holds at most X11_MAX_CLIENTS clients.
 * The X11 client's epoll tag points back to its slot. */
struct client_slot {
    struct x11_client *c;
    struct fd_tag      tag;
    int                fd;
};

static struct client_slot g_slots[X11_MAX_CLIENTS];

static struct client_slot *slot_alloc(struct x11_client *c, int fd) {
    for (int i = 0; i < X11_MAX_CLIENTS; i++) {
        if (g_slots[i].c == NULL) {
            g_slots[i].c   = c;
            g_slots[i].fd  = fd;
            g_slots[i].tag.kind   = FD_X11_CLIENT;
            g_slots[i].tag.opaque = &g_slots[i];
            return &g_slots[i];
        }
    }
    return NULL;
}

static void slot_free(struct client_slot *s) {
    if (s == NULL) return;
    s->c  = NULL;
    s->fd = -1;
}

static struct client_slot *slot_find_by_tag(void *tag_ptr) {
    return (struct client_slot *)tag_ptr;
}

/* Build a non-blocking client array for translation_forward_* calls.
 * Returns the count, or 0 if none. */
static size_t build_clients_array(struct x11_client **out, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < X11_MAX_CLIENTS && n < cap; i++) {
        if (g_slots[i].c != NULL) {
            out[n++] = g_slots[i].c;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Accept an X11 client connection. */ 
/* ------------------------------------------------------------------ */
static void on_x11_accept(int epoll_fd, int listen_fd, struct isz_client *isz) {
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            log_msg("warn", "accept4: %s", strerror(errno));
            break;
        }

        struct x11_client *c = x11_client_create(fd);
        if (c == NULL) {
            log_msg("warn", "x11_client_create failed");
            /* fd already closed by x11_client_create on failure. */
            continue;
        }

        struct client_slot *s = slot_alloc(c, fd);
        if (s == NULL) {
            log_msg("warn", "client table full; rejecting fd=%d", fd);
            x11_client_destroy(c);
            continue;
        }

        /* Run the X11 connection setup handshake synchronously here,
         * with the socket switched to blocking mode. The setup is
         * small (12 bytes in + a few hundred bytes out) and runs
         * once per connection, so blocking is fine. After setup we
         * switch back to non-blocking for epoll-driven request
         * parsing. This mirrors the Ishizue server's pattern
         * (isz_listen.c). */
        int blk_flags = fcntl(fd, F_GETFL, 0);
        if (blk_flags < 0 ||
            fcntl(fd, F_SETFL, blk_flags & ~O_NONBLOCK) < 0) {
            log_msg("warn", "fcntl blocking: %s", strerror(errno));
            x11_client_destroy(c);
            slot_free(s);
            continue;
        }

        int rc = x11_client_drain(c, isz);
        if (rc < 0) {
            log_msg("info", "X11 client fd=%d setup failed", fd);
            x11_client_destroy(c);
            slot_free(s);
            continue;
        }

        /* Switch back to non-blocking for epoll-driven I/O. */
        if (fcntl(fd, F_SETFL, blk_flags | O_NONBLOCK) < 0) {
            log_msg("warn", "fcntl non-blocking: %s", strerror(errno));
            x11_client_destroy(c);
            slot_free(s);
            continue;
        }

        struct epoll_event ev;
        ev.events   = EPOLLIN;
        ev.data.ptr = &s->tag;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            log_msg("warn", "epoll_ctl ADD client: %s",
                    strerror(errno));
            x11_client_destroy(c);
            slot_free(s);
            continue;
        }
        log_msg("info", "X11 client connected fd=%d", fd);
    }
}

/* ------------------------------------------------------------------ */
/* Handle a ready X11 client socket. */
/* ------------------------------------------------------------------ */
static void on_x11_client_ready(int epoll_fd, struct isz_client *isz,
                                struct client_slot *s) {
    if (s == NULL || s->c == NULL) return;
    log_msg("debug", "on_x11_client_ready: fd=%d", s->fd);
    int rc = x11_client_drain(s->c, isz);
    if (rc < 0) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, s->fd, NULL);
        log_msg("info", "X11 client fd=%d disconnected", s->fd);
        x11_client_destroy(s->c);
        s->c = NULL;
        s->fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Handle a ready Ishizue socket. */
/* ------------------------------------------------------------------ */
static void on_isz_ready(struct isz_client *isz) {
    for (;;) {
        uint8_t buf[ISZ_PROTO_MAX_MESSAGE];
        uint32_t msg_id = 0;
        size_t payload_len = 0;
        ssize_t n = isz_client_recv(isz, buf, sizeof(buf),
                                    &msg_id, &payload_len);
        if (n < 0) {
            log_msg("error", "isz recv error; disconnecting from server");
            g_exit = 1;  /* shut down the bridge */
            return;
        }
        if (n == 0) return;  /* no complete message */

        const uint8_t *payload = buf + ISZ_MSG_HEADER_SIZE;
        struct x11_client *clients[X11_MAX_CLIENTS];
        size_t ncl = build_clients_array(clients,
                                         sizeof(clients) / sizeof(clients[0]));

        switch (msg_id) {
        case ISZ_MSG_INPUT_KEYBOARD_KEY: {
            /* Provisional layout: u32 keycode, u8 pressed. */
            if (payload_len < 5) break;
            uint32_t keycode =
                (uint32_t)payload[0]        |
               ((uint32_t)payload[1] << 8)  |
               ((uint32_t)payload[2] << 16) |
               ((uint32_t)payload[3] << 24);
            bool pressed = payload[4] != 0u;
            (void)translation_forward_keyboard_key(clients, ncl,
                                                   keycode, pressed);
            break;
        }
        case ISZ_MSG_INPUT_POINTER_MOTION: {
            /* Provisional layout: i32 x, i32 y (absolute). */
            if (payload_len < 8) break;
            int32_t x = (int32_t)(
                (uint32_t)payload[0]        |
               ((uint32_t)payload[1] << 8)  |
               ((uint32_t)payload[2] << 16) |
               ((uint32_t)payload[3] << 24));
            int32_t y = (int32_t)(
                (uint32_t)payload[4]        |
               ((uint32_t)payload[5] << 8)  |
               ((uint32_t)payload[6] << 16) |
               ((uint32_t)payload[7] << 24));
            (void)translation_forward_pointer_motion(clients, ncl, x, y);
            break;
        }
        case ISZ_MSG_INPUT_POINTER_BUTTON: {
            /* Provisional layout: u32 button, u8 pressed. */
            if (payload_len < 5) break;
            uint32_t button =
                (uint32_t)payload[0]        |
               ((uint32_t)payload[1] << 8)  |
               ((uint32_t)payload[2] << 16) |
               ((uint32_t)payload[3] << 24);
            bool pressed = payload[4] != 0u;
            (void)translation_forward_pointer_button(clients, ncl,
                                                     button, pressed);
            break;
        }
        case ISZ_MSG_INPUT_KEYBOARD_MODIFIERS:
            /* No X11 equivalent in the scaffold (would map to a
             * state-mask update on subsequent events). */
            break;
        case ISZ_MSG_PRESENTED:
            /* Presentation feedback. The bridge does not yet forward
             * this to X11 clients. */
            break;
        case ISZ_MSG_RELEASE:
            /* Buffer release. The bridge does not yet attach buffers. */
            break;
        default:
            log_msg("debug", "isz msg_id=%u payload=%zu (no handler)",
                    (unsigned)msg_id, payload_len);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* main */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    init_log_level();

    /* Signal handling: SIGINT and SIGTERM flip g_exit. We let
     * epoll_wait return EINTR rather than using signalfd so the
     * teardown path stays simple. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* Ignore SIGPIPE so a send to a closed X11 socket just returns
     * EPIPE instead of killing the bridge. */
    signal(SIGPIPE, SIG_IGN);
    log_msg("info", "bridge pid=%d ppid=%d", (int)getpid(), (int)getppid());

    const char *isz_path = getenv("ISZ_SOCKET");
    if (isz_path == NULL || isz_path[0] == '\0') {
        isz_path = ISZ_DEFAULT_SOCKET;
    }
    int display = 0;
    if (parse_display(getenv("ISZ_X11_DISPLAY"), &display) < 0) {
        log_msg("error", "ISZ_X11_DISPLAY must be a number 0..99");
        return 1;
    }

    log_msg("info", "connecting to Ishizue at %s", isz_path);
    struct isz_client *isz = isz_client_connect(isz_path);
    if (isz == NULL) {
        log_msg("error", "isz_client_connect failed");
        return 1;
    }
    if (isz_client_handshake(isz) < 0) {
        log_msg("error", "isz_client_handshake failed "
                         "(server down or bridge not in allowlist?)");
        isz_client_destroy(isz);
        return 1;
    }

    int x11_listen = open_x11_listen(display);
    if (x11_listen < 0) {
        isz_client_destroy(isz);
        return 1;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        log_msg("error", "epoll_create1: %s", strerror(errno));
        close(x11_listen);
        isz_client_destroy(isz);
        return 1;
    }

    struct fd_tag isz_tag    = { FD_ISZ,        NULL };
    struct fd_tag listen_tag = { FD_X11_LISTEN, NULL };

    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.ptr = &isz_tag;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, isz_client_fd(isz), &ev) < 0) {
        log_msg("error", "epoll_ctl ADD isz: %s", strerror(errno));
        goto fail;
    }
    ev.events   = EPOLLIN;
    ev.data.ptr = &listen_tag;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, x11_listen, &ev) < 0) {
        log_msg("error", "epoll_ctl ADD listen: %s", strerror(errno));
        goto fail;
    }

    log_msg("info", "bridge ready; ISZ_SOCKET=%s X11=:%d",
            isz_path, display);
    log_msg("debug", "entering main loop g_exit=%d", (int)g_exit);

    while (!g_exit) {
        struct epoll_event events[16];
        log_msg("debug", "calling epoll_wait g_exit=%d", (int)g_exit);
        int n = epoll_wait(epoll_fd, events,
                           (int)(sizeof(events) / sizeof(events[0])),
                           -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_msg("error", "epoll_wait: %s", strerror(errno));
            break;
        }
        log_msg("debug", "epoll_wait returned n=%d g_exit=%d", n, (int)g_exit);
        for (int i = 0; i < n && !g_exit; i++) {
            struct fd_tag *t = (struct fd_tag *)events[i].data.ptr;
            if (t == NULL) continue;
            switch (t->kind) {
            case FD_ISZ:
                on_isz_ready(isz);
                break;
            case FD_X11_LISTEN:
                on_x11_accept(epoll_fd, x11_listen, isz);
                break;
            case FD_X11_CLIENT:
                on_x11_client_ready(epoll_fd, isz,
                                    slot_find_by_tag(t->opaque));
                break;
            default:
                break;
            }
        }
    }

    log_msg("info", "shutting down (last_signo=%d)", (int)g_last_signo);
    /* Tear down X11 clients. */
    for (int i = 0; i < X11_MAX_CLIENTS; i++) {
        if (g_slots[i].c != NULL) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, g_slots[i].fd, NULL);
            x11_client_destroy(g_slots[i].c);
            slot_free(&g_slots[i]);
        }
    }
    close(x11_listen);
    /* Unlink the X11 socket file so the next launch does not trip on
     * "address already in use". */
    {
        char path[64];
        snprintf(path, sizeof(path), "%s/X%d", X11_DIR, display);
        unlink(path);
    }
    close(epoll_fd);
    isz_client_destroy(isz);
    return 0;

fail:
    close(epoll_fd);
    close(x11_listen);
    isz_client_destroy(isz);
    return 1;
}
