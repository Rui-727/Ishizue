/* SPDX-License-Identifier: MIT
 *
 * Ishizue (礎) - internal definitions for the input subsystem. Wave 1-E.
 *
 * Public opaque typedefs (isz_seat, isz_seat_device, isz_event) live in
 * include/ishizue/isz.h; this header supplies the concrete struct
 * layouts plus the input subsystem's own state object.
 *
 * The isz_event definition is shared across subsystems: output,
 * client, and session waves all emit events. For Wave 1 it lives here
 * because input is the only producer. When another wave needs it,
 * lift this block into a shared internal header (e.g. src/event.h)
 * without changing the field layout.
 *
 * Sibling-wave contract: these symbols are referenced by this layer
 * and provided elsewhere:
 *
 *   struct isz_input_state *isz_server_get_input_state(isz_server *);
 *   void isz_server_set_input_state(isz_server *,
 *                                   struct isz_input_state *);
 *      Owner: server/lifecycle wave. Stores the input subsystem state
 *      on the server so dispatch can reach it.
 *
 *   void isz_server_emit_event(isz_server *, const isz_event *);
 *      Owner: listener-registry wave. Iterates registered listeners
 *      for ev->type and calls each in registration order (§5).
 *
 * Wave 1-E compiles standalone; linking into libishizue.so requires
 * those waves to be present. Real libinput/libxkbcommon/libseat wiring
 * is gated on ISHIZUE_HAVE_LIBINPUT / _XKBCOMMON / _LIBSEAT, which the
 * Makefile does not define yet; the DRM backend wave turns them on.
 */
#ifndef ISHIZUE_SEAT_INTERNAL_H
#define ISHIZUE_SEAT_INTERNAL_H

#include <ishizue/isz.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ISHIZUE_HAVE_LIBINPUT
#include <libinput.h>
#endif
#ifdef ISHIZUE_HAVE_XKBCOMMON
#include <xkbcommon/xkbcommon.h>
#endif
#ifdef ISHIZUE_HAVE_LIBSEAT
#include <libseat.h>
#endif

#define ISZ_SEAT_NAME_MAX 64

/* ------------------------------------------------------------------ */
/* Logging.                                                           */
/*                                                                    */
/* isz_log_internal is the hidden entry point provided by the util   */
/* wave (src/util/isz_log.c, W1-A). It applies the level filter and  */
/* the user-registered callback; with no callback it is a silent     */
/* no-op (SPEC §12: no built-in stderr). Declared here defensively   */
/* so the input subtree compiles without depending on the util       */
/* header layout; multiple identical declarations are fine in C.     */
/* ------------------------------------------------------------------ */
void isz_log_internal(enum isz_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* ------------------------------------------------------------------ */
/* Event payload (§9).                                                */
/*                                                                    */
/* isz_event is opaque in the public header. Every event carries a   */
/* CLOCK_MONOTONIC_RAW timestamp in nanoseconds. The union is named   */
/* (`u`) so initialisation with designators stays -Wpedantic-clean.   */
/* ------------------------------------------------------------------ */
enum isz_axis_source {
    ISZ_AXIS_SOURCE_WHEEL      = 0,
    ISZ_AXIS_SOURCE_FINGER     = 1,
    ISZ_AXIS_SOURCE_CONTINUOUS = 2,
};

struct isz_event {
    enum isz_event_type type;
    uint64_t time_ns;  /* CLOCK_MONOTONIC_RAW */

    union {
        struct {
            int32_t dx, dy;       /* relative motion */
            double  abs_x, abs_y; /* absolute position when has_abs */
            bool    has_abs;
        } pointer_motion;

        struct {
            uint32_t button;
            bool     pressed;
        } pointer_button;

        struct {
            double dx, dy;
            enum isz_axis_source source;
        } pointer_axis;

        struct {
            uint32_t keycode;  /* Linux evdev keycode */
            bool     pressed;
        } keyboard_key;

        struct {
            uint32_t mods_depressed;
            uint32_t mods_latched;
            uint32_t mods_locked;
            uint32_t group;  /* active layout */
        } keyboard_modifiers;

        struct {
            int32_t id;  /* touch slot */
            double  x, y;
        } touch;

        struct {
            isz_seat        *seat;
            isz_seat_device *device;
        } seat_device;

        struct {
            isz_seat    *seat;
            isz_surface *surface;  /* NULL when focus cleared */
        } keyboard_focus;
    } u;
};

/* ------------------------------------------------------------------ */
/* Per-device state.                                                  */
/* ------------------------------------------------------------------ */
struct isz_seat_device {
    struct isz_seat *seat;        /* owning seat */

#ifdef ISHIZUE_HAVE_LIBINPUT
    struct libinput_device *li_dev;
#endif

    /* Cached config. Applied to the libinput device on set and on
     * hotplug via isz_seat_device_apply_config(). */
    bool                   tap_enabled;
    bool                   tap_drag_enabled;
    bool                   natural_scroll;
    enum isz_accel_profile accel_profile;
    float                  calibration[9];

    struct isz_seat_device *next, *prev;  /* seat's device list */
};

/* ------------------------------------------------------------------ */
/* Per-seat state.                                                    */
/* ------------------------------------------------------------------ */
struct isz_seat {
    struct isz_server *srv;
    char  name[ISZ_SEAT_NAME_MAX];  /* "seat0" etc. */
    bool  is_default;

    /* xkb state, compiled once per layout change (§9). */
#ifdef ISHIZUE_HAVE_XKBCOMMON
    struct xkb_context *xkb_ctx;
    struct xkb_keymap  *xkb_keymap;
    struct xkb_state   *xkb_state;
    /* last serialized modifier/layout state, for change detection */
    uint32_t mods_depressed, mods_latched, mods_locked;
    uint32_t group;
#endif
    char layout[64];   /* last requested layout name */
    char variant[64];  /* last requested variant name */

    /* keyboard focus. §9: the library never reassigns focus itself. */
    isz_surface *keyboard_focus;

    /* tracked pointer position, for absolute→delta conversion. */
    double pointer_x, pointer_y;

    /* cursor surface config. Rendering is the render wave's job. */
    isz_surface *cursor_surface;
    uint32_t     cursor_hotspot_x, cursor_hotspot_y;
    bool         cursor_visible;

    struct isz_seat_device *devices_head;

    /* Reserved hook for per-seat listener lists. The server-level
     * registry (isz_add_listener) is the primary path; per-seat
     * filtering stays the Architect's job. Unused in Wave 1. */
    void *listeners;

    struct isz_seat *next, *prev;  /* input_state seat list */
};

/* ------------------------------------------------------------------ */
/* Input subsystem state, stored on the server.                       */
/*                                                                    */
/* The libinput context is owned here, not per-seat, because libinput */
/* multiplexes all devices on a single fd.                            */
/* ------------------------------------------------------------------ */
struct isz_input_state {
    struct isz_server *srv;
    struct isz_seat   *seats_head;
    struct isz_seat   *default_seat;

#ifdef ISHIZUE_HAVE_LIBINPUT
    struct libinput *li;
#endif
    int fd;  /* libinput fd from libseat; -1 if unset */

#ifdef ISHIZUE_HAVE_LIBSEAT
    struct libseat *session;
    bool            session_active;
#endif
};

/* ------------------------------------------------------------------ */
/* Sibling-wave externs (see file header).                            */
/* ------------------------------------------------------------------ */
struct isz_input_state *isz_server_get_input_state(isz_server *srv);
void                    isz_server_set_input_state(isz_server *srv,
                                                   struct isz_input_state *st);
void                    isz_server_emit_event(isz_server *srv,
                                              const isz_event *ev);

/* ------------------------------------------------------------------ */
/* Internal entry points.                                             */
/* ------------------------------------------------------------------ */

/* isz_input.c */
int  isz_input_init_with_fd(isz_server *srv, int fd);
void isz_input_dispatch(isz_server *srv);
void isz_input_destroy(struct isz_input_state *st);

/* isz_keymap.c */
int  isz_keymap_set_layout(isz_seat *seat, const char *layout,
                           const char *variant);
/* Returns the seat's xkb_state, or NULL when xkbcommon is absent or
 * no layout has been set. Declared as void * so the header compiles
 * without xkbcommon; callers assign to struct xkb_state * directly. */
void *isz_keymap_get_state(isz_seat *seat);
void  isz_keymap_handle_key(isz_seat *seat, uint32_t keycode, bool pressed,
                            uint64_t time_ns, isz_server *srv);

/* isz_session.c */
int  isz_session_init(isz_server *srv);
void isz_session_dispatch(isz_server *srv);

/* isz_seat_api.c (internal helpers) */
struct isz_seat *isz_seat_create(isz_server *srv, const char *name);
void             isz_seat_destroy(struct isz_seat *seat);
void             isz_seat_device_apply_config(struct isz_seat_device *dev);

#endif /* ISHIZUE_SEAT_INTERNAL_H */
