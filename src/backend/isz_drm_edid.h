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

/* isz_drm_edid.h -- EDID reader and HDR_STATIC_METADATA parser, SPEC 7.2.
 *
 * Reads the raw EDID blob from a DRM connector, validates its checksum,
 * and parses only the HDR_STATIC_METADATA block out of any CTA-861
 * extension. Everything else stays opaque for isz_output_get_edid(). */
#ifndef ISZ_BACKEND_DRM_EDID_H
#define ISZ_BACKEND_DRM_EDID_H

#include <stddef.h>
#include <stdint.h>

#include "isz_backend.h"

#ifdef ISHIZUE_HAVE_DRM

struct isz_drm_state;

/* Read the EDID blob from the connector into *out_edid / *out_size.
 * Caller frees *out_edid. Returns 0 on success, negative errno on
 * failure. A missing EDID property is not an error: returns 0 and
 * leaves *out_edid NULL. */
int isz_drm_edid_read(int drm_fd, uint32_t connector_id,
                      uint8_t **out_edid, size_t *out_size);

/* Validate the EDID block checksum (XOR of all 128 bytes == 0). Returns
 * true if valid, false otherwise. */
bool isz_drm_edid_checksum_ok(const uint8_t *edid, size_t size);

/* Parse HDR_STATIC_METADATA from the EDID. Sets *out_present to true if
 * the block was found and copies the raw block bytes (up to 28) into
 * out_buf. Returns 0 on success (including "no HDR block found", which
 * sets *out_present=false), negative on a malformed EDID. */
int isz_drm_edid_parse_hdr(const uint8_t *edid, size_t size,
                           bool *out_present,
                           uint8_t *out_buf, size_t out_buf_cap,
                           size_t *out_buf_len);

#endif /* ISHIZUE_HAVE_DRM */

#endif /* ISZ_BACKEND_DRM_EDID_H */
