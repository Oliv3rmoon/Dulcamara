/* Optional private libretro ABI v4. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DR_RUNTIME_PROTOCOL_H
#define DR_RUNTIME_PROTOCOL_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define DR_RUNTIME_VERSION 4u
#define DR_SAVE_MAX 32u
#define DR_SAVE_PATH_MAX 1024u
#define DR_SAVE_SUFFIX_MAX 128u
#define DR_ENVIRONMENT_RUNTIME (2u | 0x20000u)
enum dr_save_kind
{
    DR_SAVE_DIRECTORY = 1,
    DR_SAVE_FILE = 2,
    DR_SAVE_RPG_SLOTS = 3,
    DR_SAVE_RENPY_SLOTS = 4,
    DR_SAVE_RENPY_FILE = 5
};
enum dr_save_scope
{
    DR_SAVE_GAME = 1,
    DR_SAVE_SHARED = 2
};
enum dr_save_policy
{
    DR_SAVE_UNKNOWN = 0,
    DR_SAVE_READ_ONLY = 1,
    DR_SAVE_READ_WRITE = 2
};
/* DIRECTORY covers native save files beneath a dedicated directory. FILE is
 * exactly one shared file (MultiPersistent), never its enclosing HOME tree.
 * RPG_SLOTS selects case-insensitive Save*.lsd only, not arbitrary game assets.
 * policy reports native filesystem access capability, not guaranteed write success. */
struct dr_save_entry
{
    char path[DR_SAVE_PATH_MAX];
    uint32_t kind, scope, policy;
    /* Authoritative native renpy.savegame_suffix, empty for other kinds. */
    char suffix[DR_SAVE_SUFFIX_MAX];
};
/* Shared producer/consumer validation; UNKNOWN is a recognized non-writable observation. */
static inline bool dr_save_entry_valid(const struct dr_save_entry *entry)
{
    return entry->path[0] == '/' && memchr(entry->path, 0, sizeof(entry->path)) &&
           (entry->kind == DR_SAVE_DIRECTORY || entry->kind == DR_SAVE_FILE ||
            entry->kind == DR_SAVE_RPG_SLOTS || entry->kind == DR_SAVE_RENPY_SLOTS ||
            entry->kind == DR_SAVE_RENPY_FILE) &&
           (entry->scope == DR_SAVE_GAME || entry->scope == DR_SAVE_SHARED) &&
           (entry->policy == DR_SAVE_UNKNOWN || entry->policy == DR_SAVE_READ_ONLY ||
            entry->policy == DR_SAVE_READ_WRITE) &&
           memchr(entry->suffix, 0, sizeof(entry->suffix)) &&
           (entry->kind == DR_SAVE_RENPY_SLOTS
                ? (entry->suffix[0] && !strchr(entry->suffix, '/') &&
                   !strchr(entry->suffix, '\\') && strcmp(entry->suffix, ".") &&
                   strcmp(entry->suffix, ".."))
                : !entry->suffix[0]);
}
/* RENPY_FILE is an exact native filename plus its .new / numeric .tmp siblings;
 * one stable entry covers artifacts across processes without authorizing other files.
 * RENPY_SLOTS is a native suffix selector plus persistent, persistent.new and
 * their native .new / numeric .tmp artifacts. Token keys/upgraded.txt are exact
 * SHARED FILE entries; generic DIRECTORY is never emitted by the RenPy adapters.
 * New suffixes extend the union; they never replace prior declared selectors. */
/* Union of paths discovered by native APIs, never title-derived. A later discovery
 * checkpoint only gates that new path, NOT other writers. Initial/preinitial held
 * checkpoints grant startup quiescence; writers_finished also requires clean stop.
 * complete means the declared native union has no overflow/invalid path, not that
 * arbitrary future game init/Python I/O can be predicted. */
struct dr_save_layout
{
    uint32_t version, size, generation, count;
    bool available, complete, checkpoint, initial, writers_finished;
    /* Native writer barrier before the initial union, protects only discovered paths. */
    bool preinitial;
    struct dr_save_entry entries[DR_SAVE_MAX];
};
enum dr_stop_result
{
    DR_STOP_RUNNING,
    DR_STOP_PENDING,
    DR_STOP_CLEAN,
    DR_STOP_TIMEOUT
};
struct dr_runtime_interface
{
    uint32_t version, size;
    bool restart_required;
    void (*copy_layout)(struct dr_save_layout *out);
    bool (*release_checkpoint)(uint32_t generation);
    void (*request_stop)(void);
    enum dr_stop_result (*poll_stop)(void);
    void (*reset_transient)(void);
};
#endif
