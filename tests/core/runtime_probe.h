/* Real-core optional protocol assertions. SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../src/core/runtime_protocol.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <time.h>
#include <sys/stat.h>
#ifdef DR_TEST_SAVES
#include "../../src/saves.h"
static bool probe_saves_started;
static char probe_saved_id[SAVE_ID_MAX];
static char probe_early_id[SAVE_ID_MAX];
#endif
static struct dr_runtime_interface probe_runtime;
static unsigned probe_initial;
static double probe_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void probe_sleep(void)
{
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}
static bool probe_environment(void *data)
{
    const char *enabled = getenv("DULCAMARA_RUNTIME_PROBE");
    if (!enabled || !*enabled)
        return false;
    probe_runtime = *(struct dr_runtime_interface *)data;
    assert(probe_runtime.version == DR_RUNTIME_VERSION);
    return true;
}
static void probe_tick(void)
{
    if (!probe_runtime.version)
        return;
    struct dr_save_layout layout;
    probe_runtime.copy_layout(&layout);
    if (!layout.checkpoint)
        return;
    uint32_t epoch = layout.generation;
#ifdef DR_TEST_SAVES
    const char *store = getenv("DULCAMARA_SAVE_STORE");
    if (store && (layout.initial || layout.preinitial))
    {
        if (!probe_saves_started)
        {
            assert(saves_init(store));
            SavesStatus status;
            do
            {
                saves_poll(&status);
                probe_sleep();
            } while (status.state == SAVES_WORKING);
            assert(status.state == SAVES_DONE);
            probe_saves_started = true;
        }
        maintenance_runtime(true, true, false);
        const struct dr_save_entry *last = layout.entries + layout.count - 1;
        unsigned char before[32];
        uint64_t before_size = 0;
        bool exists = last->kind == DR_SAVE_FILE && mt_hash(last->path, before, &before_size);
        SaveRequest request = {
            .core =
                (uint32_t)atoi(getenv("DULCAMARA_SAVE_CORE") ? getenv("DULCAMARA_SAVE_CORE") : "1"),
            .session = (uint64_t)strtoull(
                getenv("DULCAMARA_SAVE_SESSION") ? getenv("DULCAMARA_SAVE_SESSION") : "1", NULL,
                10),
            .kind = layout.preinitial ? SAVE_PRE_INITIAL : SAVE_AUTOMATIC,
            .layout = layout};
        strcpy(request.game_id, "native-fixture");
        strcpy(request.baseline, probe_early_id);
        assert(saves_start(SAVES_CREATE, &request, NULL, false));
        SavesStatus status;
        double until = probe_now() + .05;
        do
        {
            if (last->kind == DR_SAVE_FILE)
            {
                unsigned char now[32];
                uint64_t size = 0;
                bool present = mt_hash(last->path, now, &size);
                assert(present == exists);
                if (present)
                    assert(size == before_size && !memcmp(now, before, 32));
            }
            saves_poll(&status);
            probe_sleep();
        } while (status.state == SAVES_WORKING || probe_now() < until);
        if (status.state != SAVES_DONE)
            fprintf(stderr, "save worker: %s\n", status.result.error);
        assert(status.state == SAVES_DONE);
        if (layout.preinitial)
            strcpy(probe_early_id, status.result.snapshot);
        assert(!probe_runtime.release_checkpoint(epoch + 1));
        if (layout.preinitial)
        {
            assert(!layout.initial);
            puts("runtime: early native bytes protected by worker");
        }
    }
#endif
    if (layout.initial)
    {
        ++probe_initial;
        const char *expected = getenv("DULCAMARA_RUNTIME_INITIAL_LIST");
        if (expected)
        {
            FILE *file = fopen(expected, "r");
            assert(file);
            char path[DR_SAVE_PATH_MAX];
            unsigned kind, scope;
            while (fscanf(file, "%u %u ", &kind, &scope) == 2)
            {
                assert(fgets(path, sizeof(path), file));
                path[strcspn(path, "\n")] = 0;
                bool found = false;
                for (uint32_t i = 0; i < layout.count; ++i)
                    if (!strcmp(layout.entries[i].path, path) && layout.entries[i].kind == kind &&
                        layout.entries[i].scope == scope)
                        found = true;
                if (!found)
                    fprintf(stderr, "missing initial native root: %s\n", path);
                assert(found);
            }
            assert(!ferror(file));
            fclose(file);
            puts("runtime: initial union contains every expected native root");
        }
        const char *absent = getenv("DULCAMARA_RUNTIME_ABSENT");
        for (int i = 0; i < 50; ++i)
        {
            struct stat st;
            if (absent)
                assert(stat(absent, &st) != 0);
            probe_sleep();
        }
        probe_runtime.copy_layout(&layout);
        assert(layout.checkpoint && layout.generation == epoch);
        assert(!probe_runtime.release_checkpoint(epoch + 1));
        puts("runtime: initial checkpoint held native writes");
    }
    for (uint32_t i = 0; i < layout.count; ++i)
        printf("native-save: %u %u %u %s\n", layout.entries[i].kind, layout.entries[i].scope,
               layout.entries[i].policy, layout.entries[i].path);
    for(uint32_t i=0;i<layout.count;++i)if(layout.entries[i].kind==DR_SAVE_RENPY_SLOTS)
        printf("native-selector: %s %s\n",layout.entries[i].suffix,layout.entries[i].path);
    double releasing = probe_now();
    assert(probe_runtime.release_checkpoint(epoch));
    printf("runtime: checkpoint release %.3f ms\n", (probe_now() - releasing) * 1000);
}
static void probe_stop(bool timeout)
{
    assert(probe_runtime.version);
    double started = probe_now();
    probe_runtime.request_stop();
    assert(probe_now() - started < .1);
    printf("runtime: stop request %.3f ms\n", (probe_now() - started) * 1000);
    double max_poll = 0;
    enum dr_stop_result result;
    do
    {
        double polling = probe_now();
        result = probe_runtime.poll_stop();
        double elapsed = probe_now() - polling;
        if (elapsed > max_poll)
            max_poll = elapsed;
        probe_sleep();
    } while (result == DR_STOP_PENDING && probe_now() - started < 12);
    printf("runtime: stop poll max %.3f ms, total %.3f ms\n", max_poll * 1000,
           (probe_now() - started) * 1000);
    struct dr_save_layout layout;
    probe_runtime.copy_layout(&layout);
    if (timeout)
    {
        assert(result == DR_STOP_TIMEOUT && !layout.writers_finished);
        assert(probe_runtime.poll_stop() == DR_STOP_TIMEOUT);
        puts("runtime: timeout retained unsafe writers");
    }
    else
    {
        assert(result == DR_STOP_CLEAN && layout.writers_finished);
        puts("runtime: clean stop finished writers");
#ifdef DR_TEST_SAVES
        const char *store = getenv("DULCAMARA_SAVE_STORE");
        if (store)
        {
            maintenance_runtime(false, false, false);
            SaveRequest request = {
                .core = (uint32_t)atoi(getenv("DULCAMARA_SAVE_CORE") ? getenv("DULCAMARA_SAVE_CORE")
                                                                     : "1"),
                .session = (uint64_t)strtoull(
                    getenv("DULCAMARA_SAVE_SESSION") ? getenv("DULCAMARA_SAVE_SESSION") : "1", NULL,
                    10),
                .kind = SAVE_AUTOMATIC,
                .layout = layout};
            strcpy(request.game_id, "native-fixture");
            SaveResult saved;
            assert(save_snapshot(store, &request, NULL, &saved));
            strcpy(probe_saved_id, saved.snapshot);
            printf("runtime: post snapshot %s\n", saved.snapshot);
        }
#endif
    }
}
