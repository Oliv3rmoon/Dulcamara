/* Verified local native-save snapshots. GPL-3.0-or-later */
#ifndef NP_SAVES_H
#define NP_SAVES_H
#include "maintenance.h"
#define SAVE_ID_MAX 64
#define SAVE_LIST_MAX 128
/* PRE_INITIAL protects discovered native python-early paths only. */
typedef enum
{
    SAVE_AUTOMATIC = 1,
    SAVE_MANUAL = 2,
    SAVE_RECOVERY = 3,
    SAVE_PRE_INITIAL = 4
} SaveKind;
typedef struct
{
    char game_id[24];
    char baseline[SAVE_ID_MAX]; /* previous successful early result in this launch, including
                                   unchanged */
    uint32_t core;
    SaveKind kind;
    uint64_t session;
    struct dr_save_layout layout;
} SaveRequest;
typedef struct
{
    char snapshot[SAVE_ID_MAX], error[256];
    bool unchanged, shared;
    uint64_t files, bytes;
    uint64_t created; /* CLOCK_REALTIME nanoseconds since the Unix epoch, not seconds. */
} SaveResult;
typedef enum
{
    SAVE_MOMENT_EARLY = 1,
    SAVE_MOMENT_PREPLAY,
    SAVE_MOMENT_POSTSTOP,
    SAVE_MOMENT_MANUAL,
    SAVE_MOMENT_RECOVERY
} SaveMoment;
typedef struct
{
    char snapshot[SAVE_ID_MAX];
    SaveKind kind;
    SaveMoment moment;
    uint64_t created; /* CLOCK_REALTIME nanoseconds since the Unix epoch, not seconds. */
    uint64_t files, bytes;
    bool shared;
} SaveInfo;
/* Blocking worker/test APIs; mutations acquire the shared maintenance gate.
 * FILE scopes remain exact, GAME means association, and shared effects are recomputed. */
bool save_snapshot(const char *root, const SaveRequest *request, const MtObserver *observer,
                   SaveResult *result);
bool save_restore(const char *root, const char *game, const char *snapshot, bool confirmed,
                  const MtObserver *observer, SaveResult *result);
bool save_list(const char *root, const char *game, SaveInfo *out, size_t capacity, size_t *count,
               char *error, size_t error_size);
bool save_details(const char *root, const char *game, const char *snapshot, SaveInfo *info,
                  struct dr_save_layout *layout, char *error, size_t error_size);
bool save_delete(const char *root, const char *game, const char *snapshot, char *error,
                 size_t error_size);
bool save_recover(const char *root, const MtObserver *observer, char *error, size_t error_size);
typedef enum
{
    SAVES_IDLE,
    SAVES_WORKING,
    SAVES_DONE,
    SAVES_ERROR,
    SAVES_CANCELLED
} SavesState;
typedef enum
{
    SAVES_RECOVER,
    SAVES_CREATE,
    SAVES_RESTORE,
    SAVES_DELETE,
    SAVES_LIST,
    SAVES_DETAILS,
    SAVES_REGISTER
} SavesOperation;
typedef struct
{
    SavesState state;
    SavesOperation operation;
    MtPhase phase;
    uint64_t done, total, serial;
    SaveResult result;
    SaveInfo list[SAVE_LIST_MAX];
    size_t count, offset; /* count is total available; list holds min(128,count-offset) */
    SaveInfo detail;
    struct dr_save_layout layout;
} SavesStatus;
/* Nonblocking main/UI entry points. Arguments/status are copied; poll joins only a
 * completed worker. Errors never release checkpoints. Restore requires confirmation
 * after displaying save_details' roots, including shared effects. */
/* Register once before init. Callback runs after save recovery, under MAINT_RECOVERY;
 * never reacquire the gate. Set restart even on failure if executable generation changed. */
typedef bool (*SavesUpdateRecovery)(void *, bool *restart, char *error, size_t size);
void saves_set_update_recovery(SavesUpdateRecovery callback, void *context);
bool saves_init(const char *root);
bool saves_start(SavesOperation operation, const SaveRequest *request, const char *snapshot,
                 bool confirmed);
bool saves_start_page(const char *game, size_t offset);
void saves_poll(SavesStatus *out);
void saves_cancel(void);
bool saves_busy(void);
#endif
