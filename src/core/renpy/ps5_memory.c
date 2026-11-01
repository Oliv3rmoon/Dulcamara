/*
 * Dulcamara - Ren'Py's cores on the PS5: the direct memory the core's libraries hold. ps5-opengl keeps its textures,
 * buffers and command memory in direct memory it allocates itself (sceKernelAllocateDirectMemory), outside the app's
 * heap, so each core's relocatable link sends those two calls here (--wrap, tools/renpy/ps5.sh and tools/renpy7/ps5.sh)
 * to count them. The memory line Ren'Py's side logs every few minutes (dulcamara_errors) reads the count, so a long
 * session's log shows whether it grows: the console itself went down after one. And as it happens: each allocation
 * the console refuses, and each time what is held passes another 64 MB, the most yet (a tester's game stopped
 * answering as she moved the cursor fast over its title menu, she thought for the graphics running out).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if defined(__PROSPERO__)

#include "log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int __real_sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length, size_t alignment,
                                         int memory_type, int64_t *physical_address);
int __real_sceKernelReleaseDirectMemory(int64_t start, size_t length);
int sceKernelAvailableFlexibleMemorySize(size_t *size);

static int64_t held_bytes, held_blocks, most_steps;

#define STEP ((int64_t)64 << 20)

int __wrap_sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length, size_t alignment,
                                         int memory_type, int64_t *physical_address)
{
    const int rc = __real_sceKernelAllocateDirectMemory(search_start, search_end, length, alignment, memory_type,
                                                        physical_address);
    if (rc == 0)
    {
        const int64_t held = __atomic_add_fetch(&held_bytes, (int64_t)length, __ATOMIC_RELAXED);
        const int64_t blocks = __atomic_add_fetch(&held_blocks, 1, __ATOMIC_RELAXED);
        int64_t was = __atomic_load_n(&most_steps, __ATOMIC_RELAXED);
        const int64_t steps = held / STEP;
        if (steps > was && __atomic_compare_exchange_n(&most_steps, &was, steps, false, __ATOMIC_RELAXED,
                                                       __ATOMIC_RELAXED))
            log_queue(1, "renpy: memory: OpenGL's memory %lld MB in %lld blocks, the most yet", (long long)(held >> 20),
                      (long long)blocks);
    }
    else
    {
        static int refused;
        if (__atomic_fetch_add(&refused, 1, __ATOMIC_RELAXED) < 50)
            log_now(2, "renpy: memory: the console refused OpenGL %zu KB (0x%x), with %lld MB in %lld blocks held",
                    length >> 10, (unsigned)rc, (long long)(__atomic_load_n(&held_bytes, __ATOMIC_RELAXED) >> 20),
                    (long long)__atomic_load_n(&held_blocks, __ATOMIC_RELAXED));
    }
    return rc;
}

int __wrap_sceKernelReleaseDirectMemory(int64_t start, size_t length)
{
    const int rc = __real_sceKernelReleaseDirectMemory(start, length);
    if (rc == 0)
    {
        __atomic_sub_fetch(&held_bytes, (int64_t)length, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&held_blocks, 1, __ATOMIC_RELAXED);
    }
    return rc;
}

/* the direct memory the core's libraries hold now, and in how many allocations */
int64_t ps5_memory_direct_bytes(void)
{
    return __atomic_load_n(&held_bytes, __ATOMIC_RELAXED);
}

int64_t ps5_memory_direct_blocks(void)
{
    return __atomic_load_n(&held_blocks, __ATOMIC_RELAXED);
}

/* The console's flexible memory still free, or -1: its small system memory, where a thread's stack is made and what
 * mmap gives comes from (0.2.10's Ren'Py 7 could not start a thread with 6 of the core's running) */
int64_t ps5_memory_flexible_free(void)
{
    size_t size = 0;
    return sceKernelAvailableFlexibleMemorySize(&size) == 0 ? (int64_t)size : -1;
}

#endif
