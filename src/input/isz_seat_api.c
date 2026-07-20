/* SPDX-License-Identifier: MIT
 *
 * Ishizue (礎) - public seat API and internal seat/device helpers.
 * Wave 1-E.
 *
 * Implements the seat-side surface of isz.h (§9): default seat
 * creation, keyboard focus, cursor surface config, and the per-device
 * libinput config knobs. Also exposes the internal helpers the rest
 * of the input subsystem relies on (seat lifecycle, device config
 * application on hotplug).
 *
 * The public functions are always compiled; they call into the
 * server accessor and event emitter provided by sibling waves. The
 * libinput config application is guarded on ISHIZUE_HAVE_LIBINPUT.
 */
#define _POSIX_C_SOURCE 200809L

#include "isz_seat_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../protocol/isz_wire_senders.h"

/* §6.8 selection slot count. Kept as a macro so future slot additions
 * ripple through both the owner array and the timestamp array. */
#define ISZ_SELECTION_SLOT_COUNT 2

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

struct isz_seat *isz_seat_create(isz_server *srv, const char *name) {
    if (!srv || !name)
        return NULL;

    struct isz_input_state *st = isz_server_get_input_state(srv);
    if (!st) {
        st = calloc(1, sizeof(*st));
        if (!st)
            return NULL;
        st->srv = srv;
        st->fd  = -1;
        isz_server_set_input_state(srv, st);
    }

    struct isz_seat *seat = calloc(1, sizeof(*seat));
    if (!seat)
        return NULL;
    seat->srv            = srv;
    seat->cursor_visible = true;
    snprintf(seat->name, sizeof(seat->name), "%s", name);

    seat->next = st->seats_head;
    if (st->seats_head)
        st->seats_head->prev = seat;
    st->seats_head = seat;
    return seat;
}

void isz_seat_destroy(struct isz_seat *seat) {
    if (!seat)
        return;

    struct isz_seat_device *d = seat->devices_head;
    while (d) {
        struct isz_seat_device *n = d->next;
        free(d);
        d = n;
    }

    /* §6.16: tear down any text-input objects still attached to this
     * seat, plus the input-method stub if one was attached. */
    struct isz_text_input *ti = seat->text_inputs_head;
    while (ti) {
        struct isz_text_input *n = ti->next;
        free(ti->surrounding_text);
        free(ti);
        ti = n;
    }
    seat->text_inputs_head = NULL;
    free(seat->input_method);
    seat->input_method = NULL;

#ifdef ISHIZUE_HAVE_XKBCOMMON
    if (seat->xkb_state)
        xkb_state_unref(seat->xkb_state);
    if (seat->xkb_keymap)
        xkb_keymap_unref(seat->xkb_keymap);
    if (seat->xkb_ctx)
        xkb_context_unref(seat->xkb_ctx);
#endif

    struct isz_input_state *st = isz_server_get_input_state(seat->srv);
    if (st) {
        if (seat->prev)
            seat->prev->next = seat->next;
        else
            st->seats_head = seat->next;
        if (seat->next)
            seat->next->prev = seat->prev;
        if (st->default_seat == seat)
            st->default_seat = NULL;
    }
    free(seat);
}

/* Apply the cached config on the underlying libinput device. Called
 * on hotplug (from isz_input.c) and after every setter below, so the
 * device follows whatever the Architect last requested. */
void isz_seat_device_apply_config(struct isz_seat_device *dev) {
    if (!dev)
        return;
#ifdef ISHIZUE_HAVE_LIBINPUT
    struct libinput_device *d = dev->li_dev;
    if (!d)
        return;

    if (libinput_device_config_tap_get_finger_count(d) > 0) {
        libinput_device_config_tap_set_enabled(d,
            dev->tap_enabled ? LIBINPUT_CONFIG_TAP_ENABLED
                             : LIBINPUT_CONFIG_TAP_DISABLED);
        libinput_device_config_tap_set_drag_enabled(d,
            dev->tap_drag_enabled ? LIBINPUT_CONFIG_DRAG_ENABLED
                                  : LIBINPUT_CONFIG_DRAG_DISABLED);
    }
    if (libinput_device_config_scroll_has_natural_scroll(d))
        libinput_device_config_scroll_set_natural_scroll_enabled(d,
            dev->natural_scroll);
    if (libinput_device_config_accel_is_available(d)) {
        enum libinput_config_accel_profile p =
            LIBINPUT_CONFIG_ACCEL_PROFILE_NONE;
        switch (dev->accel_profile) {
        case ISZ_ACCEL_FLAT:
            p = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT; break;
        case ISZ_ACCEL_ADAPTIVE:
            p = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE; break;
        default:
            p = LIBINPUT_CONFIG_ACCEL_PROFILE_NONE; break;
        }
        libinput_device_config_accel_set_profile(d, p);
    }
    if (libinput_device_config_calibration_has_matrix(d))
        libinput_device_config_calibration_set_matrix(d, dev->calibration);
#endif
}

/* ------------------------------------------------------------------ */
/* Public API (isz.h)                                                 */
/* ------------------------------------------------------------------ */

ISZ_API isz_seat *isz_seat_default(isz_server *srv) {
    if (!srv)
        return NULL;
    struct isz_input_state *st = isz_server_get_input_state(srv);
    if (!st) {
        st = calloc(1, sizeof(*st));
        if (!st)
            return NULL;
        st->srv = srv;
        st->fd  = -1;
        isz_server_set_input_state(srv, st);
    }
    if (st->default_seat)
        return st->default_seat;

    struct isz_seat *seat = isz_seat_create(srv, "seat0");
    if (!seat)
        return NULL;
    seat->is_default   = true;
    st->default_seat   = seat;
    return seat;
}

ISZ_API int isz_seat_set_keyboard_focus(isz_seat *seat, isz_surface *surf) {
    if (!seat)
        return ISZ_ERR_INVALID_ARG;
    seat->keyboard_focus = surf;
    isz_event e = { .type = ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED };
    e.u.keyboard_focus.seat    = seat;
    e.u.keyboard_focus.surface = surf;
    isz_server_emit_event(seat->srv, &e);
    return ISZ_OK;
}

ISZ_API int isz_seat_set_cursor_surface(isz_seat *seat, isz_surface *surf) {
    if (!seat)
        return ISZ_ERR_INVALID_ARG;
    seat->cursor_surface = surf;
    return ISZ_OK;
}

ISZ_API int isz_seat_set_cursor_hotspot(isz_seat *seat, uint32_t x, uint32_t y) {
    if (!seat)
        return ISZ_ERR_INVALID_ARG;
    seat->cursor_hotspot_x = x;
    seat->cursor_hotspot_y = y;
    return ISZ_OK;
}

ISZ_API int isz_seat_set_cursor_visible(isz_seat *seat, bool visible) {
    if (!seat)
        return ISZ_ERR_INVALID_ARG;
    seat->cursor_visible = visible;
    return ISZ_OK;
}

ISZ_API int isz_seat_device_set_tap_enabled(isz_seat_device *dev, bool enabled) {
    if (!dev)
        return ISZ_ERR_INVALID_ARG;
    dev->tap_enabled = enabled;
    isz_seat_device_apply_config(dev);
    return ISZ_OK;
}

ISZ_API int isz_seat_device_set_tap_drag_enabled(isz_seat_device *dev,
                                                 bool enabled) {
    if (!dev)
        return ISZ_ERR_INVALID_ARG;
    dev->tap_drag_enabled = enabled;
    isz_seat_device_apply_config(dev);
    return ISZ_OK;
}

ISZ_API int isz_seat_device_set_natural_scroll(isz_seat_device *dev,
                                               bool enabled) {
    if (!dev)
        return ISZ_ERR_INVALID_ARG;
    dev->natural_scroll = enabled;
    isz_seat_device_apply_config(dev);
    return ISZ_OK;
}

ISZ_API int isz_seat_device_set_accel_profile(isz_seat_device *dev,
                                              enum isz_accel_profile profile) {
    if (!dev)
        return ISZ_ERR_INVALID_ARG;
    dev->accel_profile = profile;
    isz_seat_device_apply_config(dev);
    return ISZ_OK;
}

ISZ_API int isz_seat_device_set_calibration(isz_seat_device *dev,
                                            const float matrix[9]) {
    if (!dev || !matrix)
        return ISZ_ERR_INVALID_ARG;
    memcpy(dev->calibration, matrix, sizeof(dev->calibration));
    isz_seat_device_apply_config(dev);
    return ISZ_OK;
}

/* ------------------------------------------------------------------ */
/* §6.8 selections                                                    */
/* ------------------------------------------------------------------ */

ISZ_API int isz_seat_set_selection_owner(isz_seat *seat,
                                          enum isz_selection_slot slot,
                                          isz_surface *owner,
                                          uint64_t timestamp_ns)
{
    if (!seat)
        return ISZ_ERR_INVALID_ARG;
    if (slot < 0 || slot >= ISZ_SELECTION_SLOT_COUNT)
        return ISZ_ERR_INVALID_ARG;

    /* §6.8 stale-claim rejection: a new claim with a timestamp older
     * than the current owner's is rejected. A release (owner == NULL)
     * is always accepted and clears the stored timestamp so a later
     * claim is not compared against a stale value. */
    if (owner != NULL) {
        if (seat->selection_owner[slot] != NULL &&
            timestamp_ns < seat->selection_timestamp_ns[slot])
            return ISZ_ERR_INVALID_ARG;
        seat->selection_owner[slot] = owner;
        seat->selection_timestamp_ns[slot] = timestamp_ns;
    } else {
        seat->selection_owner[slot] = NULL;
        seat->selection_timestamp_ns[slot] = 0;
    }
    return ISZ_OK;
}

ISZ_API isz_surface *isz_seat_get_selection_owner(isz_seat *seat,
                                                   enum isz_selection_slot slot)
{
    if (!seat || slot < 0 || slot >= ISZ_SELECTION_SLOT_COUNT)
        return NULL;
    return seat->selection_owner[slot];
}

/* ------------------------------------------------------------------ */
/* §6.16 text input and input methods                                 */
/* ------------------------------------------------------------------ */

ISZ_API isz_text_input *isz_seat_create_text_input(isz_seat *seat) {
    if (!seat)
        return NULL;
    struct isz_text_input *ti = calloc(1, sizeof(*ti));
    if (!ti)
        return NULL;
    ti->seat = seat;
    /* Link at head of the seat's text-input list. */
    ti->next = seat->text_inputs_head;
    seat->text_inputs_head = ti;
    return ti;
}

ISZ_API isz_input_method *isz_seat_create_input_method(isz_seat *seat) {
    if (!seat)
        return NULL;
    /* v1 stub: one input-method per seat. If a second is created, the
     * first is freed. Real IME routing is post-v1. */
    if (seat->input_method) {
        free(seat->input_method);
        seat->input_method = NULL;
    }
    struct isz_input_method *im = calloc(1, sizeof(*im));
    if (!im)
        return NULL;
    im->seat = seat;
    seat->input_method = im;
    return im;
}

ISZ_API int isz_text_input_set_surrounding_text(isz_text_input *ti,
                                                 const char *text,
                                                 uint32_t cursor,
                                                 uint32_t anchor)
{
    if (!ti)
        return ISZ_ERR_INVALID_ARG;
    char *copy = NULL;
    if (text) {
        copy = strdup(text);
        if (!copy)
            return ISZ_ERR_NO_MEMORY;
    }
    free(ti->surrounding_text);
    ti->surrounding_text = copy;
    ti->surrounding_cursor = cursor;
    ti->surrounding_anchor = anchor;
    return ISZ_OK;
}

ISZ_API int isz_text_input_set_content_type(isz_text_input *ti,
                                             uint32_t hint,
                                             uint32_t purpose)
{
    if (!ti)
        return ISZ_ERR_INVALID_ARG;
    ti->content_hint = hint;
    ti->content_purpose = purpose;
    return ISZ_OK;
}

ISZ_API int isz_text_input_set_cursor_rectangle(isz_text_input *ti,
                                                 int32_t x, int32_t y,
                                                 int32_t w, int32_t h)
{
    if (!ti)
        return ISZ_ERR_INVALID_ARG;
    ti->cursor_rect_x = x;
    ti->cursor_rect_y = y;
    ti->cursor_rect_w = w;
    ti->cursor_rect_h = h;
    return ISZ_OK;
}

ISZ_API int isz_text_input_enable(isz_text_input *ti)
{
    if (!ti)
        return ISZ_ERR_INVALID_ARG;
    ti->enabled = true;
    return ISZ_OK;
}

ISZ_API int isz_text_input_disable(isz_text_input *ti)
{
    if (!ti)
        return ISZ_ERR_INVALID_ARG;
    ti->enabled = false;
    return ISZ_OK;
}

ISZ_API int isz_text_input_commit_string(isz_text_input *ti,
                                          const char *text)
{
    if (!ti)
        return ISZ_ERR_INVALID_ARG;
    /* §6.16: forward the committed text to the client that owns this
     * text-input. v1 has no wire message for text-input creation, so
     * owning_conn is NULL for Architect-created text-inputs and the
     * send is skipped. The IME-side store-and-forward stays the IME's
     * job; the library only routes the bytes. */
    if (ti->owning_conn && ti->object_id != 0)
        isz_send_text_input_commit(ti->owning_conn, ti->object_id, text);
    return ISZ_OK;
}

ISZ_API int isz_text_input_preedit_string(isz_text_input *ti,
                                           const char *text,
                                           int32_t cursor_begin,
                                           int32_t cursor_end)
{
    if (!ti)
        return ISZ_ERR_INVALID_ARG;
    /* §6.16: forward the preedit to the client that owns this
     * text-input. See isz_text_input_commit_string for the
     * owning_conn NULL case. */
    if (ti->owning_conn && ti->object_id != 0)
        isz_send_text_input_preedit(ti->owning_conn, ti->object_id,
                                    text, cursor_begin, cursor_end);
    return ISZ_OK;
}

ISZ_API void isz_text_input_destroy(isz_text_input *ti)
{
    if (!ti)
        return;
    /* Unlink from the seat's text-input list. */
    struct isz_seat *seat = ti->seat;
    if (seat) {
        struct isz_text_input **pp = &seat->text_inputs_head;
        while (*pp) {
            if (*pp == ti) {
                *pp = ti->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }
    free(ti->surrounding_text);
    free(ti);
}

ISZ_API void isz_input_method_destroy(isz_input_method *im)
{
    if (!im)
        return;
    struct isz_seat *seat = im->seat;
    if (seat && seat->input_method == im)
        seat->input_method = NULL;
    free(im);
}
