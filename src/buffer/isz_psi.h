/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_psi.h - PSI (memory pressure) monitor. SPEC §8.
 *
 * The library exposes the mechanism; the Architect decides whether to
 * enable it and where to set the threshold. If /sys/kernel/mm/pressure
 * is unavailable (kernel < 4.20 or built without CONFIG_PSI) the
 * monitor disables itself; there is no /proc/meminfo fallback. */

#ifndef ISZ_PSI_H
#define ISZ_PSI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../util/isz_compiler.h"

enum isz_psi_level {
    ISZ_PSI_SOME = 0,  /* at least one task stalled on memory */
    ISZ_PSI_FULL = 1,  /* all tasks stalled on memory */
};

/* Trigger specification. window_us is the aggregation window in
 * microseconds (must be >= 500000 and <= 10000000 per kernel docs);
 * threshold_pct is the percent of the window during which the level
 * must be exceeded for the trigger to fire (1..100). */
struct isz_psi_threshold {
    enum isz_psi_level level;
    uint32_t window_us;
    uint32_t threshold_pct;
};

/* One monitor per server (or per output, if the Architect wants
 * finer-grained pressure signals). The fd is pollable: when the
 * threshold is breached, the kernel makes it readable and dispatch
 * consumes the event. */
struct isz_psi_monitor {
    int  fd;             /* pollable; -1 if PSI unavailable or destroyed */
    bool enabled;        /* false when /sys/kernel/mm/pressure is absent */
    char path[64];       /* /sys/kernel/mm/pressure/memory by default */
    char trigger[64];    /* last-armed trigger string, for re-arming */
    bool armed;          /* a trigger is currently registered with kernel */
};

/* Open /sys/kernel/mm/pressure/memory. If the path is missing or
 * cannot be opened, the monitor is left in the disabled state and
 * ISZ_OK is returned (SPEC §8: "the monitoring mechanism is simply
 * disabled. No /proc/meminfo-based fallback"). init never fails on
 * PSI-unavailable; it only fails on invalid argument. */
int  isz_psi_init(struct isz_psi_monitor *mon) ISZ_INTERNAL;

/* Arm a trigger. If a previous trigger was armed, it is replaced.
 * No-op when the monitor is disabled. Silently ignores out-of-range
 * thresholds so the Architect doesn't need to gate every call. */
void isz_psi_set_threshold(struct isz_psi_monitor *mon,
                           const struct isz_psi_threshold *t) ISZ_INTERNAL;

/* Returns the pollable fd, or -1 if the monitor is disabled. The
 * Architect adds this to their epoll set; on EPOLLIN, they call
 * isz_psi_dispatch() and then act on the pressure event however they
 * choose (eviction is their policy, per §8). */
int  isz_psi_get_fd(struct isz_psi_monitor *mon) ISZ_INTERNAL;

/* Consume a pending pressure event. Reads the kernel's PSI record and
 * re-arms the trigger so the next breach is delivered. Returns ISZ_OK
 * on success or no-op; never returns a hard error so the dispatch loop
 * can't be derailed by a flaky PSI read. */
int  isz_psi_dispatch(struct isz_psi_monitor *mon) ISZ_INTERNAL;

/* Close the fd and mark the monitor disabled. NULL-tolerant. */
void isz_psi_destroy(struct isz_psi_monitor *mon) ISZ_INTERNAL;

#endif /* ISZ_PSI_H */
