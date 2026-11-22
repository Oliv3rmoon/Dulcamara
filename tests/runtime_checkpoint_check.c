/* Production handoff/save checkpoint regression. SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/renpy/handoff.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static atomic_int writes;
static void pause_ms(void)
{
    struct timespec t = {0, 1000000};
    nanosleep(&t, 0);
}
static void *writer(void *unused)
{
    (void)unused;
    handoff_engine_begin();
    struct dr_save_entry initial[] = {
        {"/native/game/saves", DR_SAVE_DIRECTORY, DR_SAVE_GAME, DR_SAVE_READ_WRITE, ""},
        {"/native/game/local", DR_SAVE_DIRECTORY, DR_SAVE_GAME, DR_SAVE_READ_WRITE, ""},
        {"/native/tokens", DR_SAVE_DIRECTORY, DR_SAVE_SHARED, DR_SAVE_UNKNOWN, ""}};
    if (handoff_save_paths(initial, 3, true))
        atomic_fetch_add(&writes, 1);
    if (handoff_save_path("/native/shared/one", DR_SAVE_FILE, DR_SAVE_SHARED, DR_SAVE_READ_WRITE))
        atomic_fetch_add(&writes, 1);
    handoff_engine_end();
    return NULL;
}
static struct dr_save_entry malformed;
static atomic_int accepted;
static void *malformed_writer(void *unused)
{
    (void)unused;
    atomic_store(&accepted, handoff_save_paths(&malformed, 1, true));
    return NULL;
}
static void check_invalid_enums(void)
{
    for (unsigned field = 0; field < 3; ++field)
    {
        handoff_reset();
        handoff_save_enable(true);
        malformed = (struct dr_save_entry){"/native/invalid", DR_SAVE_DIRECTORY,
                                           DR_SAVE_GAME, DR_SAVE_UNKNOWN, ""};
        if (field == 0) malformed.kind = 999;
        if (field == 1) malformed.scope = 999;
        if (field == 2) malformed.policy = 999;
        atomic_store(&accepted, -1);
        pthread_t worker;
        assert(!pthread_create(&worker, NULL, malformed_writer, NULL));
        struct dr_save_layout layout;
        for (int n = 0; n < 1000 && atomic_load(&accepted) == -1; ++n)
        {
            handoff_save_copy(&layout);
            if (layout.checkpoint)
                assert(handoff_save_release(layout.generation));
            pause_ms();
        }
        pthread_join(worker, NULL);
        handoff_save_copy(&layout);
        assert(!atomic_load(&accepted) && !layout.available && !layout.complete && !layout.checkpoint);
    }
}
static void check_legacy_limits(void)
{
    handoff_reset();handoff_save_enable(true);
    struct dr_save_entry bad={.kind=DR_SAVE_RENPY_SLOTS,.scope=DR_SAVE_GAME,.policy=DR_SAVE_READ_WRITE};
    strcpy(bad.path,"/native/custom");strcpy(bad.suffix,"../bad");assert(!handoff_save_paths(&bad,1,true));
    memset(bad.suffix,'x',sizeof(bad.suffix));assert(!handoff_save_paths(&bad,1,true));
    handoff_reset(); /* optional frontend declined; representation limits do not cancel the game */
    assert(handoff_save_paths(NULL, DR_SAVE_MAX + 1, true));
    char long_path[DR_SAVE_PATH_MAX + 1];
    memset(long_path, 'x', sizeof(long_path));
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1] = 0;
    assert(handoff_save_path(long_path, DR_SAVE_DIRECTORY, DR_SAVE_GAME, DR_SAVE_READ_WRITE));
    assert(handoff_save_path("/native/invalid", 999, 999, 999));
    struct dr_save_layout layout;
    handoff_save_copy(&layout);
    assert(!layout.available && !layout.complete && !layout.checkpoint);
    assert(handoff_save_path("/native/valid", DR_SAVE_DIRECTORY, DR_SAVE_GAME, DR_SAVE_UNKNOWN));
    handoff_save_copy(&layout);
    assert(!layout.available && !layout.complete && !layout.checkpoint);
    handoff_stop(); /* cancellation remains cancellation even without the optional contract */
    assert(!handoff_save_path(long_path, DR_SAVE_DIRECTORY, DR_SAVE_GAME, DR_SAVE_READ_WRITE));
    handoff_reset();
    handoff_save_enable(true);
    assert(!handoff_save_paths(NULL, DR_SAVE_MAX + 1, true));
    assert(!handoff_save_path(long_path, DR_SAVE_DIRECTORY, DR_SAVE_GAME, DR_SAVE_READ_WRITE));
    handoff_save_copy(&layout);
    assert(!layout.available && !layout.complete && !layout.checkpoint);
}
int main(void)
{
    check_invalid_enums();
    check_legacy_limits();
    handoff_reset();
    assert(handoff_save_path("/native/nested/slot.save", DR_SAVE_RENPY_FILE, DR_SAVE_GAME, DR_SAVE_READ_WRITE));
    struct dr_save_layout artifact_layout;
    handoff_save_copy(&artifact_layout);
    assert(artifact_layout.count == 1 && artifact_layout.entries[0].kind == DR_SAVE_RENPY_FILE);
    assert(dr_save_entry_valid(artifact_layout.entries));
    strcpy(artifact_layout.entries[0].suffix, ".invalid");
    assert(!dr_save_entry_valid(artifact_layout.entries));
    for (int n = 0; n < 20; n++)
    {
        handoff_reset();
        handoff_save_enable(true);
        atomic_store(&writes, 0);
        pthread_t worker;
        assert(!pthread_create(&worker, NULL, writer, NULL));
        struct dr_save_layout layout;
        for (int i = 0; i < 1000; i++)
        {
            handoff_save_copy(&layout);
            if (layout.checkpoint)
                break;
            pause_ms();
        }
        assert(layout.count == 3 && layout.checkpoint && layout.initial && !atomic_load(&writes));
        assert(!handoff_save_release(layout.generation + 1));
        assert(handoff_save_release(layout.generation));
        for (int i = 0; i < 1000; i++)
        {
            handoff_save_copy(&layout);
            if (layout.count == 4)
                break;
            pause_ms();
        }
        assert(layout.count == 4 && layout.checkpoint && !layout.initial &&
               atomic_load(&writes) == 1);
        assert(layout.entries[3].scope == DR_SAVE_SHARED && layout.entries[3].kind == DR_SAVE_FILE);
        if (n % 2)
        {
            handoff_stop();
        }
        else
        {
            assert(handoff_save_release(layout.generation));
        }
        pthread_join(worker, NULL);
        assert(handoff_engine_ended());
        assert(atomic_load(&writes) == ((n % 2) ? 1 : 2));
    }
    handoff_reset();
    handoff_save_enable(true);
    assert(
        !handoff_save_path("relative/unknown", DR_SAVE_DIRECTORY, DR_SAVE_GAME, DR_SAVE_UNKNOWN));
    struct dr_save_layout invalid;
    handoff_save_copy(&invalid);
    assert(!invalid.available && !invalid.complete);
    handoff_reset();
    for (unsigned i = 0; i < DR_SAVE_MAX; ++i)
    {
        char path[64];
        snprintf(path, sizeof(path), "/native/root-%u", i);
        assert(handoff_save_path(path, DR_SAVE_DIRECTORY, DR_SAVE_GAME, DR_SAVE_READ_ONLY));
    }
    handoff_save_enable(true);
    assert(
        !handoff_save_path("/native/overflow", DR_SAVE_DIRECTORY, DR_SAVE_GAME, DR_SAVE_READ_ONLY));
    handoff_save_copy(&invalid);
    assert(invalid.count == DR_SAVE_MAX && !invalid.complete);
    /* An early individual discovery is not the globally quiescent initial union. */
    handoff_reset();
    assert(handoff_save_path("/native/early", DR_SAVE_FILE, DR_SAVE_SHARED, DR_SAVE_READ_WRITE));
    handoff_save_copy(&invalid);
    assert(!invalid.initial);
    struct dr_save_entry batch[] = {
        {"/native/configured", DR_SAVE_DIRECTORY, DR_SAVE_GAME, DR_SAVE_READ_WRITE, ""},
        {"/native/configured", DR_SAVE_DIRECTORY, DR_SAVE_SHARED, DR_SAVE_READ_WRITE, ""},
        {"/native/extra", DR_SAVE_DIRECTORY, DR_SAVE_GAME, DR_SAVE_READ_WRITE, ""}};
    assert(handoff_save_paths(batch, 3, true));
    handoff_save_copy(&invalid);
    assert(invalid.initial && invalid.generation == 2 && invalid.count == 3);
    assert(invalid.entries[1].scope == DR_SAVE_SHARED);
    /* Batch failure is atomic; repeating initial publication never grants a new initial hold. */
    batch[1].path[0] = 0;
    handoff_save_enable(true);
    assert(!handoff_save_paths(batch, 3, false));
    handoff_save_copy(&invalid);
    assert(!invalid.complete && invalid.count == 3 && invalid.generation == 2);
    handoff_save_enable(false);
    strcpy(batch[0].path, "/native/later");
    assert(handoff_save_paths(batch, 1, true));
    handoff_save_copy(&invalid);
    assert(!invalid.initial && invalid.generation == 3);
    puts("PASS checkpoint holds native writer, exact epochs, shared files, stop wakes, repeated "
         "transitions");
}
