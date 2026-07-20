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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* x11_atoms.c: bridge-global X11 atom table.
 *
 * Predefined atoms 1..68 are seeded at first use from the official X11
 * core-protocol name list. User-allocated atoms start at
 * X11_ATOM_FIRST_DYNAMIC (69) and grow upward without bound (capped by
 * the heap). The table is a flat array of (id, name) pairs; v1 client
 * atom counts are small enough that a linear scan is fine. */

#define _GNU_SOURCE 1

#include "x11_atoms.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The 68 predefined atom names in atom-number order. Sourced from
 * the X11 core protocol spec; see doc/research/x11-protocol-internals.md
 * section 12. */
static const char *const X11_PREDEFINED_ATOM_NAMES[68] = {
    "PRIMARY",                         /* 1 */
    "SECONDARY",                       /* 2 */
    "ARC",                             /* 3 */
    "ATOM",                            /* 4 */
    "BITMAP",                          /* 5 */
    "CARDINAL",                        /* 6 */
    "COLORMAP",                        /* 7 */
    "CURSOR",                          /* 8 */
    "CUT_BUFFER0",                     /* 9 */
    "CUT_BUFFER1",                     /* 10 */
    "CUT_BUFFER2",                     /* 11 */
    "CUT_BUFFER3",                     /* 12 */
    "CUT_BUFFER4",                     /* 13 */
    "CUT_BUFFER5",                     /* 14 */
    "CUT_BUFFER6",                     /* 15 */
    "CUT_BUFFER7",                     /* 16 */
    "DRAWABLE",                        /* 17 */
    "FONT",                            /* 18 */
    "INTEGER",                         /* 19 */
    "PIXMAP",                          /* 20 */
    "POINT",                           /* 21 */
    "RECTANGLE",                       /* 22 */
    "RESOURCE_MANAGER",                /* 23 */
    "RGB_COLOR_MAP",                   /* 24 */
    "RGB_BEST_MAP",                    /* 25 */
    "RGB_BLUE_MAP",                    /* 26 */
    "RGB_DEFAULT_MAP",                 /* 27 */
    "RGB_GRAY_MAP",                    /* 28 */
    "RGB_GREEN_MAP",                   /* 29 */
    "RGB_RED_MAP",                     /* 30 */
    "STRING",                          /* 31 */
    "VISUALID",                        /* 32 */
    "WINDOW",                          /* 33 */
    "WM_COMMAND",                      /* 34 */
    "WM_HINTS",                        /* 35 */
    "WM_CLIENT_MACHINE",               /* 36 */
    "WM_ICON_NAME",                    /* 37 */
    "WM_ICON_SIZE",                    /* 38 */
    "WM_NAME",                         /* 39 */
    "WM_NORMAL_HINTS",                 /* 40 */
    "WM_SIZE_HINTS",                   /* 41 */
    "WM_ZOOM_HINTS",                   /* 42 */
    "MIN_SPACE",                       /* 43 */
    "NORM_SPACE",                      /* 44 */
    "MAX_SPACE",                       /* 45 */
    "END_SPACE",                       /* 46 */
    "SUPERSCRIPT_X",                   /* 47 */
    "SUPERSCRIPT_Y",                   /* 48 */
    "SUBSCRIPT_X",                     /* 49 */
    "SUBSCRIPT_Y",                     /* 50 */
    "UNDERLINE_POSITION",              /* 51 */
    "UNDERLINE_THICKNESS",             /* 52 */
    "STRIKEOUT_ASCENT",                /* 53 */
    "STRIKEOUT_DESCENT",               /* 54 */
    "ITALIC_ANGLE",                    /* 55 */
    "X_HEIGHT",                        /* 56 */
    "QUAD_WIDTH",                      /* 57 */
    "WEIGHT",                          /* 58 */
    "POINT_SIZE",                      /* 59 */
    "RESOLUTION",                      /* 60 */
    "COPYRIGHT",                       /* 61 */
    "NOTICE",                          /* 62 */
    "FONT_NAME",                       /* 63 */
    "FAMILY_NAME",                     /* 64 */
    "FULL_NAME",                       /* 65 */
    "CAP_HEIGHT",                      /* 66 */
    "WM_CLASS",                        /* 67 */
    "WM_TRANSIENT_FOR",                /* 68 */
};

struct x11_atom_entry {
    uint32_t id;
    size_t   name_len;
    char    *name;  /* NUL-terminated; not strictly required but cheap */
};

/* Process-wide dynamic atom table. Grown geometrically. The first 68
 * slots are never stored here: the predefined names are looked up
 * directly from X11_PREDEFINED_ATOM_NAMES. */
static struct x11_atom_entry *g_dynamic_atoms = NULL;
static size_t g_dynamic_atoms_count = 0;
static size_t g_dynamic_atoms_cap = 0;
static uint32_t g_next_atom = X11_ATOM_FIRST_DYNAMIC;

static void x11_atoms_log(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void x11_atoms_log(const char *level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "x11bridge/atoms: %s: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Compare a name of given length to a NUL-terminated reference. */
static bool name_eq(const char *stored, size_t stored_len,
                    const char *name, size_t name_len) {
    if (stored_len != name_len) return false;
    return memcmp(stored, name, name_len) == 0;
}

uint32_t x11_atom_intern(const char *name, size_t name_len,
                         bool only_if_exists) {
    if (name == NULL || name_len == 0u) return 0u;

    /* Predefined atoms 1..68. */
    for (size_t i = 0; i < 68u; i++) {
        const char *pname = X11_PREDEFINED_ATOM_NAMES[i];
        size_t plen = strlen(pname);
        if (name_eq(pname, plen, name, name_len)) {
            return (uint32_t)(i + 1u);
        }
    }

    /* User-allocated atoms. */
    for (size_t i = 0; i < g_dynamic_atoms_count; i++) {
        if (name_eq(g_dynamic_atoms[i].name, g_dynamic_atoms[i].name_len,
                    name, name_len)) {
            return g_dynamic_atoms[i].id;
        }
    }

    if (only_if_exists) {
        return 0u;  /* None */
    }

    /* Allocate a new atom. */
    if (g_dynamic_atoms_count == g_dynamic_atoms_cap) {
        size_t new_cap = (g_dynamic_atoms_cap == 0u) ? 16u
                                                     : g_dynamic_atoms_cap * 2u;
        struct x11_atom_entry *grown =
            realloc(g_dynamic_atoms, new_cap * sizeof(*grown));
        if (grown == NULL) {
            x11_atoms_log("warn", "intern: out of memory for %zu-byte name",
                          name_len);
            return 0u;
        }
        g_dynamic_atoms = grown;
        g_dynamic_atoms_cap = new_cap;
    }

    char *copy = malloc(name_len + 1u);
    if (copy == NULL) {
        x11_atoms_log("warn", "intern: out of memory duplicating name");
        return 0u;
    }
    memcpy(copy, name, name_len);
    copy[name_len] = '\0';

    struct x11_atom_entry *e = &g_dynamic_atoms[g_dynamic_atoms_count++];
    e->id = g_next_atom++;
    e->name_len = name_len;
    e->name = copy;
    return e->id;
}

size_t x11_atom_get_name(uint32_t atom, char *out_buf, size_t out_cap) {
    if (out_buf == NULL || out_cap == 0u) return 0u;
    out_buf[0] = '\0';

    if (atom >= 1u && atom <= 68u) {
        const char *pname = X11_PREDEFINED_ATOM_NAMES[atom - 1u];
        size_t plen = strlen(pname);
        if (plen + 1u > out_cap) {
            /* Truncate so the caller still gets a NUL-terminated prefix. */
            memcpy(out_buf, pname, out_cap - 1u);
            out_buf[out_cap - 1u] = '\0';
            return plen;  /* Report the true length. */
        }
        memcpy(out_buf, pname, plen + 1u);
        return plen;
    }

    for (size_t i = 0; i < g_dynamic_atoms_count; i++) {
        if (g_dynamic_atoms[i].id == atom) {
            size_t plen = g_dynamic_atoms[i].name_len;
            const char *pname = g_dynamic_atoms[i].name;
            if (plen + 1u > out_cap) {
                memcpy(out_buf, pname, out_cap - 1u);
                out_buf[out_cap - 1u] = '\0';
                return plen;
            }
            memcpy(out_buf, pname, plen + 1u);
            return plen;
        }
    }
    return 0u;
}
