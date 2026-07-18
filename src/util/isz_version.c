/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_version.c - runtime version query, see SPEC §4 (versioning). */

#include <ishizue/isz.h>

ISZ_API const char *isz_version_string(void)
{
    return ISHIZUE_VERSION_STRING;
}
