/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* x11_client.h: per-X11-client state.
 *
 * One of these is created when an X11 client connects to the bridge's
 * listening socket. It owns the per-client socket fd, the client's
 * chosen byte order, the per-client XID range (resource-id-base +
 * resource-id-mask), the sequence counter, a partial-read buffer for
 * incomplete requests, and a small table mapping X11 window ids to
 * bridge-tracked window state.
 *
 * W9-B: the bridge handles twenty core opcodes end-to-end. The
 * ten W8-A opcodes plus GetWindowAttributes, QueryTree, GetAtomName,
 * DeleteProperty, SetSelectionOwner, GetSelectionOwner, QueryPointer,
 * SetInputFocus, CreateGC, PutImage. Per-client state gained a
 * graphics-context table (CreateGC), a selection-ownership table
 * (SetSelectionOwner), keyboard focus state (SetInputFocus), and a
 * per-window PutImage backing store.
 *
 * W10-B: five colormap opcodes added (CreateColormap, FreeColormap,
 * AllocColor, QueryColors, LookupColor). Per-client state gained a
 * colormap table. The bridge does no real color allocation; AllocColor
 * echoes the requested RGB and returns a packed pixel, QueryColors
 * unpacks each pixel as 0x00RRGGBB, and LookupColor returns black.
 *
 * W11-A: twelve font and text opcodes added (OpenFont, CloseFont,
 * QueryFont, QueryTextExtents, ListFonts, ListFontsWithInfo,
 * SetFontPath, GetFontPath, PolyText8, PolyText16, ImageText8,
 * ImageText16). Per-client state gained a font table. The bridge ships
 * no font database: OpenFont stores the name only, QueryFont and
 * QueryTextExtents return all-zero metrics, ListFonts returns an empty
 * list, ListFontsWithInfo sends only the terminator reply, GetFontPath
 * returns npaths=0, and SetFontPath / PolyText* / ImageText* accept
 * and discard their payloads. BadFont is sent when QueryFont /
 * QueryTextExtents target a font XID the bridge does not track.
 *
 * W10-A: five rendering opcodes added (FreeGC, ClearArea, CopyArea,
 * PolyFillRectangle, GetImage). Per-window state gained a
 * background_pixel field so ClearArea can paint with it. GetImage
 * reads back from the PutImage backing store; CopyArea and
 * PolyFillRectangle no-op the pixel copy in v1.
 *
 * W11-B: five cursor / misc opcodes added (CreateCursor,
 * CreateGlyphCursor, FreeCursor, RecolorCursor, QueryBestSize).
 * Per-client state gained a cursor table. The bridge does no real
 * cursor rendering; CreateCursor / CreateGlyphCursor store the
 * source (pixmap or font) and colors so RecolorCursor can update
 * them and FreeCursor can release the slot. QueryBestSize echoes
 * back the requested width and height unchanged because the bridge
 * has no hardware cursor / tile / stipple size limits. */

#ifndef X11_CLIENT_H
#define X11_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "x11_proto.h"

#define X11_CLIENT_RECV_BUF   16384
#define X11_CLIENT_MAX_WIN    64   /* windows tracked per client */
#define X11_CLIENT_MAX_PROPS  16   /* properties per window */
#define X11_PROP_MAX_BYTES    4096 /* per-property value cap */
#define X11_CLIENT_MAX_PIX    32   /* pixmaps tracked per client */
#define X11_CLIENT_MAX_GCS    32   /* graphics contexts per client */
#define X11_CLIENT_MAX_SELS   8    /* selection ownerships per client */
#define X11_CLIENT_MAX_CMAPS  16   /* colormaps tracked per client */
#define X11_CLIENT_MAX_FONTS  16   /* fonts tracked per client */
#define X11_CLIENT_MAX_CURSORS 16  /* cursors tracked per client */
#define X11_FONT_NAME_MAX     256  /* per-font name buffer */
#define X11_PUT_IMAGE_MAX_BYTES  (64u * 1024u)  /* per-PutImage data cap */

/* Per-window property: (property atom, type atom, format, value bytes).
 * Format is 8/16/32. The value is stored as raw bytes; the caller
 * reads/writes format-sized units. */
struct x11_property {
    bool      in_use;
    uint32_t  property;   /* atom */
    uint32_t  type;       /* atom */
    uint8_t   format;     /* 8, 16, or 32 */
    size_t    value_len;  /* in bytes */
    uint8_t  *value;      /* malloc'd; freed when overwritten or window destroyed */
};

struct x11_window {
    uint32_t  x11_id;          /* X11 window id (client-chosen) */
    uint32_t  isz_surface_id;  /* bridge-allocated Ishizue surface id */
    int32_t   x, y;
    int32_t   w, h;
    uint16_t  border_width;
    uint8_t   depth;
    uint8_t   window_class;    /* 0 CopyFromParent, 1 InputOutput, 2 InputOnly */
    uint32_t  visual_id;
    uint32_t  event_mask;      /* X11 SETofEVENT the client asked for */
    uint32_t  parent_xid;      /* root for top-level, real parent otherwise */
    uint32_t  cursor_xid;      /* 0 None means inherit from parent */
    int32_t   zpos;            /* stacking order; bridge-assigned */
    bool      override_redirect;
    bool      mapped;
    bool      has_surface;     /* true once an Ishizue surface has been created */
    bool      has_output;      /* true once set_output has been sent */
    bool      in_use;

    /* W9-B: GetWindowAttributes fields the client can set via the
     * CreateWindow / ChangeWindowAttributes value-list. Defaults
     * follow X11: bit_gravity=Forget(0), win_gravity=NorthWest(1),
     * backing_store=NotUseful(0), backing_planes=AllPlanes(~0),
     * backing_pixel=0, save_under=false, colormap=0
     * (CopyFromParent), do_not_propagate_mask=0.
     *
     * W10-A: background_pixel (CW_BG_PIXEL) is tracked so ClearArea
     * can paint with it. Defaults to 0 (black), matching the X11
     * spec when the client sets no background. */
    uint8_t   bit_gravity;
    uint8_t   win_gravity;
    uint8_t   backing_store;
    uint32_t  backing_planes;
    uint32_t  backing_pixel;
    uint32_t  background_pixel;
    uint32_t  colormap;
    bool      save_under;
    uint16_t  do_not_propagate_mask;

    /* W9-B: PutImage backing store. The bridge stashes the last
     * PutImage bytes for the drawable so a later commit could attach
     * them via isz_surface_attach_buffer. v1 stores only; the
     * hardware-attach path is post-v1. */
    uint8_t  *backing_image;
    size_t    backing_image_len;
    uint16_t  backing_image_w;
    uint16_t  backing_image_h;
    uint8_t   backing_image_depth;
    uint8_t   backing_image_format;  /* X11_IMAGE_FORMAT_* */

    /* Property list. Linear scan; v1 client property counts are small. */
    struct x11_property props[X11_CLIENT_MAX_PROPS];
};

/* W9-B: per-client graphics context. Created by CreateGC (55).
 * Stores only the fields the bridge inspects later; the rest of the
 * value-list is accepted and discarded. graphics_exposure drives
 * whether PutImage / CopyArea emit GraphicsExposure / NoExpose. */
struct x11_gc {
    bool      in_use;
    uint32_t  gc_xid;
    uint32_t  drawable_xid;  /* the drawable CreateGC was issued on */
    uint32_t  value_mask;    /* the mask the client set */
    uint32_t  foreground;
    uint32_t  background;
    uint8_t   function;
    uint16_t  line_width;
    uint8_t   line_style;
    uint8_t   cap_style;
    uint8_t   join_style;
    uint8_t   fill_style;
    uint8_t   fill_rule;
    uint8_t   subwindow_mode;
    bool      graphics_exposure;
    int16_t   clip_x_origin;
    int16_t   clip_y_origin;
    uint8_t   arc_mode;
};

/* W9-B: per-client selection ownership entry. Keyed by selection
 * atom. SetSelectionOwner updates this table and emits SelectionClear
 * to the previous owner when ownership changes. */
struct x11_selection {
    bool      in_use;
    uint32_t  selection_atom;
    uint32_t  owner_xid;
    uint32_t  timestamp;
};

/* Pixmap tracking. Pixmaps are drawables that the client can CreateGC
 * on, PutImage into, CopyArea from, etc. The bridge stores only the
 * geometry; pixel data is not retained (no rendering in v1). */
struct x11_pixmap {
    bool      in_use;
    uint32_t  pixmap_xid;
    uint32_t  drawable_xid;   /* the drawable CreatePixmap was issued on */
    uint16_t  width;
    uint16_t  height;
    uint8_t   depth;
};

/* W10-B: per-client colormap. Created by CreateColormap (78). The
 * bridge does no real color allocation; AllocColor and QueryColors
 * echo back the requested values, and LookupColor returns black for
 * every name. visual_id is stored for GetWindowAttributes parity but
 * the bridge does not enforce that AllocColor targets the right
 * visual. alloc_flag is 0 (AllocNone) or 1 (AllocAll). */
struct x11_colormap {
    bool      in_use;
    uint32_t  colormap_xid;   /* client-allocated XID */
    uint32_t  window_xid;     /* window passed to CreateColormap */
    uint32_t  visual_id;
    uint8_t   alloc_flag;     /* X11_ALLOC_NONE or X11_ALLOC_ALL */
};

/* W11-A: per-client font. Created by OpenFont (45). The bridge does no
 * real font loading and no text rendering in v1; the name is tracked so
 * QueryFont / ListFonts can validate the font XID. CloseFont marks the
 * slot free. PolyText8/16 and ImageText8/16 accept and discard their
 * payloads. name is NUL-terminated; name_len is the byte count without
 * the terminator. */
struct x11_font {
    bool      in_use;
    uint32_t  font_xid;            /* client-allocated XID */
    size_t    name_len;            /* bytes in name (no terminator) */
    char      name[X11_FONT_NAME_MAX];
};

/* W11-B: per-client cursor. Created by CreateCursor (93) or
 * CreateGlyphCursor (94). The bridge does no real cursor rendering in
 * v1; the cursor entry exists so RecolorCursor can update the colors
 * and FreeCursor can release the slot. is_glyph distinguishes the two
 * sources: when false, source_pixmap / mask_pixmap hold the source
 * pixmaps (mask_pixmap may be 0 = None per X11); when true,
 * source_font / mask_font hold the font XIDs (mask_font may be 0) and
 * source_char / mask_char hold the glyph indices. fore_rgb / back_rgb
 * are the cursor colors; hotspot_x / hotspot_y are the cursor hotspot
 * (only meaningful for pixmap cursors; glyph cursors inherit the
 * glyph origin). */
struct x11_cursor {
    bool      in_use;
    uint32_t  cursor_xid;          /* client-allocated XID */
    bool      is_glyph;            /* false = pixmap source, true = font source */
    uint32_t  source_pixmap;       /* CreateCursor source pixmap */
    uint32_t  mask_pixmap;         /* CreateCursor mask pixmap (0 = None) */
    uint32_t  source_font;         /* CreateGlyphCursor source font */
    uint32_t  mask_font;           /* CreateGlyphCursor mask font (0 = None) */
    uint16_t  source_char;         /* CreateGlyphCursor source glyph */
    uint16_t  mask_char;           /* CreateGlyphCursor mask glyph */
    uint16_t  fore_red, fore_green, fore_blue;
    uint16_t  back_red, back_green, back_blue;
    uint16_t  hotspot_x, hotspot_y;
};

struct x11_client {
    int      fd;
    bool     setup_done;          /* setup_request received, success sent */
    uint8_t  byte_order;          /* X11_BYTE_ORDER_LSB or _MSB */
    uint16_t sequence;            /* next reply/event sequence number */

    /* Per-client XID range, advertised in SetupSuccess. The client
     * allocates its own XIDs via (base | (counter++ & mask)). The
     * bridge tracks the same (base, mask) so it can sanity-check
     * client-chosen XIDs (currently advisory only). */
    uint32_t xid_base;
    uint32_t xid_mask;
    uint32_t next_xid;            /* bridge-side counter (currently unused) */

    /* Partial-read buffer. Requests come in 4-byte units; we
     * accumulate bytes here until we have at least one full request,
     * then dispatch and shift. */
    uint8_t  buf[X11_CLIENT_RECV_BUF];
    size_t   have;                /* valid bytes in buf */

    /* Window table: X11 window id -> bridge state. Indexed by slot;
     * linear scan on lookup. v1 client window counts are small. */
    struct x11_window windows[X11_CLIENT_MAX_WIN];

    /* Pixmap table: X11 pixmap id -> geometry. v1 stores no pixel
     * data; rendering is deferred. */
    struct x11_pixmap pixmaps[X11_CLIENT_MAX_PIX];

    /* W9-B: graphics-context table. Keyed by client-allocated GC
     * XID. Linear scan; v1 client GC counts are small. */
    struct x11_gc gcs[X11_CLIENT_MAX_GCS];

    /* W9-B: selection ownership table. Keyed by selection atom.
     * Bridge-global in X11 terms; v1 keeps it per-client because the
     * bridge has no inter-client event delivery today. A future
     * multi-client wave will hoist this into a process-global table. */
    struct x11_selection selections[X11_CLIENT_MAX_SELS];

    /* W10-B: colormap table. Keyed by client-allocated colormap XID.
     * The bridge does no real color allocation; the table exists so
     * AllocColor / QueryColors / LookupColor / FreeColormap can
     * validate the colormap XID and reject unknown ones with
     * BadColormap. */
    struct x11_colormap colormaps[X11_CLIENT_MAX_CMAPS];

    /* W11-A: font table. Keyed by client-allocated font XID. The
     * bridge ships no font database; OpenFont stores the name only so
     * QueryFont / QueryTextExtents can validate the font XID. Text
     * drawing opcodes (PolyText8/16, ImageText8/16) accept and discard
     * their payloads. */
    struct x11_font fonts[X11_CLIENT_MAX_FONTS];

    /* W11-B: cursor table. Keyed by client-allocated cursor XID. The
     * bridge does no real cursor rendering; the table exists so
     * RecolorCursor can update the colors and FreeCursor can release
     * the slot. CreateCursor stores pixmap source, CreateGlyphCursor
     * stores font source. */
    struct x11_cursor cursors[X11_CLIENT_MAX_CURSORS];

    /* W9-B: keyboard focus state, set by SetInputFocus (42). The
     * bridge translates focus to ISZ_MSG_SEAT_SET_KEYBOARD_FOCUS.
     * focus_xid is the focused window (0 None, 1 PointerRoot),
     * revert_to is the policy for when the focused window closes. */
    uint32_t focus_xid;
    uint8_t  focus_revert_to;
};

struct isz_client;  /* forward declaration */

/* Take ownership of fd. Returns NULL on failure (fd closed). */
struct x11_client *x11_client_create(int fd);
void               x11_client_destroy(struct x11_client *c);

int  x11_client_fd(const struct x11_client *c);

/* W9-B: look up a tracked window by X11 id. Exposed so the focus
 * and selection code in translation.c can resolve an XID to its
 * isz_surface_id without poking the table directly. Returns NULL
 * if the window is not tracked by this client. */
struct x11_window *x11_client_find_window_by_xid(struct x11_client *c,
                                                 uint32_t x11_id);

/* Drains pending bytes from the socket into c->buf, then parses as
 * many complete requests as possible. Each parsed request is routed
 * to translation.c, which uses `isz` to emit Ishizue wire messages.
 * Returns 0 on success (including EAGAIN), -1 on EOF or hard error
 * (caller should destroy the client). */
int  x11_client_drain(struct x11_client *c, struct isz_client *isz);

/* Send a 32-byte X11 event to this client. Used by the translation
 * layer to forward Ishizue input events. Returns 0 on success, -1 on
 * failure. */
int  x11_client_send_event(struct x11_client *c,
                           const struct x11_event_32 *ev);

/* Look up the first mapped window on this client, or NULL if there
 * is none. Used to route incoming Ishizue input events. */
struct x11_window *x11_client_first_mapped(struct x11_client *c);

/* Bump the sequence counter and return its previous value, in the
 * client's byte order, for use in event/reply headers. */
uint16_t x11_client_next_sequence(struct x11_client *c);

#endif /* X11_CLIENT_H */
