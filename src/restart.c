/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "restart.h"
#include "storage.h"
#include <string.h>
static bool valid(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    return n > 0 && n < 24 && strspn(id, "0123456789abcdefABCDEF") == n;
}
bool restart_write(const char *path, const char *id)
{
    if (!valid(id))
        return false;
    StorageFile write;
    if (!storage_begin(&write, path))
        return false;
    fprintf(write.file, "%s\n", id);
    return storage_commit(&write);
}
bool restart_read(const char *path, char *id, size_t size)
{
    if (!id || !size)
        return false;
    id[0] = '\0';
    FILE *file = fopen(path, "r");
    if (!file)
        return false;
    char line[26];
    bool ok = fgets(line, sizeof(line), file) != NULL;
    if (ok)
    {
        size_t n = strlen(line);
        ok = n && line[n - 1] == '\n' && fgetc(file) == EOF && !(ferror)(file);
        if (ok)
            line[n - 1] = '\0';
        ok = ok && valid(line) && strlen(line) < size;
    }
    if (fclose(file))
        ok = false;
    if (ok)
        strcpy(id, line);
    return ok;
}
bool restart_acknowledge(const char *path, const char *id)
{
    char recorded[24];
    return restart_read(path, recorded, sizeof(recorded)) && !strcmp(recorded, id) &&
           storage_remove(path);
}
