/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* x11_atoms.h: bridge-global X11 atom table.
 *
 * X11 atoms are server-global (not per-client) and never freed. The
 * predefined atoms 1..68 exist without InternAtom; user-allocated
 * atoms start at 69. The bridge keeps one process-wide table that
 * every X11 client reaches via x11_atom_intern / x11_atom_lookup.
 *
 * Atom 0 is reserved as None (the no-atom sentinel). */

#ifndef X11_ATOMS_H
#define X11_ATOMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* First atom id the bridge may allocate to a non-predefined name. */
#define X11_ATOM_FIRST_DYNAMIC 69u

/* Look up an atom by name. Returns 0 (None) if the name is not in
 * the table and only_if_exists is true. Otherwise (only_if_exists is
 * false) the name is added as a new atom and its id is returned.
 * name_len is in bytes; name need not be NUL-terminated. The table
 * keeps its own copy. */
uint32_t x11_atom_intern(const char *name, size_t name_len,
                         bool only_if_exists);

/* Reverse lookup: write the atom's name into out_buf and return its
 * length in bytes, not including any NUL. Returns 0 if the atom is
 * not in the table (or is atom 0). out_buf is NUL-terminated for
 * caller convenience, so out_cap must be at least 1 byte. */
size_t x11_atom_get_name(uint32_t atom, char *out_buf, size_t out_cap);

#endif /* X11_ATOMS_H */
