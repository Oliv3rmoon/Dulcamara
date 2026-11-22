/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
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

int storage_portable_root(char *root, size_t size, const char *const *mounts, size_t count)
{
    char selected[NP_PATH_MAX] = "";
    for (size_t i = 0; i < count; ++i)
    {
        char dir[NP_PATH_MAX], marker[NP_PATH_MAX];
        struct stat st;
        int n = snprintf(dir, sizeof(dir), "%s/dulcamara", mounts[i]);
        if (n < 0 || (size_t)n >= sizeof(dir) || lstat(dir, &st) || !S_ISDIR(st.st_mode))
            continue;
        n = snprintf(marker, sizeof(marker), "%s/portable.txt", dir);
        if (n < 0 || (size_t)n >= sizeof(marker) || lstat(marker, &st) || !S_ISREG(st.st_mode))
            continue;
        if (selected[0] || strlen(dir) >= size)
            return -1;
        str_copy(selected, sizeof(selected), dir);
    }
    if (!selected[0])
        return 0;
    str_copy(root, size, selected);
    return 1;
}

static bool open_library_directory(const char *path, char *error, size_t size)
{
    DIR *dir = opendir(path);
    if (!dir && errno == ENOENT && (mkdir(path, 0755) == 0 || errno == EEXIST))
        dir = opendir(path);
    if (!dir)
    {
        snprintf(error, size, "Cannot open %s (%s).", path, strerror(errno));
        return false;
    }
    closedir(dir);
    return true;
}

bool storage_check_library(const char *root, char *error, size_t size)
{
    char games[NP_PATH_MAX], probe[NP_PATH_MAX];
    path_join(games, sizeof(games), root, "games");
    /* Opening the root for enumeration can be denied while its known children are usable. */
    struct stat st;
    if (stat(root, &st) != 0)
    {
        if (errno != ENOENT || (mkdir(root, 0755) != 0 && errno != EEXIST))
        {
            snprintf(error, size, "Cannot access %s (%s).", root, strerror(errno));
            return false;
        }
    }
    else if (!S_ISDIR(st.st_mode))
    {
        snprintf(error, size, "%s is not a directory.", root);
        return false;
    }
    if (!open_library_directory(games, error, size))
        return false;
    path_join(probe, sizeof(probe), root, ".write-test");
    StorageFile file;
    if (!storage_begin(&file, probe))
    {
        snprintf(error, size, "Cannot write to %s (%s).", root, strerror(errno));
        return false;
    }
    storage_abort(&file);
    return true;
}
