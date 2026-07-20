/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* x11_proto.h: minimal X11 wire protocol constants and structs.
 *
 * From scratch, no libX11 or libXCB. The bridge parses raw X11 bytes
 * off /tmp/.X11-unix/X<n> and emits raw X11 replies/errors/events.
 * See x11bridge/README.md and doc/research/x11-protocol-internals.md
 * for the wire layout reference. */

#ifndef X11_PROTO_H
#define X11_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* X11 byte order bytes (first byte of setup_request). */
#define X11_BYTE_ORDER_LSB  0x6Cu  /* little-endian */
#define X11_BYTE_ORDER_MSB  0x42u  /* big-endian */

/* Setup reply status byte. */
#define X11_SETUP_FAILED       0u
#define X11_SETUP_AUTHENTICATE 2u
#define X11_SETUP_SUCCESS      1u

/* Predefined XIDs and pixel values. The bridge picks these once and
 * uses them in every SetupSuccess. Root, visual, colormap, etc. must
 * sit below the per-client resource-id-base so client-allocated XIDs
 * never collide with them. */
#define X11_ROOT_WINDOW_ID     0x00000100u
#define X11_ROOT_VISUAL_ID     0x00000021u
#define X11_DEFAULT_COLORMAP   0x00000022u
#define X11_BLACK_PIXEL        0x00000000u
#define X11_WHITE_PIXEL        0x00FFFFFFu

/* Per-client XID range layout. Each connecting client gets a slice of
 * the 32-bit XID space: base = X11_XID_BASE_FIRST + slot * stride,
 * mask = X11_XID_MASK. The slot is the connection index, incremented
 * per accepted client. The mask leaves 21 bits for the per-client
 * counter, so each client has ~2M XIDs before exhausting its range. */
#define X11_XID_BASE_FIRST     0x40000000u
#define X11_XID_STRIDE         0x00200000u
#define X11_XID_MASK           0x001FFFFFu

/* Default screen geometry advertised in SetupSuccess. The bridge is a
 * separate process and does not know the real Ishizue output geometry
 * at setup time, so it picks a default. Real X clients read these
 * to size their top-level windows. */
#define X11_DEFAULT_ROOT_W     1024u
#define X11_DEFAULT_ROOT_H     768u
#define X11_DEFAULT_ROOT_W_MM  200u
#define X11_DEFAULT_ROOT_H_MM  150u

/* X11 protocol release number advertised in SetupSuccess. The bridge
 * picks the same number Xorg ships, so version-checking clients pass. */
#define X11_RELEASE_NUMBER     11800000u

/* Request major opcodes the bridge handles. W8-A grew the set from
 * {CreateWindow, GetGeometry} to ten opcodes. W9-B grows it to twenty:
 * adds GetWindowAttributes, QueryTree, GetAtomName, DeleteProperty,
 * SetSelectionOwner, GetSelectionOwner, QueryPointer, SetInputFocus,
 * CreateGC, PutImage. Other opcodes the scaffold must accept
 * (QueryExtension, GetInputFocus, ...) stay in the no-op default
 * branch; see x11_client.c. */
enum x11_request {
    X11_REQ_CREATE_WINDOW        = 1,
    X11_REQ_CHANGE_WINDOW_ATTRS  = 2,
    X11_REQ_GET_WINDOW_ATTRS     = 3,
    X11_REQ_DESTROY_WINDOW       = 4,
    X11_REQ_MAP_WINDOW           = 8,
    X11_REQ_UNMAP_WINDOW         = 10,
    X11_REQ_CONFIGURE_WINDOW     = 12,
    X11_REQ_GET_GEOMETRY         = 14,
    X11_REQ_QUERY_TREE           = 15,
    X11_REQ_INTERN_ATOM          = 16,
    X11_REQ_GET_ATOM_NAME        = 17,
    X11_REQ_CHANGE_PROPERTY      = 18,
    X11_REQ_DELETE_PROPERTY      = 19,
    X11_REQ_GET_PROPERTY         = 20,
    X11_REQ_SET_SELECTION_OWNER  = 22,
    X11_REQ_GET_SELECTION_OWNER  = 23,
    X11_REQ_QUERY_POINTER        = 38,
    X11_REQ_SET_INPUT_FOCUS      = 42,
    X11_REQ_GET_INPUT_FOCUS      = 43,
    X11_REQ_CREATE_GC            = 55,
    X11_REQ_PUT_IMAGE            = 72,
    X11_REQ_QUERY_EXTENSION      = 98,
};

/* CreateWindow value-mask bits (X11 protocol spec, low bit first). The
 * value-list that follows the mask carries one 4-byte slot per set
 * bit, in this order. */
#define X11_CW_BG_PIXMAP         0x00000001u
#define X11_CW_BG_PIXEL          0x00000002u
#define X11_CW_BORDER_PIXMAP     0x00000004u
#define X11_CW_BORDER_PIXEL      0x00000008u
#define X11_CW_BIT_GRAVITY       0x00000010u
#define X11_CW_WIN_GRAVITY       0x00000020u
#define X11_CW_BACKING_STORE     0x00000040u
#define X11_CW_BACKING_PLANES    0x00000080u
#define X11_CW_BACKING_PIXEL     0x00000100u
#define X11_CW_OVERRIDE_REDIRECT 0x00000200u
#define X11_CW_SAVE_UNDER        0x00000400u
#define X11_CW_EVENT_MASK        0x00000800u
#define X11_CW_DONT_PROPAGATE    0x00001000u
#define X11_CW_COLORMAP          0x00002000u
#define X11_CW_CURSOR            0x00004000u

/* X11 error codes (from the protocol spec). */
enum x11_error_code {
    X11_ERR_REQUEST        = 1,
    X11_ERR_VALUE          = 2,
    X11_ERR_WINDOW         = 3,
    X11_ERR_PIXMAP         = 4,
    X11_ERR_ATOM           = 5,
    X11_ERR_CURSOR         = 6,
    X11_ERR_FONT           = 7,
    X11_ERR_MATCH          = 8,
    X11_ERR_DRAWABLE       = 9,
    X11_ERR_ACCESS         = 10,
    X11_ERR_ALLOC          = 11,
    X11_ERR_COLORMAP       = 12,
    X11_ERR_GCONTEXT       = 13,
    X11_ERR_IDCHOICE       = 14,
    X11_ERR_NAME           = 15,
    X11_ERR_LENGTH         = 16,
    X11_ERR_IMPLEMENTATION = 17,
};

/* X11 event codes (first byte of an X11 event). */
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
    X11_EV_EXPOSE            = 12,
    X11_EV_GRAPHICS_EXPOSURE = 13,
    X11_EV_NO_EXPOSE         = 14,
    X11_EV_DESTROY_NOTIFY    = 17,
    X11_EV_UNMAP_NOTIFY      = 18,
    X11_EV_MAP_NOTIFY        = 19,
    X11_EV_CONFIGURE_NOTIFY  = 22,
    X11_EV_PROPERTY_NOTIFY   = 28,
    X11_EV_SELECTION_CLEAR   = 29,
    X11_EV_SELECTION_REQUEST = 30,
    X11_EV_SELECTION_NOTIFY  = 31,
};

/* event-mask bits that the bridge actually inspects when deciding
 * whether to deliver an event to a given client. The full
 * SETofEVENT mask is 25 bits; the rest are accepted and ignored. */
#define X11_EVMASK_KEY_PRESS          (1u << 0)
#define X11_EVMASK_KEY_RELEASE        (1u << 1)
#define X11_EVMASK_BUTTON_PRESS       (1u << 2)
#define X11_EVMASK_BUTTON_RELEASE     (1u << 3)
#define X11_EVMASK_ENTER_WINDOW       (1u << 4)
#define X11_EVMASK_LEAVE_WINDOW       (1u << 5)
#define X11_EVMASK_POINTER_MOTION     (1u << 6)
#define X11_EVMASK_POINTER_MOTION_HINT (1u << 7)
#define X11_EVMASK_BUTTON1_MOTION     (1u << 8)
#define X11_EVMASK_BUTTON2_MOTION     (1u << 9)
#define X11_EVMASK_BUTTON3_MOTION     (1u << 10)
#define X11_EVMASK_BUTTON4_MOTION     (1u << 11)
#define X11_EVMASK_BUTTON5_MOTION     (1u << 12)
#define X11_EVMASK_BUTTON_MOTION      (1u << 13)
#define X11_EVMASK_KEYMAP_STATE       (1u << 14)
#define X11_EVMASK_EXPOSURE           (1u << 15)
#define X11_EVMASK_VISIBILITY_CHANGE  (1u << 16)
#define X11_EVMASK_STRUCTURE_NOTIFY   (1u << 17)
#define X11_EVMASK_RESIZE_REDIRECT    (1u << 18)
#define X11_EVMASK_SUBSTRUCTURE_NOTIFY (1u << 19)
#define X11_EVMASK_SUBSTRUCTURE_REDIRECT (1u << 20)
#define X11_EVMASK_FOCUS_CHANGE       (1u << 21)
#define X11_EVMASK_PROPERTY_CHANGE    (1u << 22)
#define X11_EVMASK_COLOR_MAP_CHANGE   (1u << 23)
#define X11_EVMASK_OWNER_GRAB_BUTTON (1u << 24)

/* ConfigureWindow value-mask bits (X11 protocol spec). */
#define X11_CFG_MASK_X          0x0001u
#define X11_CFG_MASK_Y          0x0002u
#define X11_CFG_MASK_WIDTH      0x0004u
#define X11_CFG_MASK_HEIGHT     0x0008u
#define X11_CFG_MASK_BORDER_W   0x0010u
#define X11_CFG_MASK_SIBLING    0x0020u
#define X11_CFG_MASK_STACK_MODE 0x0040u

/* ConfigureWindow stack-mode values, when X11_CFG_MASK_STACK_MODE is set. */
#define X11_STACK_ABOVE       0u
#define X11_STACK_BELOW       1u
#define X11_STACK_TOP_IF      2u
#define X11_STACK_BOTTOM_IF   3u
#define X11_STACK_OPPOSITE    4u

/* ChangeProperty mode values. */
#define X11_PROP_MODE_REPLACE 0u
#define X11_PROP_MODE_PREPEND 1u
#define X11_PROP_MODE_APPEND  2u

/* Window map-state values reported by GetWindowAttributes. The bridge
 * tracks only Unmapped and IsViewable: Unviewable requires a real
 * window hierarchy, which the bridge does not have. */
#define X11_MAP_STATE_UNMAPPED   0u
#define X11_MAP_STATE_UNVIEWABLE 1u
#define X11_MAP_STATE_VIEWABLE   2u

/* Backing-store values reported by GetWindowAttributes. The bridge
 * defaults to NotUseful (no backing store) unless the client set
 * backing-store via ChangeWindowAttributes. */
#define X11_BACKING_STORE_NOTUSEFUL  0u
#define X11_BACKING_STORE_WHENMAPPED 1u
#define X11_BACKING_STORE_ALWAYS     2u

/* SetInputFocus revert-to values. */
#define X11_FOCUS_REVERT_NONE        0u
#define X11_FOCUS_REVERT_POINTER_ROOT 1u
#define X11_FOCUS_REVERT_ANCESTOR    2u

/* PutImage format values. */
#define X11_IMAGE_FORMAT_BITMAP    0u
#define X11_IMAGE_FORMAT_XYPIXMAP  1u
#define X11_IMAGE_FORMAT_ZPIXMAP   2u

/* CreateGC value-mask bits (X11 protocol spec, low bit first). The
 * value-list carries one 4-byte slot per set bit, in this order. The
 * bridge stores only the fields it inspects later (graphics-exposure
 * for PutImage) and accepts the rest. */
#define X11_GC_FUNCTION          0x00000001u
#define X11_GC_PLANE_MASK        0x00000002u
#define X11_GC_FOREGROUND        0x00000004u
#define X11_GC_BACKGROUND        0x00000008u
#define X11_GC_LINE_WIDTH        0x00000010u
#define X11_GC_LINE_STYLE        0x00000020u
#define X11_GC_CAP_STYLE         0x00000040u
#define X11_GC_JOIN_STYLE        0x00000080u
#define X11_GC_FILL_STYLE        0x00000100u
#define X11_GC_FILL_RULE         0x00000200u
#define X11_GC_TILE              0x00000400u
#define X11_GC_STIPPLE           0x00000800u
#define X11_GC_TILE_STIPPLE_X    0x00001000u
#define X11_GC_TILE_STIPPLE_Y    0x00002000u
#define X11_GC_FONT              0x00004000u
#define X11_GC_SUBWINDOW_MODE    0x00008000u
#define X11_GC_GRAPHICS_EXPOSURE 0x00010000u
#define X11_GC_CLIP_X_ORIGIN     0x00020000u
#define X11_GC_CLIP_Y_ORIGIN     0x00040000u
#define X11_GC_CLIP_MASK         0x00080000u
#define X11_GC_DASH_OFFSET       0x00100000u
#define X11_GC_DASHES            0x00200000u
#define X11_GC_ARC_MODE          0x00400000u

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

/* DEPTH entry: 8-byte header + 24-byte VISUALTYPE per visual. */
struct x11_depth {
    uint8_t  depth;
    uint8_t  pad1;
    uint8_t  visuals_len[2];
    uint8_t  pad2[4];
};

struct x11_visualtype {
    uint8_t  visual_id[4];
    uint8_t  class_;
    uint8_t  pad1;
    uint8_t  colormap_entries[2];
    uint8_t  red_mask[4];
    uint8_t  green_mask[4];
    uint8_t  blue_mask[4];
    uint8_t  bits_per_rgb_value;
    uint8_t  pad2[3];
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
_Static_assert(sizeof(struct x11_depth)          == 8,  "x11_depth layout");
_Static_assert(sizeof(struct x11_visualtype)     == 24, "x11_visualtype layout");
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

/* Build a real SetupSuccess into out_buf. The reply contains:
 *   - 40-byte fixed header
 *   - vendor string (padded to 4)
 *   - 3 pixmap formats: depth 1 (bpp 1), depth 24 (bpp 32), depth 32
 *     (bpp 32), all scanline-pad 32
 *   - 1 root screen with 1 allowed-depth (24) and 1 TrueColor visual
 * Total size = 40 + 8 + 24 + 72 = 144 bytes.
 *
 * The caller passes in the per-client resource_id_base and
 * resource_id_mask so each connecting client gets its own XID range.
 * vendor must be non-NULL and NUL-terminated; only the first 252
 * bytes are used (so vendor_len fits in a u16 with padding headroom).
 *
 * Returns total bytes written, or 0 if out_cap is too small or vendor
 * is too long. */
size_t   x11_build_setup_success(uint8_t *out_buf, size_t out_cap,
                                 uint8_t client_byte_order,
                                 uint16_t server_proto_major,
                                 uint16_t server_proto_minor,
                                 uint32_t resource_id_base,
                                 uint32_t resource_id_mask,
                                 const char *vendor);

/* Build a 32-byte X11 error event. Errors are always 32 bytes. */
size_t   x11_build_error(uint8_t *out_buf, uint8_t error_code,
                         uint16_t sequence, uint32_t bad_value,
                         uint8_t major_opcode, uint8_t minor_opcode,
                         uint8_t byte_order);

/* Build a 32-byte GetGeometry reply. reply-length is 0 (no extra
 * data). depth is the window's depth; root_xid is the root window the
 * drawable lives on (X11_ROOT_WINDOW_ID for top-level windows). */
size_t   x11_build_get_geometry_reply(uint8_t *out_buf,
                                      uint8_t depth, uint32_t root_xid,
                                      int16_t x, int16_t y,
                                      uint16_t width, uint16_t height,
                                      uint16_t border_width,
                                      uint16_t sequence,
                                      uint8_t byte_order);

/* Build a 32-byte InternAtom reply. The reply carries only the atom
 * value at byte 8; bytes 4..7 are the reply-length (0). atom 0 means
 * None (only-if-exists=true and name not found). */
size_t   x11_build_intern_atom_reply(uint8_t *out_buf, uint32_t atom,
                                     uint16_t sequence, uint8_t byte_order);

/* Build a GetProperty reply. The reply is 32 bytes plus the value
 * bytes, padded to 4. The fixed fields are:
 *   byte 0: 1 (reply indicator)
 *   byte 1: format (0, 8, 16, or 32; 0 means property does not exist)
 *   bytes 2..3: sequence
 *   bytes 4..7: reply length in 4-byte units (n/4 where n is value bytes padded to 4)
 *   bytes 8..11: type atom (0 if property does not exist)
 *   bytes 12..15: bytes-after
 *   bytes 16..19: value length in format units
 *   bytes 20..31: unused
 *   bytes 32..: value (n bytes), padded to 4
 *
 * Caller supplies format, type, bytes_after, value_len (in format
 * units), and the value bytes. For "property does not exist",
 * format=0/type=0/value_len=0/value=NULL. out_buf must be at least
 * 32 + x11_pad4(value_bytes) bytes. Returns total bytes written. */
size_t   x11_build_get_property_reply(uint8_t *out_buf, size_t out_cap,
                                      uint8_t format, uint32_t type,
                                      uint32_t bytes_after,
                                      uint32_t value_len,
                                      const uint8_t *value, size_t value_bytes,
                                      uint16_t sequence, uint8_t byte_order);

/* Build a 32-byte MapNotify event. event is the window that has the
 * StructureNotify mask; window is the window that was mapped. */
size_t   x11_build_map_notify(uint8_t *out_buf,
                              uint32_t event, uint32_t window,
                              bool override_redirect,
                              uint16_t sequence, uint8_t byte_order);

/* Build a 32-byte UnmapNotify event. event is the window that has the
 * StructureNotify mask; window is the window that was unmapped.
 * from_configure is byte 1 (set when the unmap was caused by
 * ConfigureWindow on the parent's win-gravity, otherwise 0). */
size_t   x11_build_unmap_notify(uint8_t *out_buf,
                                uint32_t event, uint32_t window,
                                bool from_configure,
                                uint16_t sequence, uint8_t byte_order);

/* Build a 32-byte ConfigureNotify event. above_sibling 0 means the
 * window is at the bottom of the stacking order. */
size_t   x11_build_configure_notify(uint8_t *out_buf,
                                    uint32_t event, uint32_t window,
                                    uint32_t above_sibling,
                                    int16_t x, int16_t y,
                                    uint16_t width, uint16_t height,
                                    uint16_t border_width,
                                    bool override_redirect,
                                    uint16_t sequence, uint8_t byte_order);

/* Build a 32-byte DestroyNotify event. event is the window that has
 * the StructureNotify mask (or SubstructureNotify on the parent);
 * window is the window being destroyed. */
size_t   x11_build_destroy_notify(uint8_t *out_buf,
                                  uint32_t event, uint32_t window,
                                  uint16_t sequence, uint8_t byte_order);

/* Build a 32-byte PropertyNotify event. state is 0 for NewValue, 1
 * for Deleted. time is the X server timestamp; the bridge uses 0
 * since it does not track a clock. */
size_t   x11_build_property_notify(uint8_t *out_buf,
                                   uint32_t window, uint32_t atom,
                                   uint32_t time, uint8_t state,
                                   uint16_t sequence, uint8_t byte_order);

/* Build a 44-byte GetWindowAttributes reply. Reply length is 9
 * (36 bytes after the first 8). map_state is X11_MAP_STATE_*.
 * colormap is 0 for CopyFromParent. */
size_t   x11_build_get_window_attributes_reply(uint8_t *out_buf,
                                               uint8_t backing_store,
                                               uint32_t visual,
                                               uint16_t class_,
                                               uint8_t bit_gravity,
                                               uint8_t win_gravity,
                                               uint32_t backing_planes,
                                               uint32_t backing_pixel,
                                               bool override_redirect,
                                               bool save_under,
                                               uint8_t map_state,
                                               bool map_installed,
                                               uint32_t colormap,
                                               uint32_t all_event_masks,
                                               uint32_t your_event_mask,
                                               uint16_t do_not_propagate_mask,
                                               uint16_t sequence,
                                               uint8_t byte_order);

/* Build a QueryTree reply. The reply is 32 bytes plus 4 bytes per
 * child. root is the root window the queried window lives under;
 * parent is 0 if the queried window is the root, else the parent
 * XID. children is an array of child XIDs; n is its length. */
size_t   x11_build_query_tree_reply(uint8_t *out_buf, size_t out_cap,
                                    uint32_t root, uint32_t parent,
                                    const uint32_t *children, uint16_t n,
                                    uint16_t sequence, uint8_t byte_order);

/* Build a GetAtomName reply. The reply is 32 bytes plus the name
 * bytes padded to 4. name_len is in bytes; name need not be
 * NUL-terminated. Returns total bytes written, or 0 if out_cap is
 * too small. */
size_t   x11_build_get_atom_name_reply(uint8_t *out_buf, size_t out_cap,
                                       const char *name, uint16_t name_len,
                                       uint16_t sequence, uint8_t byte_order);

/* Build a 32-byte GetSelectionOwner reply. owner is 0 if no client
 * owns the selection. The X11 protocol does not return the timestamp
 * in this reply; the bridge keeps it internally for stale-claim
 * rejection on SetSelectionOwner. */
size_t   x11_build_get_selection_owner_reply(uint8_t *out_buf,
                                             uint32_t owner,
                                             uint16_t sequence,
                                             uint8_t byte_order);

/* Build a 32-byte QueryPointer reply. same_screen is byte 1; root is
 * the root window under the pointer; child is the window under the
 * pointer (0 if outside any window); root_x/root_y are pointer
 * coords relative to root; win_x/win_y are pointer coords relative
 * to the queried window; mask is button + modifier state. */
size_t   x11_build_query_pointer_reply(uint8_t *out_buf,
                                       bool same_screen,
                                       uint32_t root, uint32_t child,
                                       int16_t root_x, int16_t root_y,
                                       int16_t win_x, int16_t win_y,
                                       uint16_t mask,
                                       uint16_t sequence, uint8_t byte_order);

/* Build a 32-byte SelectionClear event. Delivered to the previous
 * owner when a new owner takes over or ownership is cleared. */
size_t   x11_build_selection_clear(uint8_t *out_buf,
                                   uint32_t time, uint32_t owner,
                                   uint32_t selection_atom,
                                   uint16_t sequence, uint8_t byte_order);

/* Build a 32-byte SelectionRequest event. Delivered to the current
 * owner when a requestor calls ConvertSelection. */
size_t   x11_build_selection_request(uint8_t *out_buf,
                                     uint32_t time, uint32_t owner,
                                     uint32_t requestor,
                                     uint32_t selection_atom,
                                     uint32_t target_atom,
                                     uint32_t property_atom,
                                     uint16_t sequence, uint8_t byte_order);

/* Build a 32-byte SelectionNotify event. Delivered to the requestor
 * when the owner has finished converting a selection (property is 0
 * if the conversion failed). */
size_t   x11_build_selection_notify(uint8_t *out_buf,
                                    uint32_t time, uint32_t requestor,
                                    uint32_t selection_atom,
                                    uint32_t target_atom,
                                    uint32_t property_atom,
                                    uint16_t sequence, uint8_t byte_order);

/* Build a 32-byte GraphicsExposure event. Emitted after a CopyArea
 * that exposes new pixels when the GC has graphics-exposure=true.
 * count is the number of subsequent GraphicsExposure events that
 * follow for the same request. */
size_t   x11_build_graphics_exposure(uint8_t *out_buf,
                                     uint32_t drawable,
                                     uint16_t x, uint16_t y,
                                     uint16_t width, uint16_t height,
                                     uint16_t minor_opcode,
                                     uint16_t count,
                                     uint8_t major_opcode,
                                     uint16_t sequence, uint8_t byte_order);

/* Build a 32-byte NoExpose event. Emitted after a CopyArea that
 * exposes no new pixels when the GC has graphics-exposure=true. */
size_t   x11_build_no_expose(uint8_t *out_buf,
                             uint32_t drawable,
                             uint16_t minor_opcode,
                             uint8_t major_opcode,
                             uint16_t sequence, uint8_t byte_order);

#endif /* X11_PROTO_H */
