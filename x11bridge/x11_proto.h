/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* x11_proto.h: minimal X11 wire protocol constants and structs.
 *
 * From scratch, no libX11 or libXCB. Just enough to:
 *   - parse a client setup_request
 *   - reply with a minimal setup_success
 *   - parse a 4-byte request header (opcode + length)
 *   - emit a minimal 32-byte X11 event for input translation
 *
 * The bridge is not an X server. It parses what it needs and stubs
 * the rest. See x11bridge/README.md for the long list of limitations. */

#ifndef X11_PROTO_H
#define X11_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* X11 byte order bytes (first byte of setup_request). */
#define X11_BYTE_ORDER_LSB  0x6Cu  /* little-endian */
#define X11_BYTE_ORDER_MSB  0x42u  /* big-endian */

/* Setup reply status byte. */
#define X11_SETUP_FAILED       0u
#define X11_SETUP_AUTHENTICATE 2u
#define X11_SETUP_SUCCESS      1u

/* Request major opcodes (the subset the bridge cares about). */
enum x11_request {
    X11_REQ_CREATE_WINDOW        = 1,
    X11_REQ_CHANGE_WINDOW_ATTRS  = 2,
    X11_REQ_GET_WINDOW_ATTRS     = 3,
    X11_REQ_DESTROY_WINDOW       = 4,
    X11_REQ_MAP_WINDOW           = 8,
    X11_REQ_UNMAP_WINDOW         = 10,
    X11_REQ_CONFIGURE_WINDOW     = 12,
    X11_REQ_INTERN_ATOM          = 16,
    X11_REQ_CHANGE_PROPERTY      = 18,
    X11_REQ_GET_INPUT_FOCUS      = 43,
    X11_REQ_QUERY_EXTENSION      = 98,
};

/* Event codes (first byte of an X11 event). */
enum x11_event {
    X11_EV_KEY_PRESS         = 2,
    X11_EV_KEY_RELEASE       = 3,
    X11_EV_BUTTON_PRESS      = 4,
    X11_EV_BUTTON_RELEASE    = 5,
    X11_EV_MOTION_NOTIFY     = 6,
    X11_EV_ENTER_NOTIFY      = 7,
    X11_EV_LEAVE_NOTIFY      = 8,
    X11_EV_FOCUS_IN          = 9,
    X11_EV_FOCUS_OUT         = 10,
    X11_EV_UNMAP_NOTIFY      = 18,
    X11_EV_MAP_NOTIFY        = 19,
    X11_EV_CONFIGURE_NOTIFY  = 22,
};

/* X11 button numbers for ButtonPress/ButtonRelease events. */
#define X11_BUTTON_LEFT     1u
#define X11_BUTTON_MIDDLE   2u
#define X11_BUTTON_RIGHT    3u
#define X11_BUTTON_SCROLL_UP    4u
#define X11_BUTTON_SCROLL_DOWN  5u

/* Wire-layout structs. Fields are byte arrays so we can write into
 * them with x11_put_u16/u32 without alignment or aliasing concerns.
 * The byte order is chosen by the client in setup_request.byte_order
 * and applied per-field by the bridge. */
struct x11_setup_request {
    uint8_t  byte_order;
    uint8_t  pad1;
    uint8_t  proto_major[2];
    uint8_t  proto_minor[2];
    uint8_t  auth_name_len[2];
    uint8_t  auth_data_len[2];
    uint8_t  pad2[2];
};

struct x11_setup_success {
    uint8_t  status;                  /* X11_SETUP_SUCCESS */
    uint8_t  pad1;
    uint8_t  proto_major[2];
    uint8_t  proto_minor[2];
    uint8_t  length[2];               /* 4-byte units after first 8 bytes */
    uint8_t  release_number[4];
    uint8_t  resource_id_base[4];
    uint8_t  resource_id_mask[4];
    uint8_t  motion_buffer_size[4];
    uint8_t  vendor_len[2];
    uint8_t  max_request_length[2];
    uint8_t  roots_len;
    uint8_t  pixmap_formats_len;
    uint8_t  image_byte_order;
    uint8_t  bitmap_bit_order;
    uint8_t  bitmap_scanline_unit;
    uint8_t  bitmap_scanline_pad;
    uint8_t  min_keycode;
    uint8_t  max_keycode;
    uint8_t  pad2[4];
};

struct x11_pixmap_format {
    uint8_t  depth;
    uint8_t  bits_per_pixel;
    uint8_t  scanline_pad;
    uint8_t  pad[5];
};

struct x11_root_screen {
    uint8_t  root_window_id[4];
    uint8_t  default_colormap[4];
    uint8_t  white_pixel[4];
    uint8_t  black_pixel[4];
    uint8_t  current_input_masks[4];
    uint8_t  width_px[2];
    uint8_t  height_px[2];
    uint8_t  width_mm[2];
    uint8_t  height_mm[2];
    uint8_t  min_installed_maps[2];
    uint8_t  max_installed_maps[2];
    uint8_t  root_visual_id[4];
    uint8_t  backing_stores;
    uint8_t  save_unders;
    uint8_t  root_depth;
    uint8_t  depths_len;
};

struct x11_request_header {
    uint8_t  major_opcode;
    uint8_t  data;
    uint8_t  length[2];               /* 4-byte units, includes this header */
};

/* Generic 32-byte X11 event envelope. Sufficient for KeyPress,
 * KeyRelease, ButtonPress, ButtonRelease, MotionNotify. Other event
 * types have different layouts past the first 8 bytes; the bridge
 * only sends the input event variants above. */
struct x11_event_32 {
    uint8_t  code;                    /* event type */
    uint8_t  detail;
    uint8_t  sequence_number[2];
    uint8_t  time[4];                 /* milliseconds */
    uint8_t  root[4];
    uint8_t  event[4];
    uint8_t  child[4];
    uint8_t  event_x[2];
    uint8_t  event_y[2];
    uint8_t  root_x[2];
    uint8_t  root_y[2];
    uint8_t  state[2];
    uint8_t  same_screen;
    uint8_t  pad;
};

_Static_assert(sizeof(struct x11_setup_request)  == 12, "x11_setup_request layout");
_Static_assert(sizeof(struct x11_setup_success)  == 40, "x11_setup_success layout");
_Static_assert(sizeof(struct x11_pixmap_format)  == 8,  "x11_pixmap_format layout");
_Static_assert(sizeof(struct x11_root_screen)    == 40, "x11_root_screen layout");
_Static_assert(sizeof(struct x11_request_header) == 4,  "x11_request_header layout");
_Static_assert(sizeof(struct x11_event_32)       == 32, "x11_event_32 layout");

/* Byte-order helpers. Read/write 16/32-bit fields in the byte order
 * the client chose in setup_request.byte_order. */
uint16_t x11_get_u16(const uint8_t *p, uint8_t byte_order);
uint32_t x11_get_u32(const uint8_t *p, uint8_t byte_order);
void     x11_put_u16(uint8_t *p, uint16_t v, uint8_t byte_order);
void     x11_put_u32(uint8_t *p, uint32_t v, uint8_t byte_order);

/* Pad n up to a multiple of 4. */
size_t   x11_pad4(size_t n);

/* Build a minimal setup_success into out_buf. Returns total bytes
 * written, or 0 if out_cap is too small. The reply contains: empty
 * vendor, one pixmap format (depth 24, bpp 32, pad 32), one root
 * screen (1024x768, root id 1, root visual id 1, root depth 24, no
 * depths). This gets a real X11 client past connection setup; later
 * requests that depend on visuals/depths will fail. */
size_t   x11_build_setup_success(uint8_t *out_buf, size_t out_cap,
                                 uint8_t client_byte_order,
                                 uint16_t server_proto_major,
                                 uint16_t server_proto_minor);

#endif /* X11_PROTO_H */
