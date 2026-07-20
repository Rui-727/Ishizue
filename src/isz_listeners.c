/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Rui-727
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction including without limitation
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* isz_listeners.c - listener registry (SPEC §5).
 *
 * One intrusive list per isz_event_type, stored on the server. Add
 * appends to the tail so listeners fire in registration order. Remove
 * walks the list and unlinks every node whose fn matches; userdata is
 * not matched (SPEC doesn't specify, simplicity wins).
 *
 * isz_server_emit_event walks the list for ev->type and calls each
 * listener. NULL srv or out-of-range type is a silent no-op so every
 * subsystem can call it without gating. Listeners are called only on
 * the main dispatch thread (the only thread that calls isz_dispatch),
 * so the registry itself is unsynchronized. */
#include "isz_server_internal.h"

#include <stdlib.h>

#include "util/isz_log.h"

/* Map an event_type to its bucket. Event types are 1..24 today; the
 * array has 32 slots. Anything out of range falls into bucket 0,
 * which is always empty (no event_type uses 0), so the emit becomes
 * a no-op rather than an out-of-bounds read. */
static size_t listener_bucket(enum isz_event_type t)
{
    size_t i = (size_t)t;
    return (i < ISZ_LISTENER_BUCKETS) ? i : 0;
}

ISZ_API int isz_add_listener(isz_server *srv, enum isz_event_type type,
                             isz_event_listener_fn fn, void *userdata)
{
    if (!srv || !fn)
        return ISZ_ERR_INVALID_ARG;
    if (type <= 0)
        return ISZ_ERR_INVALID_ARG;

    struct isz_listener *l = malloc(sizeof(*l));
    if (!l) {
        isz_log_internal(ISZ_LOG_ERROR, "isz_add_listener: out of memory");
        return ISZ_ERR_NO_MEMORY;
    }
    l->fn       = fn;
    l->userdata = userdata;

    isz_list_push_back(&srv->listeners[listener_bucket(type)], &l->node);
    return ISZ_OK;
}

ISZ_API int isz_remove_listener(isz_server *srv, enum isz_event_type type,
                                isz_event_listener_fn fn)
{
    if (!srv || !fn)
        return ISZ_ERR_INVALID_ARG;
    if (type <= 0)
        return ISZ_ERR_INVALID_ARG;

    isz_list *bucket = &srv->listeners[listener_bucket(type)];
    isz_list_node *pos = bucket->head.next;
    while (pos != &bucket->head) {
        isz_list_node *next = pos->next;
        struct isz_listener *l =
            container_of(pos, struct isz_listener, node);
        if (l->fn == fn) {
            isz_list_remove(pos);
            free(l);
        }
        pos = next;
    }
    return ISZ_OK;
}

/* Dispatch ev to every listener registered for ev->type, in order.
 * Called from the main dispatch thread by every event-producing
 * subsystem. */
void isz_server_emit_event(isz_server *srv, const isz_event *ev)
{
    if (!srv || !ev)
        return;
    if (srv->state == ISZ_SERVER_DESTROYING)
        return;

    isz_list *bucket = &srv->listeners[listener_bucket(ev->type)];
    /* Walk by node pointer so a listener can safely remove itself or
     * add new listeners mid-dispatch (the new ones don't fire this
     * round). */
    isz_list_node *pos = bucket->head.next;
    while (pos != &bucket->head) {
        isz_list_node *next = pos->next;
        struct isz_listener *l =
            container_of(pos, struct isz_listener, node);
        l->fn(l->userdata, ev);
        pos = next;
    }
}

/* Alias. Same function; kept so call sites that read better without
 * the server_ prefix can use the shorter name. */
void isz_emit_event(isz_server *srv, const isz_event *ev)
{
    isz_server_emit_event(srv, ev);
}
