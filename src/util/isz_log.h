/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_log.h - internal logging entry point. Public API in isz.h §12. */

#ifndef ISZ_LOG_H
#define ISZ_LOG_H

#include <ishizue/isz.h>

#include "isz_compiler.h"

/* Hidden library-internal entry point. Formats with vsnprintf, applies
 * the cached ISZ_LOG_LEVEL filter (SPEC §4), and invokes the registered
 * callback. No callback = silent no-op. Safe to call from any thread. */
ISZ_INTERNAL void isz_log_internal(enum isz_log_level level,
                                   const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* ISZ_LOG_H */
