/* Durable, bounded file transactions and the process-wide writer gate. GPL-3.0-or-later */
#ifndef NP_MAINTENANCE_H
#define NP_MAINTENANCE_H
#include "core/runtime_protocol.h"
#include <stddef.h>
#include <stdint.h>
#define MT_PATH_MAX 1024
#define MT_FILES_MAX 65536u
/* A transaction never traverses symlinks. DIRECTORY must be a dedicated owned tree;
 * FILE, RPG_SLOTS and RENPY_SLOTS constrain actual target names. Caller supplies ownership. */
typedef struct
{
    struct dr_save_entry scope;
    char target[MT_PATH_MAX], source[MT_PATH_MAX];
    uint32_t mode;
} MtTarget;
typedef enum
{
    MT_PREPARING = 1,
    MT_APPLYING = 2,
    MT_ROLLBACK = 3,
    MT_COMMITTED = 4
} MtPhase;
typedef struct
{
    bool (*cancelled)(void *);
    void (*progress)(void *, MtPhase, uint64_t, uint64_t);
    void *context;
} MtObserver;
typedef enum
{
    MAINT_NONE,
    MAINT_SNAPSHOT,
    MAINT_RESTORE,
    MAINT_UPDATE,
    MAINT_RECOVERY,
    MAINT_LINKS /* a game being added from a link (links.h): no game starts meanwhile */
} MaintenanceOwner;
/* Initialized locked until recovery succeeds. Calls are thread safe. Runtime reservation
 * is acquired on main BEFORE host_load and held through clean-stop protection. */
enum
{
    MAINT_RECOVER_SAVES = 1,
    MAINT_RECOVER_UPDATE = 2
};
/* Configure once before startup workers; each completion changes only its own bit. */
void maintenance_recovery_begin(unsigned required);
void maintenance_recovery_finish(unsigned part, bool ok);
void maintenance_recovered(bool ok); /* save subsystem compatibility */
bool maintenance_launch(void);
void maintenance_runtime(bool owned, bool quiescent, bool unsafe);
bool maintenance_acquire(MaintenanceOwner owner);
void maintenance_release(MaintenanceOwner owner, bool restart_required);
bool maintenance_ready(void);
bool maintenance_restart_ready(void);

bool mt_path(char out[MT_PATH_MAX], const char *base, const char *relative);
bool mt_relative(const char *path);
bool mt_canonical(const char *path, char out[MT_PATH_MAX]);
bool mt_mkdirs(const char *path);
bool mt_sync_dir(const char *path);
bool mt_sync_parent(const char *path);
bool mt_hash(const char *path, unsigned char digest[32], uint64_t *size);
bool mt_copy(const char *source, const char *target, const MtObserver *observer);
bool mt_space(const char *path, uint64_t bytes);
bool mt_atomic(const char *path, const void *data, size_t size);
bool mt_scope_contains(const struct dr_save_entry *scope, const char *target);
bool mt_cancelled(const MtObserver *observer);
/* journal_dir is exclusive to one serial transaction. Nonempty unfinished journal is
 * never overwritten. sources empty => deletion. Sources copied before mutation;
 * caller order is apply order (e.g. executable last). On error recover before launch. */
bool mt_transaction(const char *journal_dir, const MtTarget *targets, uint32_t count,
                    const MtObserver *observer, char *error, size_t error_size);
/* Ownership is rechecked against caller-authorized scopes, including on restart.
 * Recovery is idempotent; invalid journal/content locks maintenance without deletion. */
bool mt_recover(const char *journal_dir, const struct dr_save_entry *allowed, uint32_t count,
                const MtObserver *observer, char *error, size_t error_size);
#endif
