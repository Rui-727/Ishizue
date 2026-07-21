/* SPDX-License-Identifier: MIT
 *
 * Ishizue (礎) - libinput integration and event dispatch. Wave 1-E.
 *
 * §9: libinput_next_event() runs directly in the main dispatch loop.
 * There is no internal queue; each translated event is pushed straight
 * to the server's listener registry via isz_server_emit_event().
 *
 * Timestamps: §9 calls for CLOCK_MONOTONIC_RAW. libinput exposes only
 * CLOCK_MONOTONIC via libinput_event_get_time(); we pass it through
 * for Wave 1 (see TODO in ev_time). The raw clock has to be sampled
 * separately when the DRM backend wave lands.
 *
 * Without ISHIZUE_HAVE_LIBINPUT every entry point is a no-op stub.
 */

/* open(2)/close(2)/O_CLOEXEC are POSIX, not ISO C; the build is
 * -std=c11 strict, so enable the POSIX feature set for this TU. */
#define _POSIX_C_SOURCE 200809L

#include "isz_seat_internal.h"

#include <stdlib.h>
#include <string.h>

#ifndef ISHIZUE_HAVE_LIBINPUT

int isz_input_init_with_fd(isz_server *srv, int fd) {
    (void)srv; (void)fd;
    isz_log_internal(ISZ_LOG_DEBUG, "input_init_with_fd: libinput not built in");
    return ISZ_ERR_FEATURE_UNAVAIL;
}

void isz_input_dispatch(isz_server *srv) {
    (void)srv;
}

void isz_input_destroy(struct isz_input_state *st) {
    free(st);
}

#else  /* ISHIZUE_HAVE_LIBINPUT */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* libinput context setup                                             */
/* ------------------------------------------------------------------ */

static void log_libinput(struct libinput *li,
                         enum libinput_log_priority pri,
                         const char *fmt, va_list args) {
    (void)li;
    enum isz_log_level lvl;
    switch (pri) {
    case LIBINPUT_LOG_PRIORITY_DEBUG: lvl = ISZ_LOG_DEBUG; break;
    case LIBINPUT_LOG_PRIORITY_INFO:  lvl = ISZ_LOG_INFO;  break;
    default:                          lvl = ISZ_LOG_ERROR; break;
    }
    /* isz_log_internal takes a format string, not a va_list, so format
     * here first. §12: no built-in stderr; everything routes through
     * the user callback (silent when none is registered). */
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    isz_log_internal(lvl, "%s", buf);
}

/* libinput asks the application to open/close device nodes. With
 * libseat in the loop these are usually pre-opened, but the interface
 * is mandatory. The simple open/close here is refined when the DRM
 * backend wave wires libseat device fds. */
static int open_restricted(const char *path, int flags, void *userdata) {
    (void)userdata;
    return open(path, flags | O_CLOEXEC);
}

static void close_restricted(int fd, void *userdata) {
    (void)userdata;
    close(fd);
}

static const struct libinput_interface li_iface = {
    .open_restricted  = open_restricted,
    .close_restricted = close_restricted,
};

int isz_input_init_with_fd(isz_server *srv, int fd) {
    /* libinput has no API to wrap an already-open fd. The real
     * integration uses libseat's open_restricted callback to hand
     * fds to libinput via libinput_path_create_context +
     * libinput_path_add_device(path). That refactor belongs in the
     * DRM-backend wave. For now this function accepts the fd (so
     * the headless backend's stub path compiles) but does not
     * actually wire it into libinput. The DRM backend's real
     * init path will create the libinput context itself. */
    if (!srv || fd < 0)
        return ISZ_ERR_INVALID_ARG;

    struct isz_input_state *st = isz_server_get_input_state(srv);
    if (!st) {
        st = calloc(1, sizeof(*st));
        if (!st)
            return ISZ_ERR_NO_MEMORY;
        st->srv = srv;
        st->fd  = -1;
        isz_server_set_input_state(srv, st);
    }
    if (st->li) {
        libinput_unref(st->li);
        st->li = NULL;
    }

    st->fd = fd;
    st->li = libinput_path_create_context(&li_iface, NULL);
    if (!st->li) {
        isz_log_internal(ISZ_LOG_DEBUG, "libinput_path_create_context failed");
        return ISZ_ERR_FEATURE_UNAVAIL;
    }
    libinput_log_set_handler(st->li, log_libinput);
    libinput_log_set_priority(st->li, LIBINPUT_LOG_PRIORITY_INFO);
    return ISZ_OK;
}

void isz_input_destroy(struct isz_input_state *st) {
    if (!st)
        return;
    /* Seats and devices are freed via isz_seat_destroy(); the server
     * shutdown path owns that ordering. We only drop the libinput
     * context here. */
    if (st->li)
        libinput_unref(st->li);
    free(st);
}

/* ------------------------------------------------------------------ */
/* Event translation                                                  */
/* ------------------------------------------------------------------ */

static uint64_t ev_time(struct libinput_event *ev) {
    /* libinput exposes time per event type, not on the base event.
     * Dispatch on the type. Returns microseconds; caller multiplies
     * by 1000 to get nanoseconds. TODO: SPEC §9 wants
     * CLOCK_MONOTONIC_RAW. libinput only exposes CLOCK_MONOTONIC.
     * The DRM backend wave should sample CLOCK_MONOTONIC_RAW at
     * dispatch entry and stamp events from that. */
    enum libinput_event_type t = libinput_event_get_type(ev);
    uint32_t us = 0;
    switch (t) {
    case LIBINPUT_EVENT_KEYBOARD_KEY:
        us = libinput_event_keyboard_get_time(
            libinput_event_get_keyboard_event(ev));
        break;
    case LIBINPUT_EVENT_POINTER_MOTION:
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
    case LIBINPUT_EVENT_POINTER_BUTTON:
    case LIBINPUT_EVENT_POINTER_AXIS:
        us = libinput_event_pointer_get_time(
            libinput_event_get_pointer_event(ev));
        break;
    case LIBINPUT_EVENT_TOUCH_DOWN:
    case LIBINPUT_EVENT_TOUCH_UP:
    case LIBINPUT_EVENT_TOUCH_MOTION:
    case LIBINPUT_EVENT_TOUCH_FRAME:
        us = libinput_event_touch_get_time(
            libinput_event_get_touch_event(ev));
        break;
    default:
        us = 0;
        break;
    }
    return (uint64_t)us * 1000u;
}

static void emit(isz_server *srv, const isz_event *e) {
    isz_server_emit_event(srv, e);
}

static void handle_pointer_motion(isz_server *srv, isz_seat *seat,
                                  struct libinput_event_pointer *p) {
    isz_event e = { .type = ISZ_EVENT_INPUT_POINTER_MOTION };
    e.time_ns = ev_time((struct libinput_event *)p);
    e.u.pointer_motion.dx = (int32_t)libinput_event_pointer_get_dx(p);
    e.u.pointer_motion.dy = (int32_t)libinput_event_pointer_get_dy(p);
    e.u.pointer_motion.has_abs = false;
    e.u.pointer_motion.abs_x   = 0.0;
    e.u.pointer_motion.abs_y   = 0.0;
    seat->pointer_x += e.u.pointer_motion.dx;
    seat->pointer_y += e.u.pointer_motion.dy;
    emit(srv, &e);
}

static void handle_pointer_motion_abs(isz_server *srv, isz_seat *seat,
                                      struct libinput_event_pointer *p) {
    isz_event e = { .type = ISZ_EVENT_INPUT_POINTER_MOTION };
    e.time_ns = ev_time((struct libinput_event *)p);
    double x = libinput_event_pointer_get_absolute_x(p);
    double y = libinput_event_pointer_get_absolute_y(p);
    e.u.pointer_motion.has_abs = true;
    e.u.pointer_motion.abs_x   = x;
    e.u.pointer_motion.abs_y   = y;
    e.u.pointer_motion.dx = (int32_t)(x - seat->pointer_x);
    e.u.pointer_motion.dy = (int32_t)(y - seat->pointer_y);
    seat->pointer_x = x;
    seat->pointer_y = y;
    emit(srv, &e);
}

static void handle_pointer_button(isz_server *srv,
                                  struct libinput_event_pointer *p) {
    isz_event e = { .type = ISZ_EVENT_INPUT_POINTER_BUTTON };
    e.time_ns = ev_time((struct libinput_event *)p);
    e.u.pointer_button.button = libinput_event_pointer_get_button(p);
    e.u.pointer_button.pressed =
        libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED;
    emit(srv, &e);
}

static void handle_pointer_axis(isz_server *srv,
                                struct libinput_event_pointer *p) {
    isz_event e = { .type = ISZ_EVENT_INPUT_POINTER_AXIS };
    e.time_ns = ev_time((struct libinput_event *)p);
    e.u.pointer_axis.dx = 0.0;
    e.u.pointer_axis.dy = 0.0;
    if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
        e.u.pointer_axis.dy = libinput_event_pointer_get_axis_value(
            p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
    if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
        e.u.pointer_axis.dx = libinput_event_pointer_get_axis_value(
            p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
    switch (libinput_event_pointer_get_axis_source(p)) {
    case LIBINPUT_POINTER_AXIS_SOURCE_WHEEL:
        e.u.pointer_axis.source = ISZ_AXIS_SOURCE_WHEEL; break;
    case LIBINPUT_POINTER_AXIS_SOURCE_FINGER:
        e.u.pointer_axis.source = ISZ_AXIS_SOURCE_FINGER; break;
    default:
        e.u.pointer_axis.source = ISZ_AXIS_SOURCE_CONTINUOUS; break;
    }
    emit(srv, &e);
}

static void handle_keyboard_key(isz_server *srv, isz_seat *seat,
                                struct libinput_event_keyboard *k) {
    isz_event e = { .type = ISZ_EVENT_INPUT_KEYBOARD_KEY };
    e.time_ns = ev_time((struct libinput_event *)k);
    e.u.keyboard_key.keycode = libinput_event_keyboard_get_key(k);
    e.u.keyboard_key.pressed =
        libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED;
    emit(srv, &e);

    /* Feed xkb and emit a modifiers event if state changed (§9). */
    isz_keymap_handle_key(seat, e.u.keyboard_key.keycode,
                          e.u.keyboard_key.pressed, e.time_ns, srv);
}

static void handle_touch(isz_server *srv, enum isz_event_type t,
                         struct libinput_event_touch *tch) {
    isz_event e = { .type = t };
    e.time_ns = ev_time((struct libinput_event *)tch);
    e.u.touch.id = (int32_t)libinput_event_touch_get_slot(tch);
    if (t == ISZ_EVENT_INPUT_TOUCH_UP) {
        e.u.touch.x = 0.0;
        e.u.touch.y = 0.0;
    } else {
        e.u.touch.x = libinput_event_touch_get_x(tch);
        e.u.touch.y = libinput_event_touch_get_y(tch);
    }
    emit(srv, &e);
}

/* ------------------------------------------------------------------ */
/* Hotplug                                                            */
/* ------------------------------------------------------------------ */

static isz_seat *seat_for_libinput(isz_server *srv,
                                   struct libinput_device *d) {
    struct isz_input_state *st = isz_server_get_input_state(srv);
    if (!st)
        return NULL;
    /* libinput exposes the seat name via the seat object, not the
     * device. Use the physical seat name (e.g. "seat0"). */
    struct libinput_seat *seat = libinput_device_get_seat(d);
    const char *name = seat ? libinput_seat_get_physical_name(seat) : NULL;
    if (!name)
        name = "seat0";
    for (struct isz_seat *s = st->seats_head; s; s = s->next) {
        if (strncmp(s->name, name, ISZ_SEAT_NAME_MAX) == 0)
            return s;
    }
    return isz_seat_create(srv, name);
}

static void handle_device_added(isz_server *srv,
                                struct libinput_event *ev) {
    struct libinput_device *d = libinput_event_get_device(ev);
    isz_seat *seat = seat_for_libinput(srv, d);
    if (!seat)
        return;

    struct isz_seat_device *sd = calloc(1, sizeof(*sd));
    if (!sd)
        return;
    sd->seat          = seat;
    sd->li_dev        = d;
    sd->accel_profile = ISZ_ACCEL_NONE;
    libinput_device_set_user_data(d, sd);

    sd->next = seat->devices_head;
    if (seat->devices_head)
        seat->devices_head->prev = sd;
    seat->devices_head = sd;

    isz_seat_device_apply_config(sd);

    isz_event e = { .type = ISZ_EVENT_SEAT_ADD };
    e.u.seat_device.seat   = seat;
    e.u.seat_device.device = sd;
    emit(srv, &e);
}

static void handle_device_removed(isz_server *srv,
                                  struct libinput_event *ev) {
    struct libinput_device *d = libinput_event_get_device(ev);
    struct isz_seat_device *sd = libinput_device_get_user_data(d);
    if (!sd)
        return;

    isz_event e = { .type = ISZ_EVENT_SEAT_REMOVE };
    e.u.seat_device.seat   = sd->seat;
    e.u.seat_device.device = sd;
    emit(srv, &e);

    struct isz_seat *seat = sd->seat;
    if (sd->prev)
        sd->prev->next = sd->next;
    else
        seat->devices_head = sd->next;
    if (sd->next)
        sd->next->prev = sd->prev;
    free(sd);
    libinput_device_set_user_data(d, NULL);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

void isz_input_dispatch(isz_server *srv) {
    struct isz_input_state *st = isz_server_get_input_state(srv);
    if (!st || !st->li)
        return;

    libinput_dispatch(st->li);

    struct libinput_event *ev;
    while ((ev = libinput_get_event(st->li)) != NULL) {
        enum libinput_event_type t = libinput_event_get_type(ev);

        /* Resolve the seat. Every input event originates from a
         * device that carries our seat_device user-data; device
         * add/remove set up or tear down that mapping themselves. */
        struct libinput_device *d = libinput_event_get_device(ev);
        struct isz_seat_device *sd =
            d ? libinput_device_get_user_data(d) : NULL;
        isz_seat *seat = sd ? sd->seat : NULL;
        if (!seat && st->default_seat)
            seat = st->default_seat;

        switch (t) {
        case LIBINPUT_EVENT_POINTER_MOTION:
            if (seat)
                handle_pointer_motion(srv, seat,
                    libinput_event_get_pointer_event(ev));
            break;
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
            if (seat)
                handle_pointer_motion_abs(srv, seat,
                    libinput_event_get_pointer_event(ev));
            break;
        case LIBINPUT_EVENT_POINTER_BUTTON:
            handle_pointer_button(srv,
                libinput_event_get_pointer_event(ev));
            break;
        case LIBINPUT_EVENT_POINTER_AXIS:
            handle_pointer_axis(srv,
                libinput_event_get_pointer_event(ev));
            break;
        case LIBINPUT_EVENT_KEYBOARD_KEY:
            if (seat)
                handle_keyboard_key(srv, seat,
                    libinput_event_get_keyboard_event(ev));
            break;
        case LIBINPUT_EVENT_TOUCH_DOWN:
            handle_touch(srv, ISZ_EVENT_INPUT_TOUCH_DOWN,
                libinput_event_get_touch_event(ev));
            break;
        case LIBINPUT_EVENT_TOUCH_MOTION:
            handle_touch(srv, ISZ_EVENT_INPUT_TOUCH_MOTION,
                libinput_event_get_touch_event(ev));
            break;
        case LIBINPUT_EVENT_TOUCH_UP:
            handle_touch(srv, ISZ_EVENT_INPUT_TOUCH_UP,
                libinput_event_get_touch_event(ev));
            break;
        case LIBINPUT_EVENT_TOUCH_FRAME: {
            isz_event e = { .type = ISZ_EVENT_INPUT_TOUCH_FRAME };
            e.time_ns = ev_time(ev);
            emit(srv, &e);
            break;
        }
        case LIBINPUT_EVENT_DEVICE_ADDED:
            handle_device_added(srv, ev);
            break;
        case LIBINPUT_EVENT_DEVICE_REMOVED:
            handle_device_removed(srv, ev);
            break;
        default:
            break;
        }

        libinput_event_destroy(ev);
    }
}

#endif  /* ISHIZUE_HAVE_LIBINPUT */
