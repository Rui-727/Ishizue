/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_compiler.h - compiler hints and small macros. Used across src/.
 * No spec section; pure utility. */

#ifndef ISZ_COMPILER_H
#define ISZ_COMPILER_H

#include <stddef.h>

/* Visibility. -fvisibility=hidden in CFLAGS already defaults to hidden;
 * these make intent explicit and survive compilation without that flag. */
#ifndef ISZ_API
#define ISZ_API __attribute__((visibility("default")))
#endif
#define ISZ_INTERNAL __attribute__((visibility("hidden")))

#define ISZ_LIKELY(x)   __builtin_expect(!!(x), 1)
#define ISZ_UNLIKELY(x) __builtin_expect(!!(x), 0)

#define ISZ_UNUSED(x) ((void)(x))

#define ISZ_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#endif /* ISZ_COMPILER_H */
