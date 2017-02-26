/*
 * 9p utilities
 *
 * Copyright IBM, Corp. 2017
 *
 * Authors:
 *  Greg Kurz <groug@kaod.org>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_9P_UTIL_H
#define QEMU_9P_UTIL_H

static inline void close_preserve_errno(int fd)
{
    int serrno = errno;
    close(fd);
    errno = serrno;
}

static inline int openat_dir(int dirfd, const char *name)
{
    return openat(dirfd, name, O_DIRECTORY | O_RDONLY | O_PATH);
}

static inline int openat_file(int dirfd, const char *name, int flags,
                              mode_t mode)
{
    int fd, serrno;

    fd = openat(dirfd, name, flags | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK,
                mode);
    if (fd == -1) {
        return -1;
    }

    serrno = errno;
    /* O_NONBLOCK was only needed to open the file. Let's drop it. */
    assert(!fcntl(fd, F_SETFL, flags));
    errno = serrno;
    return fd;
}

int openat_nofollow(int dirfd, const char *path, int flags, mode_t mode);

#endif
