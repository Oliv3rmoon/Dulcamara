/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "storage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

bool storage_begin(StorageFile *write, const char *path)
{
    static atomic_uint serial;
    memset(write, 0, sizeof(*write));
    if (!path || !*path || strlen(path) >= sizeof(write->target))
        return false;
    str_copy(write->target, sizeof(write->target), path);
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        int n = snprintf(write->temporary, sizeof(write->temporary), "%s.new.%ld.%u", path,
                         (long)getpid(), atomic_fetch_add(&serial, 1));
        if (n < 0 || (size_t)n >= sizeof(write->temporary))
        {
            write->temporary[0] = '\0';
            return false;
        }
        int fd = open(write->temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
        {
            if (errno == EEXIST)
                continue;
            write->temporary[0] = '\0';
            return false;
        }
        write->file = fdopen(fd, "w");
        if (write->file)
            return true;
        close(fd);
        unlink(write->temporary);
        write->temporary[0] = '\0';
        return false;
    }
    write->temporary[0] = '\0'; /* all collisions belong to somebody else */
    return false;
}

void storage_abort(StorageFile *write)
{
    if (write->file)
        fclose(write->file);
    if (write->temporary[0])
        unlink(write->temporary);
    memset(write, 0, sizeof(*write));
}

bool storage_commit(StorageFile *write)
{
    if (!write->file)
        return false;
    bool ok = !(ferror)(write->file);
    if (fflush(write->file) != 0)
        ok = false;
    if (ok && fsync((fileno)(write->file)) != 0 && errno != ENOSYS && errno != EINVAL &&
        errno != ENOTSUP)
        ok = false;
    if (fclose(write->file) != 0)
        ok = false;
    write->file = NULL;
    if (ok)
    {
        if (rename(write->temporary, write->target) != 0)
            ok = false;
        else
            write->temporary[0] = '\0';
    }
    storage_abort(write);
    return ok;
}

bool storage_remove(const char *path)
{
    return unlink(path) == 0 || errno == ENOENT;
}
