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

/* isz_allowlist.c - client trust (SPEC §6.3).
 *
 * isz_allowlist_add_binary stats the path at call time and stores
 * (st_dev, st_ino). isz_allowlist_add_cgroup stores the path string.
 * isz_allowlist_check reads /proc/<pid>/exe (via stat, which follows
 * the symlink to the real binary), compares to stored binary entries,
 * and on no match reads /proc/<pid>/cgroup and compares to stored
 * cgroup paths (prefix match, so an Architect can allowlist a slice
 * and cover everything under it).
 *
 * SPEC §6.3: "An empty allowlist means deny-all." Both lists start
 * empty; isz_allowlist_check returns false until the Architect
 * populates one. Entries are re-resolved on each check (correctness
 * first; the cache optimization is a later pass). */
#define _POSIX_C_SOURCE 200809L

#include "isz_server_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/isz_log.h"

ISZ_API int isz_allowlist_add_binary(isz_server *srv, const char *path)
{
    if (!srv || !path)
        return ISZ_ERR_INVALID_ARG;

    struct stat st;
    if (stat(path, &st) < 0) {
        isz_log_internal(ISZ_LOG_WARN,
                         "allowlist_add_binary: stat('%s') failed: %s",
                         path, strerror(errno));
        return ISZ_ERR_INVALID_ARG;
    }

    struct isz_allowlist_binary *b = malloc(sizeof(*b));
    if (!b) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "allowlist_add_binary: out of memory");
        return ISZ_ERR_NO_MEMORY;
    }
    b->st_dev = st.st_dev;
    b->st_ino = st.st_ino;
    isz_list_push_back(&srv->allowlist_binaries, &b->node);
    return ISZ_OK;
}

ISZ_API int isz_allowlist_add_cgroup(isz_server *srv, const char *cgroup_path)
{
    if (!srv || !cgroup_path)
        return ISZ_ERR_INVALID_ARG;

    char *dup = strdup(cgroup_path);
    if (!dup) {
        isz_log_internal(ISZ_LOG_ERROR,
                         "allowlist_add_cgroup: out of memory");
        return ISZ_ERR_NO_MEMORY;
    }

    struct isz_allowlist_cgroup *c = malloc(sizeof(*c));
    if (!c) {
        free(dup);
        isz_log_internal(ISZ_LOG_ERROR,
                         "allowlist_add_cgroup: out of memory");
        return ISZ_ERR_NO_MEMORY;
    }
    c->path = dup;
    isz_list_push_back(&srv->allowlist_cgroups, &c->node);
    return ISZ_OK;
}

/* ------------------------------------------------------------------ */
/* Internal: peer check                                               */
/* ------------------------------------------------------------------ */
/* SPEC §6.3: "An empty allowlist means deny-all." Check returns false
 * if both lists are empty. Otherwise:
 *   1. stat /proc/<pid>/exe (follows the symlink to the binary), and
 *      compare (st_dev, st_ino) to every stored binary entry.
 *   2. If no binary match, read /proc/<pid>/cgroup and compare each
 *      line's path component to stored cgroup paths (prefix match).
 * Returns true on any match. */
static bool peer_pid_matches_binary(isz_server *srv, pid_t pid)
{
    if (isz_list_empty(&srv->allowlist_binaries))
        return false;

    char exe_path[64];
    snprintf(exe_path, sizeof(exe_path), "/proc/%d/exe", (int)pid);

    struct stat st;
    if (stat(exe_path, &st) < 0) {
        isz_log_internal(ISZ_LOG_DEBUG,
                         "allowlist: stat('%s') failed: %s",
                         exe_path, strerror(errno));
        return false;
    }

    isz_list_node *pos;
    isz_list_for_each(pos, &srv->allowlist_binaries) {
        struct isz_allowlist_binary *b =
            container_of(pos, struct isz_allowlist_binary, node);
        if (b->st_dev == st.st_dev && b->st_ino == st.st_ino)
            return true;
    }
    return false;
}

/* Read /proc/<pid>/cgroup into buf (NUL-terminated). Returns true on
 * success, false on read failure (treat as no match). */
static bool read_peer_cgroup(pid_t pid, char *buf, size_t buf_len)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)pid);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    size_t off = 0;
    while (off < buf_len - 1) {
        ssize_t n = read(fd, buf + off, buf_len - 1 - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return false;
        }
        if (n == 0)
            break;
        off += (size_t)n;
    }
    buf[off] = '\0';
    close(fd);
    return true;
}

/* A cgroup v2 line looks like "0::/user.slice/user-1000.slice/...\n".
 * A v1 line looks like "10:memory:/user.slice/...\n". We want the path
 * part after the second colon. Returns a pointer into the buffer (no
 * allocation), or NULL if the line is malformed. The path is NUL-
 * terminated in place by replacing the newline. */
static const char *cgroup_line_path(char *line)
{
    /* Skip past the first two colons. */
    char *p = strchr(line, ':');
    if (!p)
        return NULL;
    p = strchr(p + 1, ':');
    if (!p)
        return NULL;
    p++;
    /* Strip the trailing newline. */
    char *nl = strchr(p, '\n');
    if (nl)
        *nl = '\0';
    return p;
}

static bool peer_pid_matches_cgroup(isz_server *srv, pid_t pid)
{
    if (isz_list_empty(&srv->allowlist_cgroups))
        return false;

    char buf[4096];
    if (!read_peer_cgroup(pid, buf, sizeof(buf)))
        return false;

    /* For each stored cgroup path, check whether any line's path
     * component equals or is a child of it (prefix match, with a
     * boundary check so "/a" doesn't match "/ab"). */
    isz_list_node *pos;
    isz_list_for_each(pos, &srv->allowlist_cgroups) {
        struct isz_allowlist_cgroup *c =
            container_of(pos, struct isz_allowlist_cgroup, node);
        size_t plen = strlen(c->path);

        char *line = buf;
        char *next;
        while (line && *line) {
            next = strchr(line, '\n');
            if (next)
                *next = '\0';

            const char *p = cgroup_line_path(line);
            if (p) {
                size_t llen = strlen(p);
                if (llen >= plen && strncmp(p, c->path, plen) == 0) {
                    /* Boundary: stored path is either equal to p, or
                     * p continues with '/' (so it's a child). */
                    if (llen == plen || p[plen] == '/')
                        return true;
                }
            }

            line = next ? next + 1 : NULL;
        }
    }
    return false;
}

bool isz_allowlist_check(isz_server *srv, pid_t peer_pid)
{
    if (!srv || peer_pid <= 0)
        return false;

    /* Empty allowlist = deny-all (SPEC §6.3). */
    if (isz_list_empty(&srv->allowlist_binaries) &&
        isz_list_empty(&srv->allowlist_cgroups))
        return false;

    if (peer_pid_matches_binary(srv, peer_pid))
        return true;
    if (peer_pid_matches_cgroup(srv, peer_pid))
        return true;
    return false;
}
