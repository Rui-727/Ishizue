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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* x11_proto.c: minimal X11 wire protocol primitives.
 *
 * W7-C: real SetupSuccess with vendor string, three pixmap formats,
 * one root screen, one depth (24) with one TrueColor visual. Real
 * error event builder. Real GetGeometry reply builder. */

#include "x11_proto.h"

#include <string.h>

uint16_t x11_get_u16(const uint8_t *p, uint8_t byte_order) {
    if (byte_order == X11_BYTE_ORDER_MSB) {
        return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
    }
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t x11_get_u32(const uint8_t *p, uint8_t byte_order) {
    if (byte_order == X11_BYTE_ORDER_MSB) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
    }
    return  (uint32_t)p[0]        |
           ((uint32_t)p[1] << 8)  |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void x11_put_u16(uint8_t *p, uint16_t v, uint8_t byte_order) {
    if (byte_order == X11_BYTE_ORDER_MSB) {
        p[0] = (uint8_t)((v >> 8) & 0xffu);
        p[1] = (uint8_t)(v & 0xffu);
    } else {
        p[0] = (uint8_t)(v & 0xffu);
        p[1] = (uint8_t)((v >> 8) & 0xffu);
    }
}

void x11_put_u32(uint8_t *p, uint32_t v, uint8_t byte_order) {
    if (byte_order == X11_BYTE_ORDER_MSB) {
        p[0] = (uint8_t)((v >> 24) & 0xffu);
        p[1] = (uint8_t)((v >> 16) & 0xffu);
        p[2] = (uint8_t)((v >> 8)  & 0xffu);
        p[3] = (uint8_t)(v & 0xffu);
    } else {
        p[0] = (uint8_t)(v & 0xffu);
        p[1] = (uint8_t)((v >> 8)  & 0xffu);
        p[2] = (uint8_t)((v >> 16) & 0xffu);
        p[3] = (uint8_t)((v >> 24) & 0xffu);
    }
}

size_t x11_pad4(size_t n) {
    return (n + 3u) & ~(size_t)3u;
}

/* Layout produced by x11_build_setup_success:
 *
 *   offset 0   : x11_setup_success (40 bytes)
 *   offset 40  : vendor string, padded to 4 (vendor_len bytes)
 *   offset 40+v: 3 x11_pixmap_format entries (24 bytes total)
 *   offset 64+v: 1 x11_root_screen (40 bytes)
 *   offset 104+v: 1 x11_depth (8 bytes) + 1 x11_visualtype (24 bytes) = 32 bytes
 *
 * where v = x11_pad4(strlen(vendor)).
 *
 * For vendor "Ishizue" (7 bytes), v = 8, total = 40 + 8 + 24 + 40 + 32 = 144.
 *
 * The length field counts 4-byte units after the first 8 bytes, so
 * length = (total - 8) / 4. */
size_t x11_build_setup_success(uint8_t *out_buf, size_t out_cap,
                               uint8_t client_byte_order,
                               uint16_t server_proto_major,
                               uint16_t server_proto_minor,
                               uint32_t resource_id_base,
                               uint32_t resource_id_mask,
                               const char *vendor) {
    if (vendor == NULL) {
        return 0;
    }
    size_t vlen = strlen(vendor);
    if (vlen > 252u) {
        return 0;  /* vendor_len is u16; cap well below to leave padding headroom */
    }
    size_t vpad = x11_pad4(vlen);

    const size_t hdr_size     = sizeof(struct x11_setup_success);
    const size_t fmts_size    = 3u * sizeof(struct x11_pixmap_format);
    const size_t root_size    = sizeof(struct x11_root_screen);
    const size_t depth_size   = sizeof(struct x11_depth) + sizeof(struct x11_visualtype);
    const size_t total = hdr_size + vpad + fmts_size + root_size + depth_size;

    if (out_cap < total) {
        return 0;
    }

    memset(out_buf, 0, total);

    /* Fixed header. */
    struct x11_setup_success *hdr = (struct x11_setup_success *)out_buf;
    hdr->status              = X11_SETUP_SUCCESS;
    x11_put_u16(hdr->proto_major, server_proto_major, client_byte_order);
    x11_put_u16(hdr->proto_minor, server_proto_minor, client_byte_order);
    x11_put_u16(hdr->length, (uint16_t)((total - 8u) / 4u), client_byte_order);
    x11_put_u32(hdr->release_number, X11_RELEASE_NUMBER, client_byte_order);
    x11_put_u32(hdr->resource_id_base, resource_id_base, client_byte_order);
    x11_put_u32(hdr->resource_id_mask, resource_id_mask, client_byte_order);
    x11_put_u32(hdr->motion_buffer_size, 0u, client_byte_order);
    x11_put_u16(hdr->vendor_len, (uint16_t)vlen, client_byte_order);
    x11_put_u16(hdr->max_request_length, 0xFFFFu, client_byte_order);
    hdr->roots_len            = 1;
    hdr->pixmap_formats_len   = 3;
    hdr->image_byte_order     =
        (client_byte_order == X11_BYTE_ORDER_MSB) ? 1u : 0u;
    hdr->bitmap_bit_order     = hdr->image_byte_order;
    hdr->bitmap_scanline_unit = 32;
    hdr->bitmap_scanline_pad  = 32;
    hdr->min_keycode          = 8;
    hdr->max_keycode          = 255;

    /* Vendor string. */
    if (vlen > 0) {
        memcpy(out_buf + hdr_size, vendor, vlen);
    }

    /* Pixmap formats: depth 1 (1 bpp), depth 24 (32 bpp), depth 32 (32 bpp). */
    uint8_t *fmts = out_buf + hdr_size + vpad;
    {
        struct x11_pixmap_format *f1 = (struct x11_pixmap_format *)(fmts + 0);
        f1->depth          = 1;
        f1->bits_per_pixel = 1;
        f1->scanline_pad   = 32;
    }
    {
        struct x11_pixmap_format *f2 = (struct x11_pixmap_format *)(fmts + 8);
        f2->depth          = 24;
        f2->bits_per_pixel = 32;
        f2->scanline_pad   = 32;
    }
    {
        struct x11_pixmap_format *f3 = (struct x11_pixmap_format *)(fmts + 16);
        f3->depth          = 32;
        f3->bits_per_pixel = 32;
        f3->scanline_pad   = 32;
    }

    /* Root screen. */
    uint8_t *rootp = fmts + fmts_size;
    struct x11_root_screen *root = (struct x11_root_screen *)rootp;
    x11_put_u32(root->root_window_id, X11_ROOT_WINDOW_ID, client_byte_order);
    x11_put_u32(root->default_colormap, X11_DEFAULT_COLORMAP, client_byte_order);
    x11_put_u32(root->white_pixel, X11_WHITE_PIXEL, client_byte_order);
    x11_put_u32(root->black_pixel, X11_BLACK_PIXEL, client_byte_order);
    x11_put_u32(root->current_input_masks, 0u, client_byte_order);
    x11_put_u16(root->width_px, X11_DEFAULT_ROOT_W, client_byte_order);
    x11_put_u16(root->height_px, X11_DEFAULT_ROOT_H, client_byte_order);
    x11_put_u16(root->width_mm, X11_DEFAULT_ROOT_W_MM, client_byte_order);
    x11_put_u16(root->height_mm, X11_DEFAULT_ROOT_H_MM, client_byte_order);
    x11_put_u16(root->min_installed_maps, 1u, client_byte_order);
    x11_put_u16(root->max_installed_maps, 1u, client_byte_order);
    x11_put_u32(root->root_visual_id, X11_ROOT_VISUAL_ID, client_byte_order);
    root->backing_stores = 2u;  /* Always */
    root->save_unders    = 0;
    root->root_depth     = 24;
    root->depths_len     = 1;

    /* Depth 24 with one TrueColor visual. */
    uint8_t *depthp = rootp + root_size;
    struct x11_depth *d = (struct x11_depth *)depthp;
    d->depth = 24;
    x11_put_u16(d->visuals_len, 1u, client_byte_order);

    struct x11_visualtype *vt =
        (struct x11_visualtype *)(depthp + sizeof(struct x11_depth));
    x11_put_u32(vt->visual_id, X11_ROOT_VISUAL_ID, client_byte_order);
    vt->class_ = 2u;  /* TrueColor */
    x11_put_u16(vt->colormap_entries, 256u, client_byte_order);
    x11_put_u32(vt->red_mask,   0x00FF0000u, client_byte_order);
    x11_put_u32(vt->green_mask, 0x0000FF00u, client_byte_order);
    x11_put_u32(vt->blue_mask,  0x000000FFu, client_byte_order);
    vt->bits_per_rgb_value = 8;

    return total;
}

size_t x11_build_error(uint8_t *out_buf, uint8_t error_code,
                       uint16_t sequence, uint32_t bad_value,
                       uint8_t major_opcode, uint8_t minor_opcode,
                       uint8_t byte_order) {
    if (out_buf == NULL) return 0;
    memset(out_buf, 0, 32);
    out_buf[0] = 0u;  /* error indicator */
    out_buf[1] = error_code;
    x11_put_u16(out_buf + 2, sequence, byte_order);
    x11_put_u32(out_buf + 4, bad_value, byte_order);
    out_buf[8]  = minor_opcode;
    out_buf[9]  = major_opcode;
    /* bytes 10..31 unused (already zeroed). */
    return 32;
}

size_t x11_build_get_geometry_reply(uint8_t *out_buf,
                                    uint8_t depth, uint32_t root_xid,
                                    int16_t x, int16_t y,
                                    uint16_t width, uint16_t height,
                                    uint16_t border_width,
                                    uint16_t sequence,
                                    uint8_t byte_order) {
    if (out_buf == NULL) return 0;
    memset(out_buf, 0, 32);
    out_buf[0] = 1u;  /* reply indicator */
    out_buf[1] = depth;
    x11_put_u16(out_buf + 2, sequence, byte_order);
    x11_put_u32(out_buf + 4, 0u, byte_order);  /* reply length = 0 */
    x11_put_u32(out_buf + 8, root_xid, byte_order);
    x11_put_u16(out_buf + 12, (uint16_t)x, byte_order);
    x11_put_u16(out_buf + 14, (uint16_t)y, byte_order);
    x11_put_u16(out_buf + 16, width, byte_order);
    x11_put_u16(out_buf + 18, height, byte_order);
    x11_put_u16(out_buf + 20, border_width, byte_order);
    /* bytes 22..31 unused (already zeroed). */
    return 32;
}

size_t x11_build_intern_atom_reply(uint8_t *out_buf, uint32_t atom,
                                   uint16_t sequence, uint8_t byte_order) {
    if (out_buf == NULL) return 0;
    memset(out_buf, 0, 32);
    out_buf[0] = 1u;  /* reply indicator */
    /* byte 1 unused */
    x11_put_u16(out_buf + 2, sequence, byte_order);
    x11_put_u32(out_buf + 4, 0u, byte_order);  /* reply length = 0 */
    x11_put_u32(out_buf + 8, atom, byte_order);
    /* bytes 12..31 unused (already zeroed). */
    return 32;
}

size_t x11_build_get_property_reply(uint8_t *out_buf, size_t out_cap,
                                    uint8_t format, uint32_t type,
                                    uint32_t bytes_after,
                                    uint32_t value_len,
                                    const uint8_t *value, size_t value_bytes,
                                    uint16_t sequence, uint8_t byte_order) {
    if (out_buf == NULL) return 0;
    size_t padded = x11_pad4(value_bytes);
    if (out_cap < 32u + padded) return 0;
    memset(out_buf, 0, 32u + padded);
    out_buf[0] = 1u;  /* reply indicator */
    out_buf[1] = format;
    x11_put_u16(out_buf + 2, sequence, byte_order);
    x11_put_u32(out_buf + 4, (uint32_t)(padded / 4u), byte_order);
    x11_put_u32(out_buf + 8, type, byte_order);
    x11_put_u32(out_buf + 12, bytes_after, byte_order);
    x11_put_u32(out_buf + 16, value_len, byte_order);
    /* bytes 20..31 unused */
    if (value_bytes > 0u && value != NULL) {
        memcpy(out_buf + 32, value, value_bytes);
    }
    return 32u + padded;
}

/* MapNotify (event type 19) wire layout:
 *   0   CARD8 code (19)
 *   1   BOOL override-redirect
 *   2   CARD16 sequence
 *   4   WINDOW event
 *   8   WINDOW window
 *   12..31 unused */
size_t x11_build_map_notify(uint8_t *out_buf,
                            uint32_t event, uint32_t window,
                            bool override_redirect,
                            uint16_t sequence, uint8_t byte_order) {
    if (out_buf == NULL) return 0;
    memset(out_buf, 0, 32);
    out_buf[0] = X11_EV_MAP_NOTIFY;
    out_buf[1] = override_redirect ? 1u : 0u;
    x11_put_u16(out_buf + 2, sequence, byte_order);
    x11_put_u32(out_buf + 4, event, byte_order);
    x11_put_u32(out_buf + 8, window, byte_order);
    return 32;
}

/* UnmapNotify (event type 18) wire layout:
 *   0   CARD8 code (18)
 *   1   BOOL from-configure
 *   2   CARD16 sequence
 *   4   WINDOW event
 *   8   WINDOW window
 *   12..31 unused */
size_t x11_build_unmap_notify(uint8_t *out_buf,
                              uint32_t event, uint32_t window,
                              bool from_configure,
                              uint16_t sequence, uint8_t byte_order) {
    if (out_buf == NULL) return 0;
    memset(out_buf, 0, 32);
    out_buf[0] = X11_EV_UNMAP_NOTIFY;
    out_buf[1] = from_configure ? 1u : 0u;
    x11_put_u16(out_buf + 2, sequence, byte_order);
    x11_put_u32(out_buf + 4, event, byte_order);
    x11_put_u32(out_buf + 8, window, byte_order);
    return 32;
}

/* ConfigureNotify (event type 22) wire layout:
 *   0   CARD8 code (22)
 *   1   unused
 *   2   CARD16 sequence
 *   4   WINDOW event
 *   8   WINDOW window
 *   12  WINDOW above-sibling
 *   16  INT16 x
 *   18  INT16 y
 *   20  CARD16 width
 *   22  CARD16 height
 *   24  CARD16 border-width
 *   26  BOOL override-redirect
 *   27..31 unused */
size_t x11_build_configure_notify(uint8_t *out_buf,
                                  uint32_t event, uint32_t window,
                                  uint32_t above_sibling,
                                  int16_t x, int16_t y,
                                  uint16_t width, uint16_t height,
                                  uint16_t border_width,
                                  bool override_redirect,
                                  uint16_t sequence, uint8_t byte_order) {
    if (out_buf == NULL) return 0;
    memset(out_buf, 0, 32);
    out_buf[0] = X11_EV_CONFIGURE_NOTIFY;
    x11_put_u16(out_buf + 2, sequence, byte_order);
    x11_put_u32(out_buf + 4, event, byte_order);
    x11_put_u32(out_buf + 8, window, byte_order);
    x11_put_u32(out_buf + 12, above_sibling, byte_order);
    x11_put_u16(out_buf + 16, (uint16_t)x, byte_order);
    x11_put_u16(out_buf + 18, (uint16_t)y, byte_order);
    x11_put_u16(out_buf + 20, width, byte_order);
    x11_put_u16(out_buf + 22, height, byte_order);
    x11_put_u16(out_buf + 24, border_width, byte_order);
    out_buf[26] = override_redirect ? 1u : 0u;
    return 32;
}

/* DestroyNotify (event type 17) wire layout:
 *   0   CARD8 code (17)
 *   1   unused
 *   2   CARD16 sequence
 *   4   WINDOW event
 *   8   WINDOW window
 *   12..31 unused */
size_t x11_build_destroy_notify(uint8_t *out_buf,
                                uint32_t event, uint32_t window,
                                uint16_t sequence, uint8_t byte_order) {
    if (out_buf == NULL) return 0;
    memset(out_buf, 0, 32);
    out_buf[0] = X11_EV_DESTROY_NOTIFY;
    x11_put_u16(out_buf + 2, sequence, byte_order);
    x11_put_u32(out_buf + 4, event, byte_order);
    x11_put_u32(out_buf + 8, window, byte_order);
    return 32;
}

/* PropertyNotify (event type 28) wire layout:
 *   0   CARD8 code (28)
 *   1   unused
 *   2   CARD16 sequence
 *   4   WINDOW window
 *   8   ATOM atom
 *   12  TIMESTAMP time
 *   16  CARD8 state (0 NewValue, 1 Deleted)
 *   17..31 unused */
size_t x11_build_property_notify(uint8_t *out_buf,
                                 uint32_t window, uint32_t atom,
                                 uint32_t time, uint8_t state,
                                 uint16_t sequence, uint8_t byte_order) {
    if (out_buf == NULL) return 0;
    memset(out_buf, 0, 32);
    out_buf[0] = X11_EV_PROPERTY_NOTIFY;
    x11_put_u16(out_buf + 2, sequence, byte_order);
    x11_put_u32(out_buf + 4, window, byte_order);
    x11_put_u32(out_buf + 8, atom, byte_order);
    x11_put_u32(out_buf + 12, time, byte_order);
    out_buf[16] = state;
    return 32;
}
