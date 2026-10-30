/*
 * SwanStationPS5 - writes a crash report into SwanStationPS5.log on the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Shell only says "something went wrong". On a fatal signal this logs
 * the signal, the faulting address, the instruction pointer and a short
 * frame-pointer walk, each also as an offset from main(), so the CI build's
 * symbol map (SwanStationPS5-symbols.txt) turns them into function names.
 *
 * Dulcamara: each address is also named by the module it falls in (the title's
 * eboot.bin or a system module, as the kernel listed them when the handler was
 * installed), the report says whether the main thread or another crashed, and
 * the stack is read only as far as the crashing thread's is mapped (a thread
 * that had just started crashed the handler itself, reading past its top).
 */
#if defined(__PROSPERO__)
#include "ps5_crash.h"

#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

extern int main(void);

int32_t sceKernelGetModuleList(int32_t *handles, size_t size, size_t *available);
int32_t sceKernelGetModuleInfo(int32_t handle, void *info);
int32_t sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection);

/* the kernel's module record (0x160 bytes, as the open PS4 and PS5 SDKs declare it) */
typedef struct
{
    uint64_t address;
    uint32_t size;
    int32_t protection;
} ModuleSegment;

typedef struct
{
    uint64_t size;
    char name[256];
    ModuleSegment segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
} ModuleInfo;

_Static_assert(sizeof(ModuleInfo) == 0x160, "the kernel's module record is 0x160 bytes");

typedef struct
{
    uintptr_t start, end;
    char name[40];
} Region;

static int log_fd = -1;
static const char *last_step = "start";
static Region regions[160];
static int region_count;
static uintptr_t title_start, title_end; /* the module holding main: the title */
static uintptr_t main_stack_start, main_stack_end;

void ps5_crash_step(const char *step)
{
    last_step = step;
}

static void put(const char *s)
{
    if (log_fd >= 0)
        (void)write(log_fd, s, strlen(s));
}

static const Region *region_of(uintptr_t addr)
{
    for (int i = 0; i < region_count; ++i)
        if (addr >= regions[i].start && addr < regions[i].end)
            return &regions[i];
    return NULL;
}

static void report_address(const char *label, uintptr_t addr)
{
    char line[200];
    const intptr_t offset = (intptr_t)addr - (intptr_t)(uintptr_t)&main;
    const Region *r = region_of(addr);
    if (r)
        snprintf(line, sizeof(line), "  %s 0x%llx (main%+lld; %s+0x%llx)\n", label, (unsigned long long)addr,
                 (long long)offset, r->name, (unsigned long long)(addr - r->start));
    else
        snprintf(line, sizeof(line), "  %s 0x%llx (main%+lld)\n", label, (unsigned long long)addr,
                 (long long)offset);
    put(line);
}

/* code the stack may return into: the title's, or a system module's */
static bool code_address(uintptr_t v)
{
    if (title_end ? v >= title_start && v < title_end
                  : v > (uintptr_t)&main - 0x4000000 && v < (uintptr_t)&main + 0x4000000)
        return true;
    return region_of(v) != NULL;
}

static void on_crash(int sig, siginfo_t *info, void *context)
{
    char line[240];
    const uintptr_t fault = info ? (uintptr_t)info->si_addr : 0;
    snprintf(line, sizeof(line), "CRASH: signal %d at address %p during '%s'; main at %p\n", sig, (void *)fault,
             last_step, (void *)(uintptr_t)&main);
    put(line);
    ucontext_t *uc = (ucontext_t *)context;
    if (uc)
    {
        const uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip;
        const uintptr_t rsp = (uintptr_t)uc->uc_mcontext.mc_rsp;
        report_address("rip", rip);
        report_address("fault", fault);
        const bool main_thread = rsp >= main_stack_start && rsp < main_stack_end;
        void *stack_start = NULL, *stack_end = NULL;
        uint32_t protection = 0;
        const bool mapped =
            sceKernelQueryMemoryProtection((void *)rsp, &stack_start, &stack_end, &protection) == 0 &&
            (uintptr_t)stack_end > rsp;
        snprintf(line, sizeof(line), "  thread: %s, rsp 0x%llx, its stack mapped to 0x%llx\n",
                 main_thread ? "the main thread" : "another thread", (unsigned long long)rsp,
                 (unsigned long long)(mapped ? (uintptr_t)stack_end : 0));
        put(line);
        /* return addresses near the top of the stack (works without frame pointers), as far as it is mapped */
        const size_t words = mapped ? ((uintptr_t)stack_end - rsp) / sizeof(uintptr_t) : 64;
        const uintptr_t *stack = (const uintptr_t *)rsp;
        int found = 0;
        for (size_t i = 0; i < words && i < 1024 && found < 16; ++i)
            if (code_address(stack[i]))
            {
                report_address("stack", stack[i]);
                ++found;
            }
        if (!region_count)
            put("  (the kernel gave no module list)\n");
    }
    put("END CRASH\n");
    if (log_fd >= 0)
        fsync(log_fd);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Where the title and the system modules are, while the kernel can still be asked; and the main thread's stack. */
static void list_modules(void)
{
    int32_t handles[128];
    size_t available = 0;
    if (sceKernelGetModuleList(handles, sizeof(handles) / sizeof(handles[0]), &available) != 0)
        return;
    if (available > sizeof(handles) / sizeof(handles[0]))
        available = sizeof(handles) / sizeof(handles[0]);
    for (size_t m = 0; m < available; ++m)
    {
        ModuleInfo module;
        memset(&module, 0, sizeof(module));
        module.size = sizeof(module);
        if (sceKernelGetModuleInfo(handles[m], &module) != 0)
            continue;
        module.name[sizeof(module.name) - 1] = '\0';
        const uint32_t segments = module.segment_count < 4 ? module.segment_count : 4;
        for (uint32_t s = 0; s < segments && region_count < (int)(sizeof(regions) / sizeof(regions[0])); ++s)
        {
            const ModuleSegment *segment = &module.segments[s];
            if (!segment->address || !segment->size)
                continue;
            Region *r = &regions[region_count++];
            r->start = (uintptr_t)segment->address;
            r->end = r->start + segment->size;
            snprintf(r->name, sizeof(r->name), "%s", module.name);
            if ((uintptr_t)&main >= r->start && (uintptr_t)&main < r->end)
            {
                title_start = r->start;
                title_end = r->end;
            }
        }
    }
    int here = 0;
    void *start = NULL, *end = NULL;
    uint32_t protection = 0;
    if (sceKernelQueryMemoryProtection(&here, &start, &end, &protection) == 0)
    {
        main_stack_start = (uintptr_t)start;
        main_stack_end = (uintptr_t)end;
    }
}

void ps5_crash_install(const char *log_path)
{
    log_fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT, 0666);
    list_modules();
    if (log_fd >= 0)
    {
        char line[120];
        snprintf(line, sizeof(line), "crash report: %d module regions listed, the title %s\n", region_count,
                 title_end ? "among them" : "not among them");
        put(line);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i)
        sigaction(signals[i], &sa, NULL);
}
#endif
