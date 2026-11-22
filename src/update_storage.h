/* Owned installation staging/recovery. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NP_UPDATE_STORAGE_H
#define NP_UPDATE_STORAGE_H
#include "maintenance.h"
/* Apply acquires MAINT_UPDATE itself; no installed byte changes before complete CRC staging.
 * Caller has already authenticated the downloaded ZIP's SHA256 and expected size. */
bool update_storage_apply(const char *zip, const char *installed, const char *expected_version,
                          const MtObserver *observer, char *error, size_t size);
/* Called under MAINT_RECOVERY. restart stays true even on failure after finding evidence.
 * Successful recovery removes its workspace durably, so next process does not restart again. */
bool update_storage_recover(const char *installed, bool *restart, char *error, size_t size);
#endif
