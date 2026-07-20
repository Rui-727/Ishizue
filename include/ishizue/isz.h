#ifndef ISHIZUE_H
#define ISHIZUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ishizue/version.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public symbols are default-visible; internal ones are hidden via
 * __attribute__((visibility("hidden"))) in the .c files. The linker
 * version script (libishizue.map) keeps only isz_* exported. */
#define ISZ_API __attribute__((visibility("default")))

/* ------------------------------------------------------------------ */
/* Error codes - §7.10                                                */
/* ------------------------------------------------------------------ */
typedef enum {
    ISZ_OK                    = 0,
    ISZ_ERR_COMMIT_FAILED     = -1,
    ISZ_ERR_COMMIT_PENDING    = -2,
    ISZ_ERR_RESOURCE_LIMIT    = -3,
    ISZ_ERR_SURFACE_NO_PLANE_SLOT = -4,
    ISZ_ERR_PLANE_UNAVAIL     = -5,
    ISZ_ERR_TRANSFORM_UNSUPPORTED = -6,
    ISZ_ERR_OUTPUT_DISCONNECTED = -7,
    ISZ_ERR_INVALID_DMABUF    = -8,
    ISZ_ERR_CLIENT_DISCONNECTED = -9,
    ISZ_ERR_INVALID_ARG       = -10,
    ISZ_ERR_FEATURE_UNAVAIL   = -11,
    ISZ_ERR_NO_MEMORY         = -12,
    ISZ_ERR_DRM_MASTER        = -13,
    ISZ_ERR_CLIENT_TOO_SLOW   = -14,
    ISZ_ERR_ACCESS_DENIED     = -15,
} isz_error;

const char *isz_strerror(int err) ISZ_API;

/* ------------------------------------------------------------------ */
/* Logging - §12                                                      */
/* ------------------------------------------------------------------ */
enum isz_log_level {
    ISZ_LOG_ERROR = 0,
    ISZ_LOG_WARN  = 1,
    ISZ_LOG_INFO  = 2,
    ISZ_LOG_DEBUG = 3,
};

typedef void (*isz_log_fn)(void *userdata, enum isz_log_level level,
                           const char *msg);

void isz_set_log_callback(isz_log_fn fn, void *userdata) ISZ_API;

/* ------------------------------------------------------------------ */
/* Opaque object handles                                              */
/* ------------------------------------------------------------------ */
typedef struct isz_server        isz_server;
typedef struct isz_output        isz_output;
typedef struct isz_surface       isz_surface;
typedef struct isz_seat          isz_seat;
typedef struct isz_seat_device   isz_seat_device;
typedef struct isz_mode          isz_mode;
typedef struct isz_buffer_desc   isz_buffer_desc;
typedef struct isz_event         isz_event;
typedef struct isz_hdr_metadata  isz_hdr_metadata;
typedef struct isz_rect          isz_rect;
typedef struct isz_plane_slot_info isz_plane_slot_info;
typedef struct isz_text_input    isz_text_input;
typedef struct isz_input_method  isz_input_method;

/* ------------------------------------------------------------------ */
/* Backend selection - §10                                            */
/* ------------------------------------------------------------------ */
enum isz_backend_type {
    ISZ_BACKEND_DRM       = 0,
    ISZ_BACKEND_HEADLESS  = 1,
    ISZ_BACKEND_NESTED    = 2,  /* post-v1, API fixed now */
};

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_rate; /* mHz */
} isz_headless_config;

typedef struct {
    void *parent_window_handle; /* platform-specific */
} isz_nested_config;

/* ------------------------------------------------------------------ */
/* Lifecycle - §5, §7.6                                               */
/* ------------------------------------------------------------------ */
isz_server *isz_init(enum isz_backend_type backend,
                     void *backend_config) ISZ_API;
void        isz_dispatch(isz_server *srv) ISZ_API;
int         isz_get_fds(isz_server *srv, int *fds, size_t max) ISZ_API;
void        isz_destroy(isz_server *srv) ISZ_API;

/* §6.1: attach a listening Unix domain socket. The Architect creates and
 * binds the socket, calls listen(2), then hands the fd to the library.
 * The library owns accept/handshake/allowlist/dispatch from here on. */
int isz_listen(isz_server *srv, int listen_fd) ISZ_API;

/* ------------------------------------------------------------------ */
/* Client allowlist - §6.3                                            */
/* ------------------------------------------------------------------ */
int isz_allowlist_add_binary(isz_server *srv, const char *path) ISZ_API;
int isz_allowlist_add_cgroup(isz_server *srv, const char *cgroup_path) ISZ_API;

/* ------------------------------------------------------------------ */
/* Outputs - §7.6, §10                                                */
/* ------------------------------------------------------------------ */
isz_output **isz_output_list(isz_server *srv, size_t *count) ISZ_API;
isz_mode   **isz_output_get_modes(isz_output *out, size_t *count) ISZ_API;
int          isz_output_enable(isz_output *out, isz_mode *mode) ISZ_API;
int          isz_output_disable(isz_output *out) ISZ_API;
void         isz_output_destroy(isz_output *out) ISZ_API;

enum isz_dpms_state {
    ISZ_DPMS_ON      = 0,
    ISZ_DPMS_STANDBY = 1,
    ISZ_DPMS_SUSPEND = 2,
    ISZ_DPMS_OFF     = 3,
};
int isz_output_set_dpms(isz_output *out, enum isz_dpms_state state) ISZ_API;

const uint8_t *isz_output_get_edid(isz_output *out, size_t *size) ISZ_API;

/* Color management - §7.2 */
int isz_output_set_gamma(isz_output *out, const uint16_t *r,
                         const uint16_t *g, const uint16_t *b,
                         size_t size) ISZ_API;
int isz_output_set_degamma(isz_output *out, const uint16_t *r,
                           const uint16_t *g, const uint16_t *b,
                           size_t size) ISZ_API;
int isz_output_set_ctm(isz_output *out, const float matrix[9]) ISZ_API;
int isz_output_set_hdr_metadata(isz_output *out,
                                const isz_hdr_metadata *meta) ISZ_API;

/* Plane slots - §7.7 */
size_t isz_output_get_plane_slots(isz_output *out,
                                  isz_plane_slot_info *out_slots,
                                  size_t max) ISZ_API;

/* ------------------------------------------------------------------ */
/* Surfaces - §7.6, §6.6, §6.7                                        */
/* ------------------------------------------------------------------ */
isz_surface *isz_surface_create(isz_server *srv) ISZ_API;
void         isz_surface_destroy(isz_surface *surf) ISZ_API;

/* SPEC §6.4 surface serial: 64-bit, monotonic, global to the server
 * lifetime, never reused. Returns 0 only for a NULL surface. */
uint64_t     isz_surface_get_serial(isz_surface *surf) ISZ_API;

int isz_surface_attach_buffer(isz_surface *surf, int dmabuf_fd,
                              isz_buffer_desc *desc) ISZ_API;
int isz_surface_detach_buffer(isz_surface *surf) ISZ_API;
int isz_surface_damage(isz_surface *surf, isz_rect *rects, size_t count) ISZ_API;

int isz_surface_set_output(isz_surface *surf, isz_output *out) ISZ_API;
int isz_surface_clear_output(isz_surface *surf) ISZ_API;
int isz_surface_set_position(isz_surface *surf, int x, int y) ISZ_API;
int isz_surface_set_size(isz_surface *surf, int width, int height) ISZ_API;
int isz_surface_set_plane_type(isz_surface *surf, int type) ISZ_API;
int isz_surface_set_plane_slot(isz_surface *surf, int slot) ISZ_API;
int isz_surface_set_zpos(isz_surface *surf, int zpos) ISZ_API;

enum isz_plane_type {
    ISZ_PLANE_PRIMARY = 0,
    ISZ_PLANE_OVERLAY = 1,
    ISZ_PLANE_CURSOR  = 2,
};

enum isz_transform {
    ISZ_TRANSFORM_NORMAL   = 0,
    ISZ_TRANSFORM_ROTATE_90 = 1,
    ISZ_TRANSFORM_ROTATE_180 = 2,
    ISZ_TRANSFORM_ROTATE_270 = 3,
    ISZ_TRANSFORM_REFLECT_X = 4,
    ISZ_TRANSFORM_REFLECT_Y = 5,
};
int isz_surface_set_transform(isz_surface *surf, enum isz_transform t) ISZ_API;

/* SPEC §7.2 fractional scale: stored (numerator, denominator). The
 * library forwards the preferred scale to the owning client via a
 * ISZ_MSG_SURFACE_PREFERRED_SCALE event so the client renders at the
 * right resolution. The library does not composite or rescale.
 * denominator == 0 is rejected as ISZ_ERR_INVALID_ARG. */
int isz_surface_set_scale(isz_surface *surf, uint32_t numerator,
                           uint32_t denominator) ISZ_API;

/* SPEC §6.15 idle inhibit: per-surface flag. When the count of
 * inhibited surfaces on a given output transitions between zero and
 * non-zero, the library emits ISZ_EVENT_IDLE_INHIBIT_ACTIVE /
 * ISZ_EVENT_IDLE_INHIBIT_INACTIVE for that output. The library does
 * not touch any idle timer, screensaver, or DPMS state. */
int isz_surface_set_idle_inhibit(isz_surface *surf, bool inhibit) ISZ_API;

/* SPEC §6.17 surface roles: an optional role attached at creation
 * time. role_handle is the X11 window XID for the X11 roles and 0 for
 * NORMAL. Setting an X11 role is gated by the §6.3 allowlist on the
 * wire side; the library does not enforce that here. */
enum isz_surface_role {
    ISZ_SURFACE_ROLE_NORMAL      = 0,
    ISZ_SURFACE_ROLE_X11_TOPLEVEL = 1,
    ISZ_SURFACE_ROLE_X11_POPUP   = 2,
    ISZ_SURFACE_ROLE_LAYER       = 3,
};
int                   isz_surface_set_role(isz_surface *surf,
                                            enum isz_surface_role role,
                                            uint64_t role_handle) ISZ_API;
enum isz_surface_role isz_surface_get_role(isz_surface *surf) ISZ_API;
uint64_t              isz_surface_get_role_handle(isz_surface *surf) ISZ_API;

/* Subsurfaces - §6.6 */
isz_surface *isz_surface_create_subsurface(isz_surface *parent) ISZ_API;
#define ISZ_SUBSURFACE_DESYNC 1u
int isz_surface_set_subsurface_flags(isz_surface *sub, uint32_t flags) ISZ_API;

/* Popups - §6.7 */
isz_surface *isz_surface_create_popup(isz_surface *parent,
                                       int x, int y) ISZ_API;

/* Layer-shell - §6.7 */
enum isz_layer {
    ISZ_LAYER_OVERLAY = 0,
    ISZ_LAYER_BOTTOM  = 1,
    ISZ_LAYER_TOP     = 2,
    ISZ_LAYER_LOCK    = 3,
};
isz_surface *isz_surface_create_layer(isz_output *out,
                                       enum isz_layer layer) ISZ_API;

/* ------------------------------------------------------------------ */
/* Buffer descriptor - §8                                             */
/* ------------------------------------------------------------------ */
enum isz_alpha_mode {
    ISZ_ALPHA_NONE             = 0,
    ISZ_ALPHA_PREMULTIPLIED    = 1,
    ISZ_ALPHA_NON_PREMULTIPLIED = 2,
};

struct isz_buffer_desc {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t offset;
    uint32_t format;     /* DRM_FORMAT_* fourcc */
    uint64_t modifier;   /* DRM_FORMAT_MOD_* or DRM_FORMAT_MOD_INVALID */
    uint8_t  alpha_mode; /* isz_alpha_mode */
};

/* ------------------------------------------------------------------ */
/* Composition target - §7.7                                          */
/* ------------------------------------------------------------------ */
int isz_composition_target_create(isz_server *srv, uint32_t width,
                                   uint32_t height, uint32_t format,
                                   int *dmabuf_fd_out) ISZ_API;
int isz_composition_target_get_egl_image(int dmabuf_fd,
                                          isz_buffer_desc *desc,
                                          void **egl_image_out) ISZ_API;

/* ------------------------------------------------------------------ */
/* Seat / input - §9                                                  */
/* ------------------------------------------------------------------ */
isz_seat *isz_seat_default(isz_server *srv) ISZ_API;

int isz_seat_set_keyboard_focus(isz_seat *seat, isz_surface *surf) ISZ_API;

int isz_seat_set_cursor_surface(isz_seat *seat, isz_surface *surf) ISZ_API;
int isz_seat_set_cursor_hotspot(isz_seat *seat, uint32_t x, uint32_t y) ISZ_API;
int isz_seat_set_cursor_visible(isz_seat *seat, bool visible) ISZ_API;

enum isz_accel_profile {
    ISZ_ACCEL_NONE    = 0,
    ISZ_ACCEL_FLAT    = 1,
    ISZ_ACCEL_ADAPTIVE = 2,
};

int isz_seat_device_set_tap_enabled(isz_seat_device *dev, bool enabled) ISZ_API;
int isz_seat_device_set_tap_drag_enabled(isz_seat_device *dev, bool enabled) ISZ_API;
int isz_seat_device_set_natural_scroll(isz_seat_device *dev, bool enabled) ISZ_API;
int isz_seat_device_set_accel_profile(isz_seat_device *dev,
                                       enum isz_accel_profile profile) ISZ_API;
int isz_seat_device_set_calibration(isz_seat_device *dev,
                                     const float matrix[9]) ISZ_API;

/* SPEC §6.8 selections: two slots per seat, PRIMARY (mouse-select)
 * and CLIPBOARD (Ctrl-C). Each ownership change carries a
 * CLOCK_MONOTONIC_RAW timestamp in nanoseconds; the library uses it
 * to break ties when claims arrive in quick succession, latest wins.
 * Stale claims (timestamp older than the current owner's) are
 * rejected with ISZ_ERR_INVALID_ARG. */
enum isz_selection_slot {
    ISZ_SELECTION_PRIMARY   = 0,
    ISZ_SELECTION_CLIPBOARD = 1,
};
int          isz_seat_set_selection_owner(isz_seat *seat,
                                           enum isz_selection_slot slot,
                                           isz_surface *owner,
                                           uint64_t timestamp_ns) ISZ_API;
isz_surface *isz_seat_get_selection_owner(isz_seat *seat,
                                           enum isz_selection_slot slot) ISZ_API;

/* SPEC §6.16 text input and input methods. Per-seat text-input object
 * on the client side and per-seat input-method object on the IME side.
 * v1 stores the state on the text-input struct; real IME routing is
 * post-v1. */
isz_text_input *isz_seat_create_text_input(isz_seat *seat) ISZ_API;
isz_input_method *isz_seat_create_input_method(isz_seat *seat) ISZ_API;

int  isz_text_input_set_surrounding_text(isz_text_input *ti,
                                          const char *text,
                                          uint32_t cursor,
                                          uint32_t anchor) ISZ_API;
int  isz_text_input_set_content_type(isz_text_input *ti,
                                      uint32_t hint,
                                      uint32_t purpose) ISZ_API;
int  isz_text_input_set_cursor_rectangle(isz_text_input *ti,
                                          int32_t x, int32_t y,
                                          int32_t w, int32_t h) ISZ_API;
int  isz_text_input_enable(isz_text_input *ti) ISZ_API;
int  isz_text_input_disable(isz_text_input *ti) ISZ_API;
int  isz_text_input_commit_string(isz_text_input *ti,
                                   const char *text) ISZ_API;
int  isz_text_input_preedit_string(isz_text_input *ti,
                                    const char *text,
                                    int32_t cursor_begin,
                                    int32_t cursor_end) ISZ_API;
void isz_text_input_destroy(isz_text_input *ti) ISZ_API;

void isz_input_method_destroy(isz_input_method *im) ISZ_API;

/* ------------------------------------------------------------------ */
/* Commit - §7.3                                                      */
/* ------------------------------------------------------------------ */
#define ISZ_COMMIT_NORMAL    (0u)
#define ISZ_COMMIT_ASYNC     (1u << 0)
#define ISZ_COMMIT_TEST_ONLY (1u << 1)

int isz_commit(isz_output *out, uint32_t flags) ISZ_API;

/* ------------------------------------------------------------------ */
/* Thread pool - §5                                                   */
/* ------------------------------------------------------------------ */
typedef void (*isz_work_fn)(void *ctx);
/* Returns a fence fd the Architect polls; closes after work completes.
 * Returns -1 on failure. */
int isz_thread_pool_submit(isz_server *srv, isz_work_fn fn, void *ctx) ISZ_API;

/* ------------------------------------------------------------------ */
/* Events - §5, §9                                                    */
/* ------------------------------------------------------------------ */
enum isz_event_type {
    ISZ_EVENT_INPUT_POINTER_MOTION = 1,
    ISZ_EVENT_INPUT_POINTER_BUTTON = 2,
    ISZ_EVENT_INPUT_POINTER_AXIS   = 3,
    ISZ_EVENT_INPUT_KEYBOARD_KEY   = 4,
    ISZ_EVENT_INPUT_KEYBOARD_MODIFIERS = 5,
    ISZ_EVENT_INPUT_TOUCH_DOWN     = 6,
    ISZ_EVENT_INPUT_TOUCH_MOTION   = 7,
    ISZ_EVENT_INPUT_TOUCH_UP       = 8,
    ISZ_EVENT_INPUT_TOUCH_FRAME    = 9,
    ISZ_EVENT_SEAT_ADD             = 10,
    ISZ_EVENT_SEAT_REMOVE          = 11,
    ISZ_EVENT_SESSION_ACTIVE       = 12,
    ISZ_EVENT_SESSION_INACTIVE     = 13,
    ISZ_EVENT_OUTPUT_ADD           = 14,
    ISZ_EVENT_OUTPUT_REMOVE        = 15,
    ISZ_EVENT_CLIENT_CONNECT       = 16,
    ISZ_EVENT_CLIENT_DISCONNECT    = 17,
    ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED = 18,
    ISZ_EVENT_CLIPBOARD_REQUEST    = 19,
    ISZ_EVENT_IDLE_INHIBIT_ACTIVE   = 20,  /* §6.15 */
    ISZ_EVENT_IDLE_INHIBIT_INACTIVE = 21,  /* §6.15 */
    ISZ_EVENT_TEXT_INPUT_PREEDIT    = 22,  /* §6.16 */
    ISZ_EVENT_TEXT_INPUT_COMMIT     = 23,  /* §6.16 */
    ISZ_EVENT_TEXT_INPUT_CURSOR_RECTANGLE_NEEDED = 24,  /* §6.16 */
};

/* Event accessors - §5, §9. Allow reading isz_event fields without
 * including internal headers. All return by value; pointers returned
 * are valid only for the duration of the listener callback. */

/* Common to all events. */
enum isz_event_type isz_event_get_type(const isz_event *ev) ISZ_API;
uint64_t isz_event_get_timestamp_ns(const isz_event *ev) ISZ_API; /* CLOCK_MONOTONIC_RAW */

/* ISZ_EVENT_INPUT_POINTER_MOTION */
int isz_event_get_pointer_motion(const isz_event *ev,
                                  double *dx_out, double *dy_out,
                                  double *abs_x_out, double *abs_y_out) ISZ_API;
/* Returns 0 on success, ISZ_ERR_INVALID_ARG if ev is the wrong type. */

/* ISZ_EVENT_INPUT_POINTER_BUTTON */
int isz_event_get_pointer_button(const isz_event *ev,
                                  uint32_t *button_out,
                                  bool *press_out) ISZ_API;

/* ISZ_EVENT_INPUT_POINTER_AXIS */
int isz_event_get_pointer_axis(const isz_event *ev,
                                double *dx_out, double *dy_out,
                                int *source_out) ISZ_API; /* 0=finger, 1=wheel, 2=continuous */

/* ISZ_EVENT_INPUT_KEYBOARD_KEY */
int isz_event_get_keyboard_key(const isz_event *ev,
                                uint32_t *keycode_out,
                                bool *press_out) ISZ_API;

/* ISZ_EVENT_INPUT_KEYBOARD_MODIFIERS */
int isz_event_get_keyboard_modifiers(const isz_event *ev,
                                      uint32_t *mods_depressed_out,
                                      uint32_t *mods_latched_out,
                                      uint32_t *mods_locked_out,
                                      uint32_t *group_out) ISZ_API;

/* ISZ_EVENT_INPUT_TOUCH_DOWN / MOTION / UP */
int isz_event_get_touch(const isz_event *ev,
                         int32_t *touch_id_out,
                         double *x_out, double *y_out) ISZ_API;

/* ISZ_EVENT_INPUT_KEYBOARD_FOCUS_CHANGED */
isz_surface *isz_event_get_keyboard_focus(const isz_event *ev) ISZ_API;

/* ISZ_EVENT_OUTPUT_ADD / REMOVE / CLIENT_CONNECT / CLIENT_DISCONNECT */
isz_output *isz_event_get_output(const isz_event *ev) ISZ_API;
const char *isz_event_get_client_binary_path(const isz_event *ev) ISZ_API; /* NULL-terminated, valid for the callback duration */

/* ISZ_EVENT_CLIPBOARD_REQUEST */
const char *isz_event_get_clipboard_mime_type(const isz_event *ev) ISZ_API;
uint64_t    isz_event_get_clipboard_timestamp(const isz_event *ev) ISZ_API;

/* ISZ_EVENT_IDLE_INHIBIT_ACTIVE / INACTIVE */
isz_output *isz_event_get_idle_inhibit_output(const isz_event *ev) ISZ_API;

/* ISZ_EVENT_TEXT_INPUT_PREEDIT */
const char *isz_event_get_text_input_preedit(const isz_event *ev,
                                              int32_t *cursor_begin_out,
                                              int32_t *cursor_end_out) ISZ_API;
/* ISZ_EVENT_TEXT_INPUT_COMMIT */
const char *isz_event_get_text_input_commit(const isz_event *ev) ISZ_API;
/* ISZ_EVENT_TEXT_INPUT_CURSOR_RECTANGLE_NEEDED */
int isz_event_get_text_input_cursor_rectangle(const isz_event *ev,
                                                int32_t *x_out,
                                                int32_t *y_out,
                                                int32_t *w_out,
                                                int32_t *h_out) ISZ_API;

typedef void (*isz_event_listener_fn)(void *userdata, const isz_event *ev);

int isz_add_listener(isz_server *srv, enum isz_event_type type,
                     isz_event_listener_fn fn, void *userdata) ISZ_API;
int isz_remove_listener(isz_server *srv, enum isz_event_type type,
                        isz_event_listener_fn fn) ISZ_API;

/* ------------------------------------------------------------------ */
/* Portal consent (SPEC §6.11)                                         */
/* ------------------------------------------------------------------ */
/* General per-(output, kind) user-grant mechanism with a 60s timeout.
 * Replaces the older isz_capture_grant / isz_capture_check_consent
 * pair, which remain as thin wrappers for screen capture so existing
 * callers (x11bridge, tests) keep building. */
enum isz_consent_kind {
    ISZ_CONSENT_SCREEN_CAPTURE = 0,
    ISZ_CONSENT_FILE_ACCESS    = 1,
    ISZ_CONSENT_NOTIFICATION   = 2,
};
int  isz_consent_grant(isz_server *srv, isz_output *out,
                        enum isz_consent_kind kind) ISZ_API;

/* ------------------------------------------------------------------ */
/* Screen capture - §7.11                                             */
/* ------------------------------------------------------------------ */
int isz_output_capture_start(isz_output *out, int dmabuf_fd,
                              isz_buffer_desc *desc) ISZ_API;
int isz_output_capture_stop(isz_output *out) ISZ_API;

/* ------------------------------------------------------------------ */
/* Misc small structs                                                  */
/* ------------------------------------------------------------------ */
struct isz_rect {
    int32_t x1, y1, x2, y2; /* surface-local, half-open */
};

struct isz_plane_slot_info {
    int id;
    enum isz_plane_type type;
    uint32_t supported_formats[16];
    size_t format_count;
    bool supports_scaling;
    bool supports_transform;
    int zpos_min;
    int zpos_max;
};

/* ------------------------------------------------------------------ */
/* Crash recovery - §12 (opt-in)                                      */
/* ------------------------------------------------------------------ */
int isz_enable_crash_recovery(isz_server *srv) ISZ_API;

/* ------------------------------------------------------------------ */
/* Test hooks - §4 (only when ISHIZUE_ENABLE_TEST_HOOKS is defined)    */
/* ------------------------------------------------------------------ */
#ifdef ISHIZUE_ENABLE_TEST_HOOKS
typedef struct isz_test_client isz_test_client;
isz_test_client *isz_test_connect(isz_server *srv,
                                   const char *fake_binary_path) ISZ_API;
void isz_test_send_key(isz_test_client *client, uint32_t keycode,
                        bool press) ISZ_API;
void isz_test_send_pointer_motion(isz_test_client *client, int x, int y) ISZ_API;
void isz_test_simulate_output_hotplug(isz_server *srv, uint32_t width,
                                       uint32_t height) ISZ_API;
#endif

#ifdef __cplusplus
}
#endif

#endif /* ISHIZUE_H */
