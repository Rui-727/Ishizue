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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* isz_drm_edid.c -- EDID reader and HDR_STATIC_METADATA parser, SPEC 7.2.
 *
 * The library reads the raw EDID blob from the DRM connector, validates
 * its checksum, and only parses HDR_STATIC_METADATA out of it. Every
 * other field is exposed to the Architect as raw opaque bytes via
 * isz_output_get_edid(). */
#include "isz_drm_edid.h"

#ifdef ISHIZUE_HAVE_DRM

#include "../util/isz_log.h"

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <stdlib.h>
#include <string.h>

#define ISZ_EDID_BLOCK_SIZE 128u
#define ISZ_EDID_CTA_TAG    0x02u  /* CTA-861 extension block tag */
#define ISZ_HDR_DB_TAG      0x06u  /* HDR Static Metadata data block tag */

int isz_drm_edid_read(int drm_fd, uint32_t connector_id,
                      uint8_t **out_edid, size_t *out_size)
{
    if (out_edid) *out_edid = NULL;
    if (out_size) *out_size = 0;
    if (drm_fd < 0 || connector_id == 0)
        return -1;

    /* Look up the EDID property on the connector. */
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(drm_fd, connector_id,
                                   DRM_MODE_OBJECT_CONNECTOR);
    if (!props)
        return -1;

    uint32_t edid_prop_id = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(drm_fd, props->props[i]);
        if (!p)
            continue;
        if (strcmp(p->name, "EDID") == 0) {
            edid_prop_id = props->props[i];
            drmModeFreeProperty(p);
            break;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);

    if (!edid_prop_id)
        return 0;  /* no EDID property; not an error */

    /* Pull the blob. drmModeGetPropertyBlob returns a struct with a
     * data pointer and length; we copy the bytes so we can free the
     * libdrm handle immediately. */
    drmModePropertyBlobRes *blob =
        drmModeGetPropertyBlob(drm_fd, edid_prop_id);
    if (!blob)
        return 0;

    if (blob->length == 0 || blob->length % ISZ_EDID_BLOCK_SIZE != 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "drm edid: connector %u blob length %lu invalid",
                         connector_id, (unsigned long)blob->length);
        drmModeFreePropertyBlob(blob);
        return -1;
    }

    if (out_edid && out_size) {
        uint8_t *copy = malloc(blob->length);
        if (!copy) {
            drmModeFreePropertyBlob(blob);
            return -1;
        }
        memcpy(copy, blob->data, blob->length);
        *out_edid = copy;
        *out_size = blob->length;
    }

    drmModeFreePropertyBlob(blob);
    return 0;
}

bool isz_drm_edid_checksum_ok(const uint8_t *edid, size_t size)
{
    if (!edid || size == 0 || size % ISZ_EDID_BLOCK_SIZE != 0)
        return false;

    /* Each 128-byte block has its own checksum: XOR of all 128 bytes
     * should be zero. */
    for (size_t off = 0; off < size; off += ISZ_EDID_BLOCK_SIZE) {
        uint8_t sum = 0;
        for (size_t i = 0; i < ISZ_EDID_BLOCK_SIZE; i++)
            sum ^= edid[off + i];
        if (sum != 0)
            return false;
    }
    return true;
}

/* Walk the CTA-861 extension block's data block collection looking for
 * the HDR Static Metadata data block (tag 0x06). The CTA block format:
 *
 *   byte 0: tag (0x02) -- this is the EDID extension tag, not the DB tag
 *   byte 1: extension revision (3 for CTA-861-F+)
 *   byte 2: offset to detailed timing data
 *   byte 3: number of native DTDs (low 4 bits)
 *   byte 4+: data block collection, each DB prefixed by:
 *       bits 7..5: tag
 *       bits 4..0: length (in bytes, excluding the tag byte)
 */
int isz_drm_edid_parse_hdr(const uint8_t *edid, size_t size,
                           bool *out_present,
                           uint8_t *out_buf, size_t out_buf_cap,
                           size_t *out_buf_len)
{
    if (out_present) *out_present = false;
    if (out_buf_len) *out_buf_len = 0;
    if (!edid || size < ISZ_EDID_BLOCK_SIZE * 2u)
        return 0;  /* only one base block, no extensions */

    size_t nblocks = size / ISZ_EDID_BLOCK_SIZE;
    for (size_t b = 1; b < nblocks; b++) {
        const uint8_t *block = edid + b * ISZ_EDID_BLOCK_SIZE;
        if (block[0] != ISZ_EDID_CTA_TAG)
            continue;  /* not a CTA-861 extension */

        /* Data blocks start at offset 4 within the CTA block. */
        size_t dtd_offset = block[2];
        size_t i = 4;
        size_t end = dtd_offset;
        if (end > ISZ_EDID_BLOCK_SIZE)
            end = ISZ_EDID_BLOCK_SIZE;

        while (i < end) {
            uint8_t db_tag = (block[i] >> 5) & 0x07;
            uint8_t db_len = block[i] & 0x1f;
            i++;
            if (i + db_len > end)
                break;

            if (db_tag == ISZ_HDR_DB_TAG) {
                if (out_present) *out_present = true;
                if (out_buf && out_buf_cap > 0) {
                    size_t n = db_len;
                    if (n > out_buf_cap) n = out_buf_cap;
                    memcpy(out_buf, block + i, n);
                    if (out_buf_len) *out_buf_len = n;
                }
                return 0;
            }
            i += db_len;
        }
    }
    return 0;
}

#endif /* ISHIZUE_HAVE_DRM */
