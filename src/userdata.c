/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "userdata.h"
#include "storage.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

const GameUserData *user_metadata_find(const UserMetadata *metadata, const char *id)
{
    for (size_t i = 0; i < metadata->count; ++i)
        if (!strcmp(metadata->records[i].id, id))
            return &metadata->records[i];
    return NULL;
}

static GameUserData *record_for(UserMetadata *metadata, const char *id)
{
    if (!game_id_valid(id))
        return NULL;
    for (size_t i = 0; i < metadata->count; ++i)
        if (!strcmp(metadata->records[i].id, id))
            return &metadata->records[i];
    if (metadata->count >= USER_METADATA_MAX_RECORDS)
        return NULL;
    if (metadata->count == metadata->capacity)
    {
        size_t capacity = metadata->capacity ? metadata->capacity * 2 : 32;
        if (capacity > USER_METADATA_MAX_RECORDS)
            capacity = USER_METADATA_MAX_RECORDS;
        GameUserData *records = realloc(metadata->records, capacity * sizeof(*records));
        if (!records)
            return NULL;
        metadata->records = records;
        metadata->capacity = capacity;
    }
    GameUserData *record = &metadata->records[metadata->count++];
    memset(record, 0, sizeof(*record));
    str_copy(record->id, sizeof(record->id), id);
    return record;
}

bool user_metadata_set_favorite(UserMetadata *metadata, const char *id, bool favorite)
{
    GameUserData *record = record_for(metadata, id);
    if (!record)
        return false;
    record->favorite = favorite;
    return true;
}

bool user_metadata_add_playtime(UserMetadata *metadata, const char *id, uint64_t seconds)
{
    GameUserData *record = record_for(metadata, id);
    if (!record)
        return false;
    record->playtime_seconds = seconds > UINT64_MAX - record->playtime_seconds
                                   ? UINT64_MAX
                                   : record->playtime_seconds + seconds;
    return true;
}

void user_metadata_free(UserMetadata *metadata)
{
    free(metadata->records);
    memset(metadata, 0, sizeof(*metadata));
}

bool user_metadata_load(UserMetadata *metadata, const char *path)
{
    user_metadata_free(metadata);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    UserMetadata loaded = {0};
    char line[128];
    bool ok = fgets(line, sizeof(line), f) && (!strcmp(line, "metadata_version = 1\n") ||
                                               !strcmp(line, "metadata_version = 1\r\n"));
    bool complete = false;
    while (ok && fgets(line, sizeof(line), f))
    {
        if (!strcmp(line, "metadata_end\n") || !strcmp(line, "metadata_end\r\n"))
        {
            complete = true;
            if (fgetc(f) != EOF)
                ok = false;
            break;
        }
        char *first = strchr(line, '\t');
        char *second = first ? strchr(first + 1, '\t') : NULL;
        if (!strchr(line, '\n') || !first || !second || second - first != 2 ||
            (first[1] != '0' && first[1] != '1'))
        {
            ok = false;
            break;
        }
        *first = '\0';
        if (!game_id_valid(line) || second[1] < '0' || second[1] > '9' ||
            user_metadata_find(&loaded, line))
        {
            ok = false;
            break;
        }
        char *end;
        errno = 0;
        uint64_t seconds = strtoull(second + 1, &end, 10);
        if (errno || (strcmp(end, "\n") && strcmp(end, "\r\n")))
        {
            ok = false;
            break;
        }
        GameUserData *record = record_for(&loaded, line);
        if (!record)
        {
            ok = false;
            break;
        }
        record->favorite = first[1] == '1';
        record->playtime_seconds = seconds;
    }
    if (!complete || (ferror)(f))
        ok = false;
    if (fclose(f) != 0)
        ok = false;
    if (ok)
        *metadata = loaded;
    else
        user_metadata_free(&loaded);
    return ok;
}

bool user_metadata_save(const UserMetadata *metadata, const char *path)
{
    if (metadata->count > USER_METADATA_MAX_RECORDS)
        return false;
    StorageFile write;
    if (!storage_begin(&write, path))
        return false;
    fputs("metadata_version = 1\n", write.file);
    for (size_t i = 0; i < metadata->count; ++i)
    {
        const GameUserData *record = &metadata->records[i];
        if (!game_id_valid(record->id))
        {
            storage_abort(&write);
            return false;
        }
        fprintf(write.file, "%s\t%d\t%" PRIu64 "\n", record->id, record->favorite,
                record->playtime_seconds);
    }
    fputs("metadata_end\n", write.file);
    return storage_commit(&write);
}
