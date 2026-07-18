/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Rui-727
 * See LICENSE for full text. */

/* isz_list.h - intrusive doubly-linked list with a sentinel head.
 * Implements the read-only-during-dispatch list pattern from SPEC §5. */

#ifndef ISZ_LIST_H
#define ISZ_LIST_H

#include <stdbool.h>

#include "isz_compiler.h"

typedef struct isz_list_node {
    struct isz_list_node *prev;
    struct isz_list_node *next;
} isz_list_node;

typedef struct {
    isz_list_node head; /* sentinel: prev = tail, next = first */
} isz_list;

static inline void isz_list_init(isz_list *l)
{
    l->head.prev = &l->head;
    l->head.next = &l->head;
}

static inline bool isz_list_empty(const isz_list *l)
{
    return l->head.next == &l->head;
}

static inline void isz_list_insert(isz_list_node *n,
                                   isz_list_node *prev,
                                   isz_list_node *next)
{
    prev->next = n;
    next->prev = n;
    n->prev = prev;
    n->next = next;
}

static inline void isz_list_push_back(isz_list *l, isz_list_node *n)
{
    isz_list_insert(n, l->head.prev, &l->head);
}

static inline void isz_list_push_front(isz_list *l, isz_list_node *n)
{
    isz_list_insert(n, &l->head, l->head.next);
}

/* Detach n from its list. Caller must know n is currently linked. */
static inline void isz_list_remove(isz_list_node *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = n;
    n->next = n;
}

/* Unlink and return the first node, or NULL if empty. */
static inline isz_list_node *isz_list_pop_front(isz_list *l)
{
    isz_list_node *n = l->head.next;
    if (n == &l->head)
        return NULL;
    isz_list_remove(n);
    return n;
}

/* Iterate over node pointers. pos: isz_list_node *, l: isz_list *. */
#define isz_list_for_each(pos, l) \
    for ((pos) = (l)->head.next; (pos) != &(l)->head; (pos) = (pos)->next)

/* Iterate and recover the containing struct via container_of.
 * pos: type *, l: isz_list *, member: field name in type. */
#define isz_list_for_each_entry(pos, l, type, member) \
    for ((pos) = container_of((l)->head.next, type, member); \
         &(pos)->member != &(l)->head; \
         (pos) = container_of((pos)->member.next, type, member))

#endif /* ISZ_LIST_H */
