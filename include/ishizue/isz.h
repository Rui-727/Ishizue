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
};

typedef void (*isz_event_listener_fn)(void *userdata, const isz_event *ev);

int isz_add_listener(isz_server *srv, enum isz_event_type type,
                     isz_event_listener_fn fn, void *userdata) ISZ_API;
int isz_remove_listener(isz_server *srv, enum isz_event_type type,
                        isz_event_listener_fn fn) ISZ_API;

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
