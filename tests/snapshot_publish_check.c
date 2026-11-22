/* Owned disposable durability-boundary probe, not a power-loss simulation. */
#include "saves.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct stat backup_identity;
static char backup_directory[MT_PATH_MAX];
static bool armed, persistent;
static unsigned failures, synchronized_after_failure, minimum_published = 1;

static bool published_exists(void)
{
    DIR *d = opendir(backup_directory);
    assert(d);
    unsigned found = 0;
    struct dirent *entry;
    while ((entry = readdir(d)))
        if (entry->d_name[0] != '.')
            ++found;
    assert(!closedir(d));
    return found >= minimum_published;
}

/* Only maintenance.c is compiled with this symbol replacing fsync. All calls
 * except the one selected by actual directory identity use the real syscall. */
int probe_fsync(int fd)
{
    struct stat st;
    bool selected = armed && !fstat(fd, &st) &&
                    st.st_dev == backup_identity.st_dev &&
                    st.st_ino == backup_identity.st_ino && published_exists();
    if (selected && (!failures || persistent))
    {
        ++failures;
        errno = EIO;
        return -1;
    }
    int result = fsync(fd);
    if (selected && failures && !result)
        ++synchronized_after_failure;
    return result;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    persistent = !strcmp(argv[2], "persistent");
    char root[MT_PATH_MAX], native[MT_PATH_MAX], slot[MT_PATH_MAX];
    assert(mt_path(root, argv[1], "app-data") && mt_mkdirs(root));
    assert(mt_path(native, argv[1], "native") && mt_mkdirs(native));
    assert(mt_path(slot, native, "Save01.lsd"));
    FILE *f = fopen(slot, "wb");
    assert(f && fwrite("original-native-save", 1, 20, f) == 20 && !fclose(f));
    assert(mt_path(backup_directory, root, "backups/fixture"));
    assert(mt_mkdirs(backup_directory) && !stat(backup_directory, &backup_identity));
    SaveRequest request = {.kind = SAVE_AUTOMATIC, .core = 0};
    strcpy(request.game_id, "fixture");
    request.layout = (struct dr_save_layout){
        .version = DR_RUNTIME_VERSION, .size = sizeof(struct dr_save_layout),
        .available = true, .complete = true, .writers_finished = true, .count = 1};
    request.layout.entries[0] = (struct dr_save_entry){
        .kind = DR_SAVE_RPG_SLOTS, .scope = DR_SAVE_GAME, .policy = DR_SAVE_READ_WRITE};
    strcpy(request.layout.entries[0].path, native);
    maintenance_recovered(true);
    bool retention = !strcmp(argv[2], "retention");
    if (retention)
    {
        SaveResult seeded;
        for (int i = 0; i < 5; ++i)
        {
            f = fopen(slot, "wb");
            assert(f && fprintf(f, "seed-%d", i) > 0 && !fclose(f));
            assert(save_snapshot(root, &request, NULL, &seeded));
        }
        request.kind = SAVE_MANUAL;
        assert(save_snapshot(root, &request, NULL, &seeded));
        request.kind = SAVE_RECOVERY;
        assert(save_snapshot(root, &request, NULL, &seeded));
        request.kind = SAVE_AUTOMATIC;
        f = fopen(slot, "wb");
        assert(f && fwrite("original-native-save", 1, 20, f) == 20 && !fclose(f));
        minimum_published = 8;
    }
    armed = true;
    SaveResult first, retry;
    bool first_ok = save_snapshot(root, &request, NULL, &first);
    bool published = published_exists();
    bool retry_ok = save_snapshot(root, &request, NULL, &retry);
    printf("{\"first_ok\":%d,\"injected_flush_failures\":%u,\"published_after_failure\":%d,"
           "\"retry_ok\":%d,\"retry_unchanged\":%d,\"successful_parent_sync_after_failure\":%u}\n",
           first_ok, failures, published, retry_ok, retry.unchanged, synchronized_after_failure);
    assert(!first_ok && failures >= 1 && published);
    if (persistent) {
        assert(!retry_ok && !synchronized_after_failure);
        persistent = false;
        retry_ok = save_snapshot(root, &request, NULL, &retry);
    }
    assert(retry_ok && retry.unchanged && synchronized_after_failure);
    size_t count = 0;
    assert(save_list(root, "fixture", NULL, 0, &count, NULL, 0) && count == (retention ? 7 : 1));
    if (retention)
    {
        SaveInfo entries[8];
        assert(save_list(root, "fixture", entries, 8, &count, NULL, 0));
        unsigned autos = 0, manuals = 0, recoveries = 0;
        for (size_t i = 0; i < count; ++i)
        {
            autos += entries[i].kind == SAVE_AUTOMATIC;
            manuals += entries[i].kind == SAVE_MANUAL;
            recoveries += entries[i].kind == SAVE_RECOVERY;
        }
        assert(autos == 5 && manuals == 1 && recoveries == 1);
    }
    char bytes[21] = {0};
    f = fopen(slot, "rb");
    assert(f && fread(bytes, 1, 20, f) == 20 && !fclose(f));
    assert(!strcmp(bytes, "original-native-save"));
    /* A successful retry must complete the previously failed publication sync. */
    return retry_ok && !synchronized_after_failure ? 1 : 0;
}
