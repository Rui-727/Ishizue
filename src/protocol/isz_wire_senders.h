/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Rui-727
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense,
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* isz_wire_senders.h - server-to-client wire message senders (W9-A).
 *
 * Five S2C messages added by W8-B are sent from the library side to
 * the owning client's connection. The implementations live in
 * isz_client_dispatch.c (alongside isz_send_presented /
 * isz_send_release / isz_send_capture_done); this header gives the
 * render and input layers a place to call them from without pulling
 * in the dispatcher's other internals.
 *
 * All senders are NULL-tolerant on conn (a NULL owning conn, e.g. an
 * Architect-created surface, is a silent no-op). Text and cursor
 * arguments are borrowed for the duration of the call only; the
 * sender copies them into the wire frame before returning. */

#ifndef ISZ_WIRE_SENDERS_H
#define ISZ_WIRE_SENDERS_H

#include <stddef.h>
#include <stdint.h>

#include "../util/isz_compiler.h"

struct isz_conn;

/* SPEC §7.2: preferred fractional scale for a surface. Payload is
 * u32 surface_id + u32 numerator + u32 denominator. Sent when
 * isz_surface_set_scale stores a new (numerator, denominator) pair. */
void isz_send_surface_preferred_scale(struct isz_conn *conn,
                                      uint32_t surface_id,
                                      uint32_t numerator,
                                      uint32_t denominator)
    ISZ_INTERNAL;

/* SPEC §6.16: preedit text from the active input method. Payload is
 * u32 text_input_id + i32 cursor_begin + i32 cursor_end + UTF-8 text
 * bytes (no NUL terminator; length derived from payload_len). text
 * may be NULL (sends an empty preedit). NULL-tolerant on conn. */
void isz_send_text_input_preedit(struct isz_conn *conn,
                                 uint32_t text_input_id,
                                 const char *text,
                                 int32_t cursor_begin,
                                 int32_t cursor_end)
    ISZ_INTERNAL;

/* SPEC §6.16: committed text from the active input method. Payload
 * is u32 text_input_id + UTF-8 text bytes. text may be NULL (sends
 * an empty commit). NULL-tolerant on conn. */
void isz_send_text_input_commit(struct isz_conn *conn,
                                uint32_t text_input_id,
                                const char *text)
    ISZ_INTERNAL;

/* SPEC §6.15: an output entered the idle-inhibit-active state.
 * Payload is u32 output_id. */
void isz_send_idle_inhibit_active(struct isz_conn *conn,
                                  uint32_t output_id)
    ISZ_INTERNAL;

/* SPEC §6.15: an output returned to the idle-inhibit-inactive state.
 * Payload is u32 output_id. */
void isz_send_idle_inhibit_inactive(struct isz_conn *conn,
                                    uint32_t output_id)
    ISZ_INTERNAL;

#endif /* ISZ_WIRE_SENDERS_H */
