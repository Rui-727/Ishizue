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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* isz_event_accessors.c - public read-side API for isz_event (W5-C).
 *
 * isz_event stays opaque in include/ishizue/isz.h; listeners receive a
 * const pointer and read fields through these accessors instead of
 * pulling in src/input/isz_seat_internal.h. Each accessor validates
 * ev != NULL and ev->type matches the event the accessor is for, then
 * reads from the union. Wrong type or NULL ev yields ISZ_ERR_INVALID_ARG
 * (or NULL for the pointer-returning accessors).
 *
 * The accessors are read-only and run from the listener callback on the
 * main dispatch thread, so they need no internal locking. The union
 * members are written by the event-producing subsystems before
 * isz_server_emit_event fires; by the time a listener sees the event,
 * the payload is frozen.
 *
 * The pointer_axis source value is re-numbered on the way out: the
 * internal isz_axis_source enum orders wheel/finger/continuous as
 * 0/1/2, but the public contract documented in isz.h orders them
 * finger/wheel/continuous as 0/1/2. The mapping is local to this file.
 *
 * For ISZ_EVENT_OUTPUT_ADD / OUTPUT_REMOVE / CLIENT_CONNECT /
 * CLIENT_DISCONNECT / CLIPBOARD_REQUEST the tagged-union payload is
 * not yet populated by the emit sites; the accessors type-check and
 * return NULL so callers can branch on the result without crashing.
 * Wiring the payloads into the emit sites is a separate wave that
 * will not change these accessor signatures. */

#include <ishizue/isz.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input/isz_seat_internal.h"

/* Translate the internal isz_axis_source enum into the public
 * numbering documented in isz.h: 0=finger, 1=wheel, 2=continuous.
 * Unknown values fall through to 0 (finger); they should not occur;
 * the emit sites only ever set the three defined enum values. */
static int isz_axis_source_public(enum isz_axis_source s)
{
    switch (s) {
    case ISZ_AXIS_SOURCE_FINGER:     return 0;
    case ISZ_AXIS_SOURCE_WHEEL:      return 1;
    case ISZ_AXIS_SOURCE_CONTINUOUS: return 2;
    default:                         return 0;
    }
}

/* Helper: assign *out = val only when out is non-NULL. Lets a caller
 * pass NULL for fields it does not care about without tripping the
 * accessors. */
static void isz_set_dx(double *out, double v) { if (out) *out = v; }
static void isz_set_u32(uint32_t *out, uint32_t v) { if (out) *out = v; }
static void isz_set_i32(int32_t *out, int32_t v) { if (out) *out = v; }
static void isz_set_bool(bool *out, bool v) { if (out) *out = v; }
static void isz_set_int(int *out, int v) { if (out) *out = v; }

ISZ_API enum isz_event_type isz_event_get_type(const isz_event *ev)
{
    if (!ev)
        return (enum isz_event_type)0;
    return ev->type;
}

ISZ_API uint64_t isz_event_get_timestamp_ns(const isz_event *ev)
{
    if (!ev)
        return 0;
    return ev->time_ns;
}

ISZ_API int isz_event_get_pointer_motion(const isz_event *ev,
                                          double *dx_out, double *dy_out,
                                          double *abs_x_out,
                                          double *abs_y_out)
{
    if (!ev || ev->type != ISZ_EVENT_INPUT_POINTER_MOTION)
        return ISZ_ERR_INVALID_ARG;
    /* Union stores relative motion as int32_t; the public API widens
     * to double so callers can do float math without a cast. */
    isz_set_dx(dx_out,    (double)ev->u.pointer_motion.dx);
    isz_set_dx(dy_out,    (double)ev->u.pointer_motion.dy);
    isz_set_dx(abs_x_out, ev->u.pointer_motion.abs_x);
    isz_set_dx(abs_y_out, ev->u.pointer_motion.abs_y);
    return ISZ_OK;
}

ISZ_API int isz_event_get_pointer_button(const isz_event *ev,
                                          uint32_t *button_out,
                                          bool *press_out)
{
    if (!ev || ev->type != ISZ_EVENT_INPUT_POINTER_BUTTON)
        return ISZ_ERR_INVALID_ARG;
    isz_set_u32(button_out, ev->u.pointer_button.button);
    isz_set_bool(press_out, ev->u.pointer_button.pressed);
    return ISZ_OK;
}

ISZ_API int isz_event_get_pointer_axis(const isz_event *ev,
                                        double *dx_out, double *dy_out,
                                        int *source_out)
{
    if (!ev || ev->type != ISZ_EVENT_INPUT_POINTER_AXIS)
        return ISZ_ERR_INVALID_ARG;
    isz_set_dx(dx_out, ev->u.pointer_axis.dx);
    isz_set_dx(dy_out, ev->u.pointer_axis.dy);
    isz_set_int(source_out,
                isz_axis_source_public(ev->u.pointer_axis.source));
    return ISZ_OK;
}

ISZ_API int isz_event_get_keyboard_key(const isz_event *ev,
                                        uint32_t *keycode_out,
                                        bool *press_out)
{
    if (!ev || ev->type != ISZ_EVENT_INPUT_KEYBOARD_KEY)
        return ISZ_ERR_INVALID_ARG;
    isz_set_u32(keycode_out, ev->u.keyboard_key.keycode);
    isz_set_bool(press_out, ev->u.keyboard_key.pressed);
    return ISZ_OK;
}

ISZ_API int isz_event_get_keyboard_modifiers(const isz_event *ev,
                                              uint32_t *mods_depressed_out,
                                              uint32_t *mods_latched_out,
                                              uint32_t *mods_locked_out,
                                              uint32_t *group_out)
{
    if (!ev || ev->type != ISZ_EVENT_INPUT_KEYBOARD_MODIFIERS)
        return ISZ_ERR_INVALID_ARG;
    isz_set_u32(mods_depressed_out, ev->u.keyboard_modifiers.mods_depressed);
    isz_set_u32(mods_latched_out,   ev->u.keyboard_modifiers.mods_latched);
    isz_set_u32(mods_locked_out,    ev->u.keyboard_modifiers.mods_locked);
    isz_set_u32(group_out,          ev->u.keyboard_modifiers.group);
    return ISZ_OK;
}

ISZ_API int isz_event_get_touch(const isz_event *ev,
                                 int32_t *touch_id_out,
                                 double *x_out, double *y_out)
{
    if (!ev || (ev->type != ISZ_EVENT_INPUT_TOUCH_DOWN &&
                ev->type != ISZ_EVENT_INPUT_TOUCH_MOTION &&
                ev->type != ISZ_EVENT_INPUT_TOUCH_UP))
        return ISZ_ERR_INVALID_ARG;
    isz_set_i32(touch_id_out, ev->u.touch.id);
    isz_set_dx(x_out, ev->u.touch.x);
    isz_set_dx(y_out, ev->u.touch.y);
    return ISZ_OK;
}

ISZ_API isz_surface *isz_event_get_keyboard_focus(const isz_event *ev)
{
    if (!ev || ev->type != ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED)
        return NULL;
    return ev->u.keyboard_focus.surface;
}

ISZ_API isz_output *isz_event_get_output(const isz_event *ev)
{
    /* OUTPUT_ADD / OUTPUT_REMOVE emit sites zero the union today, so
     * there is no output pointer to return yet. The type check still
     * runs so callers can rely on NULL meaning "wrong type or not
     * wired". When a later wave adds an output field to the union and
     * populates it at emit, this body changes; the signature does not. */
    if (!ev || (ev->type != ISZ_EVENT_OUTPUT_ADD &&
                ev->type != ISZ_EVENT_OUTPUT_REMOVE))
        return NULL;
    return NULL;
}

ISZ_API const char *isz_event_get_client_binary_path(const isz_event *ev)
{
    /* CLIENT_CONNECT / CLIENT_DISCONNECT emit sites zero the union
     * today. Same forward-compatibility note as isz_event_get_output. */
    if (!ev || (ev->type != ISZ_EVENT_CLIENT_CONNECT &&
                ev->type != ISZ_EVENT_CLIENT_DISCONNECT))
        return NULL;
    return NULL;
}

ISZ_API const char *isz_event_get_clipboard_mime_type(const isz_event *ev)
{
    /* No emit site for CLIPBOARD_REQUEST exists yet. Type-check and
     * return NULL so callers can branch safely. */
    if (!ev || ev->type != ISZ_EVENT_CLIPBOARD_REQUEST)
        return NULL;
    return ev->u.clipboard_request.mime_type;
}

ISZ_API uint64_t isz_event_get_clipboard_timestamp(const isz_event *ev)
{
    /* §6.8 selection-ownership timestamp. 0 when no emit site has
     * populated the field yet, or when ev is the wrong type. */
    if (!ev || ev->type != ISZ_EVENT_CLIPBOARD_REQUEST)
        return 0;
    return ev->u.clipboard_request.timestamp_ns;
}

ISZ_API isz_output *isz_event_get_idle_inhibit_output(const isz_event *ev)
{
    /* §6.15: ACTIVE / INACTIVE carry the output whose inhibit count
     * transitioned. NULL on wrong type or NULL ev. */
    if (!ev || (ev->type != ISZ_EVENT_IDLE_INHIBIT_ACTIVE &&
                ev->type != ISZ_EVENT_IDLE_INHIBIT_INACTIVE))
        return NULL;
    return ev->u.idle_inhibit.output;
}

ISZ_API const char *isz_event_get_text_input_preedit(const isz_event *ev,
                                                      int32_t *cursor_begin_out,
                                                      int32_t *cursor_end_out)
{
    /* §6.16: preedit text from the active input method. text is
     * borrowed and valid for the listener callback. */
    if (!ev || ev->type != ISZ_EVENT_TEXT_INPUT_PREEDIT)
        return NULL;
    isz_set_i32(cursor_begin_out, ev->u.text_input_preedit.cursor_begin);
    isz_set_i32(cursor_end_out,   ev->u.text_input_preedit.cursor_end);
    return ev->u.text_input_preedit.text;
}

ISZ_API const char *isz_event_get_text_input_commit(const isz_event *ev)
{
    /* §6.16: committed text from the active input method. */
    if (!ev || ev->type != ISZ_EVENT_TEXT_INPUT_COMMIT)
        return NULL;
    return ev->u.text_input_commit.text;
}

ISZ_API int isz_event_get_text_input_cursor_rectangle(const isz_event *ev,
                                                        int32_t *x_out,
                                                        int32_t *y_out,
                                                        int32_t *w_out,
                                                        int32_t *h_out)
{
    /* §6.16: cursor rectangle for the focused text-input, forwarded
     * back to the IME on _CURSOR_RECTANGLE_NEEDED. */
    if (!ev || ev->type != ISZ_EVENT_TEXT_INPUT_CURSOR_RECTANGLE_NEEDED)
        return ISZ_ERR_INVALID_ARG;
    isz_set_i32(x_out, ev->u.text_input_cursor_rectangle.x);
    isz_set_i32(y_out, ev->u.text_input_cursor_rectangle.y);
    isz_set_i32(w_out, ev->u.text_input_cursor_rectangle.w);
    isz_set_i32(h_out, ev->u.text_input_cursor_rectangle.h);
    return ISZ_OK;
}
