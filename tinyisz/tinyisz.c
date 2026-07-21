/* SPDX-License-Identifier: MIT */
/* tinyisz.c - minimal tiling window manager on Ishizue.
 *
 * The Architect: owns the main loop, creates the UDS, spawns the X11
 * bridge subprocess, listens for events, applies the layout. Surfaces
 * are created by tinyisz itself; see README.md for the limitation.
 *
 * Usage:
 *   tinyisz [--backend drm|headless] [--bridge PATH]
 *           [--x11-display N] [--isz-socket PATH]
 *           [--width W] [--height H] [--refresh HZ]
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ishizue/isz.h>

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "input.h"

static volatile sig_atomic_t g_exit = 0;
static volatile sig_atomic_t g_sigchld = 0;

static void on_signal(int signo)
{
    if (signo == SIGCHLD) g_sigchld = 1;
    else g_exit = 1;
}

/* Forward isz_log_internal messages to stderr. The level is shown
 * as a short prefix so the user can see what's an error vs info. */
static const char *level_name(enum isz_log_level lvl)
{
    switch (lvl) {
    case ISZ_LOG_ERROR: return "error";
    case ISZ_LOG_WARN:  return "warn";
    case ISZ_LOG_INFO:  return "info";
    case ISZ_LOG_DEBUG: return "debug";
    default:            return "?";
    }
}

static void tinyisz_log_fn(void *userdata, enum isz_log_level level,
                           const char *msg)
{
    (void)userdata;
    fprintf(stderr, "tinyisz: %s: %s\n", level_name(level), msg);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [--backend drm|headless] [--bridge PATH]\n"
        "          [--x11-display N] [--isz-socket PATH]\n"
        "          [--width W] [--height H] [--refresh HZ]\n"
        "defaults: headless, $ISZ_X11BRIDGE_BIN or ../x11bridge/x11bridge,\n"
        "          :99, /tmp/.ishizue-tinyisz, 1280x720@60\n",
        prog);
}

static int make_listen_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return -1; }
    (void)unlink(path);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 16) < 0) {
        perror("bind/listen"); close(fd); return -1;
    }
    chmod(path, 0600);
    return fd;
}

/* Set LD_LIBRARY_PATH to the repo root (parent of tinyisz's binary
 * dir) so the bridge subprocess finds libishizue.so even when its
 * RUNPATH points at a stale build-test/ dir. */
static void set_ld_path_for_bridge(void)
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = '\0';
    char *p = strrchr(exe, '/');
    if (!p) return;
    *p = '\0';
    p = strrchr(exe, '/');
    if (!p) return;
    *p = '\0';
    const char *cur = getenv("LD_LIBRARY_PATH");
    char ld[4300];
    snprintf(ld, sizeof(ld), "%s%s%s", exe,
             (cur && cur[0]) ? ":" : "",
             (cur && cur[0]) ? cur : "");
    setenv("LD_LIBRARY_PATH", ld, 1);
}

static pid_t spawn_bridge(const char *bridge, const char *sock, int disp)
{
    char x11_sock[64];
    snprintf(x11_sock, sizeof(x11_sock), "/tmp/.X11-unix/X%d", disp);
    (void)unlink(x11_sock);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        setenv("ISZ_SOCKET", sock, 1);
        char d[16];
        snprintf(d, sizeof(d), "%d", disp);
        setenv("ISZ_X11_DISPLAY", d, 1);
        set_ld_path_for_bridge();
        execl(bridge, bridge, NULL);
        perror("execl bridge");
        _exit(127);
    }
    return pid;
}

static void on_client(void *ud, const isz_event *ev)
{
    struct tinyisz_ctx *c = ud;
    enum isz_event_type t = isz_event_get_type(ev);
    if (t == ISZ_EVENT_CLIENT_CONNECT) {
        fprintf(stderr, "tinyisz: client connected\n");
        tinyisz_ctx_on_client_connect(c);
    } else if (t == ISZ_EVENT_CLIENT_DISCONNECT) {
        fprintf(stderr, "tinyisz: client disconnected\n");
        tinyisz_ctx_on_client_disconnect(c);
    }
}

/* OUTPUT_ADD payload is not wired in the public API yet. Walk the
 * server's output list and enable any output we have not seen. */
static void on_output_add(void *ud, const isz_event *ev)
{
    struct tinyisz_ctx *c = ud;
    if (isz_event_get_type(ev) != ISZ_EVENT_OUTPUT_ADD)
        return;
    size_t n = 0;
    isz_output **list = isz_output_list(c->srv, &n);
    for (size_t i = 0; i < n; i++) {
        bool seen = false;
        for (size_t k = 0; k < c->n_outputs; k++)
            if (c->outputs[k] == list[i]) { seen = true; break; }
        if (seen)
            continue;
        size_t mn = 0;
        isz_mode **modes = isz_output_get_modes(list[i], &mn);
        if (modes && mn > 0 &&
            isz_output_enable(list[i], modes[0]) == ISZ_OK) {
            tinyisz_ctx_add_output(c, list[i]);
            fprintf(stderr, "tinyisz: output enabled (%zu)\n", c->n_outputs);
        }
    }
}

static void on_session(void *ud, const isz_event *ev)
{
    struct tinyisz_ctx *c = ud;
    enum isz_event_type t = isz_event_get_type(ev);
    if (t == ISZ_EVENT_SESSION_ACTIVE) {
        c->session_active = true;
        fprintf(stderr, "tinyisz: session active\n");
        tinyisz_ctx_retile(c);
    } else if (t == ISZ_EVENT_SESSION_INACTIVE) {
        c->session_active = false;
        fprintf(stderr, "tinyisz: session inactive\n");
    }
}

int main(int argc, char **argv)
{
    const char *backend_str = "headless", *bridge_flag = NULL;
    const char *isz_socket_path = "/tmp/.ishizue-tinyisz";
    int x11_display = 99, width = 1280, height = 720, refresh_hz = 60;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
            backend_str = argv[++i];
        else if (strcmp(argv[i], "--bridge") == 0 && i + 1 < argc)
            bridge_flag = argv[++i];
        else if (strcmp(argv[i], "--x11-display") == 0 && i + 1 < argc)
            x11_display = atoi(argv[++i]);
        else if (strcmp(argv[i], "--isz-socket") == 0 && i + 1 < argc)
            isz_socket_path = argv[++i];
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
            width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
            height = atoi(argv[++i]);
        else if (strcmp(argv[i], "--refresh") == 0 && i + 1 < argc)
            refresh_hz = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "tinyisz: unknown option: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    enum isz_backend_type backend;
    if (strcmp(backend_str, "drm") == 0) backend = ISZ_BACKEND_DRM;
    else if (strcmp(backend_str, "headless") == 0) backend = ISZ_BACKEND_HEADLESS;
    else { fprintf(stderr, "tinyisz: unknown backend: %s\n", backend_str); return 1; }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Wire a log sink so isz_log_internal messages reach stderr.
     * Without this, every library error is silently swallowed. */
    isz_set_log_callback(tinyisz_log_fn, NULL);

    isz_headless_config hcfg = {
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .refresh_rate = (uint32_t)(refresh_hz * 1000),
    };
    isz_server *srv = isz_init(backend,
        (backend == ISZ_BACKEND_HEADLESS) ? &hcfg : NULL);
    if (!srv) { fprintf(stderr, "tinyisz: isz_init failed\n"); return 1; }
    fprintf(stderr, "tinyisz: backend=%s %dx%d@%d\n",
            backend_str, width, height, refresh_hz);

    const char *bridge_path = bridge_flag;
    if (!bridge_path) {
        const char *env = getenv("ISZ_X11BRIDGE_BIN");
        bridge_path = (env && env[0]) ? env : "../x11bridge/x11bridge";
    }
    if (isz_allowlist_add_binary(srv, bridge_path) != ISZ_OK ||
        isz_allowlist_add_binary(srv, "/proc/self/exe") != ISZ_OK) {
        fprintf(stderr, "tinyisz: allowlist_add_binary(%s) failed\n",
                bridge_path);
        isz_destroy(srv);
        return 1;
    }

    int listen_fd = make_listen_socket(isz_socket_path);
    if (listen_fd < 0) { isz_destroy(srv); return 1; }
    if (isz_listen(srv, listen_fd) != ISZ_OK) {
        perror("isz_listen");
        close(listen_fd);
        isz_destroy(srv);
        (void)unlink(isz_socket_path);
        return 1;
    }
    fprintf(stderr, "tinyisz: listening on %s\n", isz_socket_path);

    struct tinyisz_ctx ctx;
    tinyisz_ctx_init(&ctx, srv, isz_seat_default(srv),
                     width, height, x11_display, &g_exit);

    /* Seed outputs: the headless backend creates the default output
     * inside isz_init, before listeners can register. */
    {
        size_t n = 0;
        isz_output **list = isz_output_list(srv, &n);
        for (size_t i = 0; i < n; i++) {
            size_t mn = 0;
            isz_mode **modes = isz_output_get_modes(list[i], &mn);
            if (modes && mn > 0 &&
                isz_output_enable(list[i], modes[0]) == ISZ_OK)
                tinyisz_ctx_add_output(&ctx, list[i]);
        }
    }

    isz_add_listener(srv, ISZ_EVENT_OUTPUT_ADD, on_output_add, &ctx);
    isz_add_listener(srv, ISZ_EVENT_SESSION_ACTIVE, on_session, &ctx);
    isz_add_listener(srv, ISZ_EVENT_SESSION_INACTIVE, on_session, &ctx);
    isz_add_listener(srv, ISZ_EVENT_CLIENT_CONNECT, on_client, &ctx);
    isz_add_listener(srv, ISZ_EVENT_CLIENT_DISCONNECT, on_client, &ctx);
    isz_add_listener(srv, ISZ_EVENT_INPUT_KEYBOARD_KEY,
        (isz_event_listener_fn)tinyisz_input_keyboard_key, &ctx);
    isz_add_listener(srv, ISZ_EVENT_INPUT_KEYBOARD_MODIFIERS,
        (isz_event_listener_fn)tinyisz_input_keyboard_modifiers, &ctx);
    isz_add_listener(srv, ISZ_EVENT_INPUT_POINTER_MOTION,
        (isz_event_listener_fn)tinyisz_input_pointer_motion, &ctx);

    pid_t bridge_pid = spawn_bridge(bridge_path, isz_socket_path, x11_display);
    if (bridge_pid < 0) {
        tinyisz_ctx_destroy(&ctx);
        isz_destroy(srv);
        close(listen_fd);
        (void)unlink(isz_socket_path);
        return 1;
    }
    fprintf(stderr, "tinyisz: bridge pid=%d, X11 :%d\n",
            (int)bridge_pid, x11_display);

    while (!g_exit) {
        isz_dispatch(srv);
        if (g_sigchld) {
            g_sigchld = 0;
            tinyisz_ctx_reap_children(&ctx);
            if (waitpid(bridge_pid, NULL, WNOHANG) == bridge_pid) {
                fprintf(stderr, "tinyisz: bridge exited\n");
                break;
            }
        }
        usleep(1000);
    }

    fprintf(stderr, "tinyisz: shutting down\n");
    kill(bridge_pid, SIGTERM);
    for (int i = 0; i < 2000 && waitpid(bridge_pid, NULL, WNOHANG) != bridge_pid; i++)
        usleep(1000);
    kill(bridge_pid, SIGKILL);
    waitpid(bridge_pid, NULL, 0);

    tinyisz_ctx_destroy(&ctx);
    isz_destroy(srv);
    close(listen_fd);
    (void)unlink(isz_socket_path);
    char x11_sock[64];
    snprintf(x11_sock, sizeof(x11_sock), "/tmp/.X11-unix/X%d", x11_display);
    (void)unlink(x11_sock);
    return 0;
}
