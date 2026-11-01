/*
 * Dulcamara - Ren'Py's cores on the PS5: the direct memory the core's libraries hold. ps5-opengl keeps its textures,
 * buffers and command memory in direct memory it allocates itself (sceKernelAllocateDirectMemory), outside the app's
 * heap, so each core's relocatable link sends those two calls here (--wrap, tools/renpy/ps5.sh and tools/renpy7/ps5.sh)
 * to count them. The memory line Ren'Py's side logs every few minutes (dulcamara_errors) reads the count, so a long
 * session's log shows whether it grows: the console itself went down after one.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if defined(__PROSPERO__)

#include <stddef.h>
#include <stdint.h>

int __real_sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length, size_t alignment,
                                         int memory_type, int64_t *physical_address);
int __real_sceKernelReleaseDirectMemory(int64_t start, size_t length);

static int64_t held_bytes, held_blocks;

int __wrap_sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length, size_t alignment,
                                         int memory_type, int64_t *physical_address)
{
    const int rc = __real_sceKernelAllocateDirectMemory(search_start, search_end, length, alignment, memory_type,
                                                        physical_address);
    if (rc == 0)
    {
        __atomic_add_fetch(&held_bytes, (int64_t)length, __ATOMIC_RELAXED);
        __atomic_add_fetch(&held_blocks, 1, __ATOMIC_RELAXED);
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

#endif
