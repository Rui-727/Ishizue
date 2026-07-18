/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_psi.c - PSI monitor. SPEC §8.
 *
 * Wave 1 scope: the polling shape. The Architect wires this into
 * their epoll loop, sets a threshold, and on dispatch decides what
 * offscreen backing storage to evict. Eviction itself is not
 * implemented here; this file only owns the kernel PSI trigger
 * lifecycle. */

/* Enable POSIX.1-2008 symbols (O_CLOEXEC, stat, S_ISDIR) under -std=c11.
 * The Makefile does not define _GNU_SOURCE; each .c file opts in. */
#define _POSIX_C_SOURCE 200809L

#include "isz_psi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ishizue/isz.h>

#define ISZ_PSI_DIR "/sys/kernel/mm/pressure"

/* The server's logger is wired in a later wave. Until then, the PSI
 * "PSI unavailable" debug message has nowhere to go; we fall back to
 * silent disable, which is the behaviour SPEC §8 requires regardless
 * of whether the debug line is emitted. The parent task will route
 * a log callback into this module. */

static bool psi_available(void)
{
    struct stat st;
    if (stat(ISZ_PSI_DIR, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

int isz_psi_init(struct isz_psi_monitor *mon)
{
    if (mon == NULL) return ISZ_ERR_INVALID_ARG;

    memset(mon, 0, sizeof(*mon));
    mon->fd      = -1;
    mon->enabled = false;
    mon->armed   = false;
    /* Default to memory pressure; cpu and io exist too but the spec
     * only calls out memory-pressure-driven eviction. */
    (void)snprintf(mon->path, sizeof(mon->path),
                   "%s/memory", ISZ_PSI_DIR);

    if (!psi_available()) {
        /* SPEC §8: PSI unavailable -> disabled, no fallback. Don't
         * fail init; the caller treats a disabled monitor the same as
         * an absent one. The debug-log line is deferred until the
         * logger is wired. */
        return ISZ_OK;
    }

    int fd = open(mon->path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        /* Directory exists but the memory file is missing or
         * unreadable. Treat the same as PSI-unavailable. */
        return ISZ_OK;
    }

    mon->fd      = fd;
    mon->enabled = true;
    return ISZ_OK;
}

void isz_psi_set_threshold(struct isz_psi_monitor *mon,
                           const struct isz_psi_threshold *t)
{
    if (mon == NULL || t == NULL || !mon->enabled) return;
    if (mon->fd < 0) return;

    /* Kernel constraints: window in [500000, 10000000] us, threshold
     * in [1, 100]. Clamp rather than reject so the Architect can pass
     * rough values without gating each call. */
    uint32_t window = t->window_us;
    if (window < 500000u)  window = 500000u;
    if (window > 10000000u) window = 10000000u;
    uint32_t pct = t->threshold_pct;
    if (pct == 0u)   pct = 1u;
    if (pct > 100u)  pct = 100u;

    int n = snprintf(mon->trigger, sizeof(mon->trigger),
                     "%s %u %u",
                     t->level == ISZ_PSI_FULL ? "full" : "some",
                     window, pct);
    if (n < 0 || (size_t)n >= sizeof(mon->trigger)) {
        mon->trigger[0] = '\0';
        return;
    }

    /* Writing the trigger string arms it. The write may fail with
     * EINVAL if the kernel rejects the combination; treat that as
     * "not armed" and let dispatch re-arm with the next call. */
    ssize_t w = write(mon->fd, mon->trigger, (size_t)n);
    if (w == (ssize_t)n) {
        mon->armed = true;
    }
    /* errno on a short write is informational only; nothing to act on
     * from inside the library. */
}

int isz_psi_get_fd(struct isz_psi_monitor *mon)
{
    if (mon == NULL) return -1;
    if (!mon->enabled) return -1;
    return mon->fd;
}

int isz_psi_dispatch(struct isz_psi_monitor *mon)
{
    if (mon == NULL || !mon->enabled || mon->fd < 0) return ISZ_OK;

    /* Drain the kernel's PSI record. The exact contents (one-line
     * stats for some/full, averaged over 10s/60s/300s) are not
     * parsed here; the Architect reads them via a separate path if
     * they want detail. This call exists to clear the readability
     * edge so the fd stops showing up in epoll until the next
     * trigger. */
    char buf[256];
    ssize_t n;
    do {
        n = read(mon->fd, buf, sizeof(buf));
    } while (n < 0 && (errno == EINTR));

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* Spurious wakeup; nothing to consume. */
        return ISZ_OK;
    }
    if (n < 0) {
        /* Other read errors: log-and-continue once the logger is
         * wired. For now, swallow. */
        return ISZ_OK;
    }

    /* Triggers are one-shot: once fired, the kernel will not signal
     * again until re-armed. Re-arm with the last-set threshold so
     * the Architect doesn't have to call set_threshold every cycle. */
    if (mon->armed && mon->trigger[0] != '\0') {
        size_t len = strlen(mon->trigger);
        ssize_t w = write(mon->fd, mon->trigger, len);
        if (w != (ssize_t)len) {
            mon->armed = false;
        }
    }
    return ISZ_OK;
}

void isz_psi_destroy(struct isz_psi_monitor *mon)
{
    if (mon == NULL) return;
    if (mon->fd >= 0) {
        close(mon->fd);
        mon->fd = -1;
    }
    mon->enabled = false;
    mon->armed   = false;
    mon->trigger[0] = '\0';
}
