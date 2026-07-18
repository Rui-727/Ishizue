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
#include "isz_seat_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
