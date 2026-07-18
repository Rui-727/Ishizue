/* SPDX-License-Identifier: MIT */
/*
 * isz_conn.c - per-connection state and outbound queue.
 */
#define _DEFAULT_SOURCE 1
#include "isz_conn.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ishizue/isz.h>

/* Allocate a queue node holding an already-encoded frame plus any fds
 * the caller wants attached. fds are dup'd into the node so the caller
 * retains ownership of its originals. Returns NULL on failure (caller
 * must close any fds it was trying to send). */
static struct isz_msg_node *isz_msg_node_alloc(const void *frame,
                                               size_t frame_len,
                                               const int *fds,
                                               size_t n_fds) {
    struct isz_msg_node *node =
        malloc(sizeof(*node) + frame_len);
    if (node == NULL) {
        return NULL;
    }
    node->next     = NULL;
    node->total    = frame_len;
    node->sent     = 0;
    node->n_fds    = 0;
    memcpy(node->buf, frame, frame_len);

    for (size_t i = 0; i < n_fds; i++) {
        int dup_fd = dup(fds[i]);
        if (dup_fd < 0) {
            /* Roll back any dups we already did. */
            for (size_t j = 0; j < node->n_fds; j++) {
                close(node->fds[j]);
            }
            free(node);
            return NULL;
        }
        node->fds[node->n_fds++] = dup_fd;
    }
    return node;
}

static void isz_msg_node_free(struct isz_msg_node *node) {
    if (node == NULL) return;
    for (size_t i = 0; i < node->n_fds; i++) {
        close(node->fds[i]);
    }
    free(node);
}

struct isz_conn *isz_conn_create(int fd) {
    struct isz_conn *conn = malloc(sizeof(*conn));
    if (conn == NULL) {
        close(fd);
        return NULL;
    }
    memset(conn, 0, sizeof(*conn));
    conn->fd            = fd;
    conn->version       = 0;
    conn->allowlisted   = false;
    conn->handshake_done = false;
    conn->dead          = false;
    conn->send_head     = NULL;
    conn->send_tail     = NULL;
    conn->queued_events = 0;
    conn->objects       = NULL;
    conn->objects_count = 0;
    conn->objects_cap   = 0;
    conn->next_object_id = 1;  /* 0 is the sentinel */

    isz_proto_recv_state_init(&conn->recv_state);

    /* SPEC §6.13: SO_SNDBUF default 262144. The kernel doubles this
     * internally; the setsockopt value is the hint. Failure to set is
     * non-fatal; we fall back to the kernel default and rely on the
     * internal queue for backpressure. */
    int sndbuf = ISZ_DEFAULT_SNDBUF;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    return conn;
}

void isz_conn_close(struct isz_conn *conn) {
    if (conn == NULL) return;

    /* SPEC §6.12: cleanup. The caller is responsible for releasing
     * surfaces, buffers, and plane slots held by the client before
     * calling this. Here we just close the fd, free the outbound queue
     * (closing any fds carried by queued messages), discard any partial
     * read state (closing its fds), and free the conn. */
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }

    struct isz_msg_node *node = conn->send_head;
    while (node != NULL) {
        struct isz_msg_node *next = node->next;
        isz_msg_node_free(node);
        node = next;
    }
    conn->send_head = NULL;
    conn->send_tail = NULL;
    conn->queued_events = 0;

    isz_proto_recv_state_discard(&conn->recv_state);
    free(conn->objects);
    free(conn);
}

ssize_t isz_conn_recv(struct isz_conn *conn,
                      void *out_buf, size_t buf_len,
                      uint32_t *out_msg_id,
                      size_t *out_payload_len,
                      int *out_fds, size_t *out_n_fds) {
    return isz_proto_recv_state_read(conn->fd, &conn->recv_state,
                                     out_buf, buf_len,
                                     out_msg_id, out_payload_len,
                                     out_fds, out_n_fds);
}

int isz_conn_send(struct isz_conn *conn,
                  uint32_t msg_id,
                  const void *payload, size_t payload_len,
                  const int *fds, size_t n_fds) {
    if (conn->dead) {
        return ISZ_ERR_CLIENT_DISCONNECTED;
    }

    /* Encode into a stack buffer first; if the queue is non-empty we
     * have to enqueue (preserving order), and the encoded form is what
     * gets stored. */
    uint8_t buf[ISZ_PROTO_MAX_MESSAGE];
    size_t total = isz_proto_encode(buf, sizeof(buf), msg_id,
                                    payload, payload_len);
    if (total == 0) {
        return ISZ_ERR_INVALID_ARG;
    }

    /* If there's anything queued, we must append to preserve order;
     * skipping the queue would let a later message overtake an earlier
     * one that's still blocked. */
    if (conn->send_head != NULL) {
        goto enqueue;
    }

    {
        ssize_t s = isz_proto_send_frame(conn->fd, buf, total, fds, n_fds);
        if (s == (ssize_t)total) {
            return ISZ_OK;
        }
        if (s == 0) {
            goto enqueue;  /* EAGAIN */
        }
        /* Hard error: caller marks dead. */
        conn->dead = true;
        return ISZ_ERR_CLIENT_DISCONNECTED;
    }

enqueue:
    /* SPEC §6.13: cap the queue at ISZ_MAX_EVENTS_PER_CLIENT. Beyond
     * that, the client is too slow. */
    if (conn->queued_events >= (size_t)ISZ_MAX_EVENTS_PER_CLIENT) {
        conn->dead = true;
        return ISZ_ERR_CLIENT_TOO_SLOW;
    }

    struct isz_msg_node *node = isz_msg_node_alloc(buf, total, fds, n_fds);
    if (node == NULL) {
        conn->dead = true;
        return ISZ_ERR_NO_MEMORY;
    }
    if (conn->send_tail != NULL) {
        conn->send_tail->next = node;
    } else {
        conn->send_head = node;
    }
    conn->send_tail = node;
    conn->queued_events++;
    return ISZ_OK;
}

int isz_conn_drain(struct isz_conn *conn) {
    if (conn->dead) {
        return ISZ_ERR_CLIENT_DISCONNECTED;
    }

    while (conn->send_head != NULL) {
        struct isz_msg_node *node = conn->send_head;
        ssize_t s = isz_proto_send_frame(conn->fd, node->buf, node->total,
                                         node->fds, node->n_fds);
        if (s == 0) {
            /* EAGAIN: stop and wait for the next writable event. */
            return ISZ_ERR_COMMIT_PENDING;
        }
        if (s < 0) {
            conn->dead = true;
            return ISZ_ERR_CLIENT_DISCONNECTED;
        }

        /* Sent. Pop the head. */
        conn->send_head = node->next;
        if (conn->send_head == NULL) {
            conn->send_tail = NULL;
        }
        conn->queued_events--;
        isz_msg_node_free(node);
    }
    return ISZ_OK;
}

/* ------------------------------------------------------------------ */
/* Per-connection object ID table (SPEC §6.4)                          */
/* ------------------------------------------------------------------ */
uint32_t isz_conn_register_object(struct isz_conn *conn, void *handle,
                                  int kind) {
    if (conn == NULL)
        return 0;

    if (conn->objects_count == conn->objects_cap) {
        size_t ncap = conn->objects_cap ? conn->objects_cap * 2 : 8;
        struct isz_conn_object_entry *ne =
            realloc(conn->objects, ncap * sizeof(*ne));
        if (ne == NULL)
            return 0;
        conn->objects = ne;
        conn->objects_cap = ncap;
    }

    uint32_t id = conn->next_object_id++;
    /* Wrap at 32 bits, skipping the 0 sentinel. */
    if (conn->next_object_id == 0)
        conn->next_object_id = 1;

    conn->objects[conn->objects_count].id     = id;
    conn->objects[conn->objects_count].handle = handle;
    conn->objects[conn->objects_count].kind   = kind;
    conn->objects_count++;
    return id;
}

void *isz_conn_lookup_object(struct isz_conn *conn, uint32_t id, int kind) {
    if (conn == NULL || id == 0)
        return NULL;
    for (size_t i = 0; i < conn->objects_count; i++) {
        if (conn->objects[i].id == id) {
            if (conn->objects[i].kind != kind)
                return NULL;
            return conn->objects[i].handle;
        }
    }
    return NULL;
}

void isz_conn_unregister_object(struct isz_conn *conn, uint32_t id) {
    if (conn == NULL || id == 0)
        return;
    for (size_t i = 0; i < conn->objects_count; i++) {
        if (conn->objects[i].id == id) {
            /* Compact by moving the tail entry down. */
            conn->objects_count--;
            if (i != conn->objects_count)
                conn->objects[i] = conn->objects[conn->objects_count];
            return;
        }
    }
}
