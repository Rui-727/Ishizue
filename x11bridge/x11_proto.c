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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* x11_proto.c: minimal X11 wire protocol primitives. */

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

size_t x11_build_setup_success(uint8_t *out_buf, size_t out_cap,
                               uint8_t client_byte_order,
                               uint16_t server_proto_major,
                               uint16_t server_proto_minor) {
    /* Layout: 40-byte fixed header + 0-byte vendor + 1 pixmap format
     * (8 bytes) + 1 root screen (40 bytes) = 88 bytes total. The
     * length field counts 4-byte units after the first 8 bytes, so
     * length = (88 - 8) / 4 = 20. */
    const size_t total = sizeof(struct x11_setup_success) +
                         sizeof(struct x11_pixmap_format) +
                         sizeof(struct x11_root_screen);
    if (out_cap < total) {
        return 0;
    }

    memset(out_buf, 0, total);

    struct x11_setup_success *hdr =
        (struct x11_setup_success *)out_buf;
    hdr->status              = X11_SETUP_SUCCESS;
    x11_put_u16(hdr->proto_major, server_proto_major, client_byte_order);
    x11_put_u16(hdr->proto_minor, server_proto_minor, client_byte_order);
    x11_put_u16(hdr->length, (uint16_t)((total - 8u) / 4u),
                client_byte_order);
    x11_put_u32(hdr->release_number, 0u, client_byte_order);
    x11_put_u32(hdr->resource_id_base, 1u, client_byte_order);
    x11_put_u32(hdr->resource_id_mask, 0x00FFFFFFu, client_byte_order);
    x11_put_u32(hdr->motion_buffer_size, 0u, client_byte_order);
    x11_put_u16(hdr->vendor_len, 0u, client_byte_order);
    x11_put_u16(hdr->max_request_length, 0xFFFFu, client_byte_order);
    hdr->roots_len            = 1;
    hdr->pixmap_formats_len   = 1;
    hdr->image_byte_order     =
        (client_byte_order == X11_BYTE_ORDER_MSB) ? 1u : 0u;
    hdr->bitmap_bit_order     = hdr->image_byte_order;
    hdr->bitmap_scanline_unit = 32;
    hdr->bitmap_scanline_pad  = 32;
    hdr->min_keycode          = 8;
    hdr->max_keycode          = 255;

    /* Pixmap format: depth 24, 32 bits per pixel, scanline pad 32. */
    struct x11_pixmap_format *fmt =
        (struct x11_pixmap_format *)(out_buf + sizeof(struct x11_setup_success));
    fmt->depth          = 24;
    fmt->bits_per_pixel = 32;
    fmt->scanline_pad   = 32;

    /* Root screen. */
    struct x11_root_screen *root =
        (struct x11_root_screen *)(out_buf + sizeof(struct x11_setup_success) +
                                             sizeof(struct x11_pixmap_format));
    x11_put_u32(root->root_window_id, 1u, client_byte_order);
    x11_put_u32(root->default_colormap, 0u, client_byte_order);
    x11_put_u32(root->white_pixel, 0x00FFFFFFu, client_byte_order);
    x11_put_u32(root->black_pixel, 0u, client_byte_order);
    x11_put_u32(root->current_input_masks, 0u, client_byte_order);
    x11_put_u16(root->width_px, 1024u, client_byte_order);
    x11_put_u16(root->height_px, 768u, client_byte_order);
    x11_put_u16(root->width_mm, 200u, client_byte_order);
    x11_put_u16(root->height_mm, 150u, client_byte_order);
    x11_put_u16(root->min_installed_maps, 1u, client_byte_order);
    x11_put_u16(root->max_installed_maps, 1u, client_byte_order);
    x11_put_u32(root->root_visual_id, 1u, client_byte_order);
    root->backing_stores = 0;
    root->save_unders    = 0;
    root->root_depth     = 24;
    root->depths_len     = 0;

    return total;
}
