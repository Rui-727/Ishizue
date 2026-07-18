/* SPDX-License-Identifier: MIT
 *
 * Ishizue (礎) - xkbcommon keymap cache and modifier tracking. Wave 1-E.
 *
 * §9: the keymap is compiled once per layout change and cached per
 * seat; it is not recompiled per keystroke. On each key event we feed
 * the keycode to xkb_state_update_key and, if the serialized modifier
 * or layout state changed, emit a keyboard_modifiers event carrying
 * mods_depressed / mods_latched / mods_locked / group.
 *
 * The library does no keysym translation; that stays with the
 * Architect. Without ISHIZUE_HAVE_XKBCOMMON the entry points are
 * stubs that record the requested layout name for later and return
 * ISZ_ERR_FEATURE_UNAVAIL.
 */
#include "isz_seat_internal.h"

#include <stdio.h>
#include <string.h>

#ifndef ISHIZUE_HAVE_XKBCOMMON

int isz_keymap_set_layout(isz_seat *seat, const char *layout,
                          const char *variant) {
    if (!seat)
        return ISZ_ERR_INVALID_ARG;
    snprintf(seat->layout, sizeof(seat->layout), "%s", layout ? layout : "");
    snprintf(seat->variant, sizeof(seat->variant), "%s", variant ? variant : "");
    isz_log_internal(ISZ_LOG_DEBUG, "keymap_set_layout: xkbcommon not built in");
    return ISZ_ERR_FEATURE_UNAVAIL;
}

void *isz_keymap_get_state(isz_seat *seat) {
    (void)seat;
    return NULL;
}

void isz_keymap_handle_key(isz_seat *seat, uint32_t keycode, bool pressed,
                           uint64_t time_ns, isz_server *srv) {
    (void)seat; (void)keycode; (void)pressed; (void)time_ns; (void)srv;
}

#else  /* ISHIZUE_HAVE_XKBCOMMON */

static void keymap_drop(struct isz_seat *seat) {
    if (seat->xkb_state) {
        xkb_state_unref(seat->xkb_state);
        seat->xkb_state = NULL;
    }
    if (seat->xkb_keymap) {
        xkb_keymap_unref(seat->xkb_keymap);
        seat->xkb_keymap = NULL;
    }
    seat->mods_depressed = 0;
    seat->mods_latched   = 0;
    seat->mods_locked    = 0;
    seat->group          = 0;
}

int isz_keymap_set_layout(isz_seat *seat, const char *layout,
                          const char *variant) {
    if (!seat)
        return ISZ_ERR_INVALID_ARG;

    if (!seat->xkb_ctx) {
        seat->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!seat->xkb_ctx)
            return ISZ_ERR_NO_MEMORY;
    }

    snprintf(seat->layout,  sizeof(seat->layout),  "%s", layout  ? layout  : "");
    snprintf(seat->variant, sizeof(seat->variant), "%s", variant ? variant : "");

    struct xkb_rule_names names = {
        .rules   = "evdev",
        .model   = "pc104",
        .layout  = seat->layout,
        .variant = seat->variant,
        .options = NULL,
    };
    struct xkb_keymap *km = xkb_keymap_new_from_names(
        seat->xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) {
        isz_log_internal(ISZ_LOG_DEBUG, "xkb_keymap_new_from_names failed");
        return ISZ_ERR_FEATURE_UNAVAIL;
    }

    keymap_drop(seat);
    seat->xkb_keymap = km;
    seat->xkb_state  = xkb_state_new(km);
    if (!seat->xkb_state) {
        keymap_drop(seat);
        return ISZ_ERR_NO_MEMORY;
    }
    return ISZ_OK;
}

void *isz_keymap_get_state(isz_seat *seat) {
    return seat ? seat->xkb_state : NULL;
}

void isz_keymap_handle_key(isz_seat *seat, uint32_t keycode, bool pressed,
                           uint64_t time_ns, isz_server *srv) {
    if (!seat || !seat->xkb_state)
        return;

    /* libinput reports evdev keycodes; xkb_common expects keycode+8. */
    xkb_state_update_key(seat->xkb_state, keycode + 8,
                         pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

    uint32_t dep = xkb_state_serialize_mods(seat->xkb_state,
                                            XKB_STATE_MODS_DEPRESSED);
    uint32_t lat = xkb_state_serialize_mods(seat->xkb_state,
                                            XKB_STATE_MODS_LATCHED);
    uint32_t lck = xkb_state_serialize_mods(seat->xkb_state,
                                            XKB_STATE_MODS_LOCKED);
    uint32_t grp = xkb_state_serialize_layout(seat->xkb_state,
                                              XKB_STATE_LAYOUT_EFFECTIVE);

    if (dep == seat->mods_depressed && lat == seat->mods_latched &&
        lck == seat->mods_locked   && grp == seat->group)
        return;

    seat->mods_depressed = dep;
    seat->mods_latched   = lat;
    seat->mods_locked    = lck;
    seat->group          = grp;

    isz_event e = { .type = ISZ_EVENT_INPUT_KEYBOARD_MODIFIERS,
                    .time_ns = time_ns };
    e.u.keyboard_modifiers.mods_depressed = dep;
    e.u.keyboard_modifiers.mods_latched   = lat;
    e.u.keyboard_modifiers.mods_locked    = lck;
    e.u.keyboard_modifiers.group          = grp;
    isz_server_emit_event(srv, &e);
}

#endif  /* ISHIZUE_HAVE_XKBCOMMON */
