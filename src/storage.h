/* Checked atomic replacement for local sidecars. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NP_STORAGE_H
#define NP_STORAGE_H

#include "nightshade.h"
#include <stdio.h>

typedef struct
{
    FILE *file;
    char target[NP_PATH_MAX];
    char temporary[NP_PATH_MAX];
} StorageFile;

/* A regular dulcamara/portable.txt selects that mounted drive. Zero leaves root unchanged. */
int storage_portable_root(char *root, size_t size, const char *const *mounts, size_t count);
/* Check or create the library and test read/write access using an exclusive temporary file. */
bool storage_check_library(const char *root, char *error, size_t size);

/* Creates an exclusive sibling temporary file. Parent directory must already exist. */
bool storage_begin(StorageFile *write, const char *path);
/* Checks stream errors, flush, supported fsync, close and rename. Always consumes write.
 * Failure removes only the temporary file: the prior target is never unlinked. */
bool storage_commit(StorageFile *write);
void storage_abort(StorageFile *write);
/* Removes a file only; missing is success, other filesystem errors are failure. */
bool storage_remove(const char *path);
#endif
