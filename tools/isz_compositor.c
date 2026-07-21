/* isz_compositor.c - minimal Ishizue compositor host.
 *
 * Initializes a headless Ishizue server, listens on a UDS, spawns the
 * x11bridge as a subprocess, and runs the dispatch loop. X11 clients
 * (xeyes, xterm, etc.) can then connect to the bridge's X11 socket.
 *
 * Usage:
 *   isz_compositor --bridge /path/to/x11bridge --x11-display 99
 *
 * The compositor runs until SIGINT or SIGTERM. On exit it kills the
 * bridge subprocess and tears down the server.
 *
 * This is NOT a real window manager. It has no tiling logic, no focus
 * policy, no hotkeys. It just wires the library to the bridge so X11
 * clients have something to talk to. Per SPEC §1, all WM policy is the
 * Architect's job; this host is the thinnest possible Architect.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ishizue/isz.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_exit = 0;

static void on_signal(int signo) {
    (void)signo;
    g_exit = 1;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --bridge PATH       path to x11bridge binary (required)\n"
        "  --x11-display N     X11 display number (default 99)\n"
        "  --isz-socket PATH   Ishizue UDS path (default /tmp/.ishizue-compositor)\n"
        "  --width W           headless output width (default 1024)\n"
        "  --height H          headless output height (default 768)\n"
        "  --refresh HZ        headless output refresh in Hz (default 60)\n",
        prog);
}

int main(int argc, char **argv) {
    const char *bridge_path = NULL;
    int x11_display = 99;
    const char *isz_socket_path = "/tmp/.ishizue-compositor";
    int width = 1024, height = 768, refresh_hz = 60;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bridge") == 0 && i + 1 < argc) {
            bridge_path = argv[++i];
        } else if (strcmp(argv[i], "--x11-display") == 0 && i + 1 < argc) {
            x11_display = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--isz-socket") == 0 && i + 1 < argc) {
            isz_socket_path = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--refresh") == 0 && i + 1 < argc) {
            refresh_hz = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!bridge_path) {
        fprintf(stderr, "error: --bridge is required\n");
        usage(argv[0]);
        return 1;
    }

    /* Set up signal handlers. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* 1. Initialize the headless Ishizue server. */
    isz_headless_config cfg = {
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .refresh_rate = (uint32_t)(refresh_hz * 1000),
    };
    isz_server *srv = isz_init(ISZ_BACKEND_HEADLESS, &cfg);
    if (!srv) {
        fprintf(stderr, "error: isz_init failed\n");
        return 1;
    }
    fprintf(stderr, "compositor: server initialized (%dx%d@%d)\n",
            width, height, refresh_hz);

    /* 2. Allowlist the bridge binary. */
    int rc = isz_allowlist_add_binary(srv, bridge_path);
    if (rc != ISZ_OK) {
        fprintf(stderr, "error: isz_allowlist_add_binary: %d\n", rc);
        isz_destroy(srv);
        return 1;
    }

    /* 3. Create the Ishizue UDS. */
    (void)unlink(isz_socket_path);
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "error: socket: %s\n", strerror(errno));
        isz_destroy(srv);
        return 1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, isz_socket_path, sizeof(addr.sun_path) - 1);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "error: bind(%s): %s\n", isz_socket_path, strerror(errno));
        close(listen_fd);
        isz_destroy(srv);
        return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        fprintf(stderr, "error: listen: %s\n", strerror(errno));
        close(listen_fd);
        isz_destroy(srv);
        return 1;
    }
    chmod(isz_socket_path, 0600);

    /* 4. Tell the library to accept on this fd. */
    rc = isz_listen(srv, listen_fd);
    if (rc != ISZ_OK) {
        fprintf(stderr, "error: isz_listen: %d\n", rc);
        close(listen_fd);
        isz_destroy(srv);
        return 1;
    }
    fprintf(stderr, "compositor: listening on %s\n", isz_socket_path);

    /* 5. Spawn the bridge. */
    char x11_sock[128];
    snprintf(x11_sock, sizeof(x11_sock), "/tmp/.X11-unix/X%d", x11_display);
    (void)unlink(x11_sock);

    pid_t bridge_pid = fork();
    if (bridge_pid < 0) {
        fprintf(stderr, "error: fork: %s\n", strerror(errno));
        close(listen_fd);
        isz_destroy(srv);
        return 1;
    }
    if (bridge_pid == 0) {
        /* Child: exec the bridge. */
        setenv("ISZ_SOCKET", isz_socket_path, 1);
        char display_env[32];
        snprintf(display_env, sizeof(display_env), "%d", x11_display);
        setenv("ISZ_X11_DISPLAY", display_env, 1);
        execl(bridge_path, bridge_path, NULL);
        perror("execl");
        _exit(127);
    }
    fprintf(stderr, "compositor: bridge pid=%d, X11 :%d\n", bridge_pid, x11_display);

    /* 6. Run the dispatch loop. */
    while (!g_exit) {
        isz_dispatch(srv);
        /* Check if the bridge is still alive. */
        pid_t w = waitpid(bridge_pid, NULL, WNOHANG);
        if (w == bridge_pid) {
            fprintf(stderr, "compositor: bridge exited\n");
            break;
        }
        usleep(1000);
    }

    /* 7. Clean up. */
    if (g_exit) {
        fprintf(stderr, "compositor: shutting down\n");
        kill(bridge_pid, SIGTERM);
        /* Wait up to 2 seconds for the bridge to exit. */
        for (int i = 0; i < 2000; i++) {
            pid_t w = waitpid(bridge_pid, NULL, WNOHANG);
            if (w == bridge_pid) break;
            usleep(1000);
        }
        /* Force kill if still alive. */
        kill(bridge_pid, SIGKILL);
        waitpid(bridge_pid, NULL, 0);
    }

    isz_destroy(srv);
    close(listen_fd);
    (void)unlink(isz_socket_path);
    (void)unlink(x11_sock);
    return 0;
}
