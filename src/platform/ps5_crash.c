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
 * installed, which the log lists too), the report gives the registers and the
 * code around the faulting instruction where it can be read, says whether the
 * main thread or another crashed, and reads the stack only where the crashing
 * thread's is mapped (a bad stack pointer crashed the handler itself).
 *
 * The console's kernel puts FreeBSD's amd64 mcontext_t at offset 64 of the ucontext a handler is given, not at the
 * SDK header's 16 (as a PS5 port of Wine measured on firmware 12.02): Dulcamara 0.2.0 to 0.2.2 read every register
 * 48 bytes early, their "rsp" the fault address and their "rip" r14. The report takes the layout whose fault address
 * or instruction pointer is the signal's own, and names an address outside the listed modules by asking the kernel
 * which module holds it (the list has the title's modules, not the system's).
 */
#if defined(__PROSPERO__)
#include "ps5_crash.h"

#include "../nightshade.h"

#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

extern int main(void);

int32_t sceKernelGetModuleList(int32_t *handles, size_t size, size_t *available);
int32_t sceKernelGetModuleInfo(int32_t handle, void *info);
int32_t sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection);
int32_t sceKernelGetModuleInfoForUnwind(void *address, int32_t flags, void *info);

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

/* the kernel's answer to which module holds an address (0x130 bytes, as the open PS4 SDK declares it) */
typedef struct
{
    uint64_t size;
    char name[256];
    uint64_t eh_frame_header, eh_frame, eh_frame_size;
    uint64_t start, length; /* the first segment's */
} UnwindInfo;

_Static_assert(sizeof(UnwindInfo) == 0x130, "the kernel's unwind record is 0x130 bytes");

/* where the console puts the registers in a handler's ucontext: offset 64, then the header's, to be checked */
static const size_t CONTEXT_OFFSETS[] = {64, offsetof(ucontext_t, uc_mcontext)};

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

/* a system module holding addr, as the kernel names it: its name, and its first segment's start */
static bool system_module_of(uintptr_t addr, UnwindInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    if (sceKernelGetModuleInfoForUnwind((void *)addr, 1, info) != 0 || !info->name[0])
        return false;
    info->name[sizeof(info->name) - 1] = '\0';
    return true;
}

static void report_address(const char *label, uintptr_t addr)
{
    char line[300];
    const intptr_t offset = (intptr_t)addr - (intptr_t)(uintptr_t)&main;
    const Region *r = region_of(addr);
    UnwindInfo info;
    if (r)
        snprintf(line, sizeof(line), "  %s 0x%llx (main%+lld; %s+0x%llx)\n", label, (unsigned long long)addr,
                 (long long)offset, r->name, (unsigned long long)(addr - r->start));
    else if (system_module_of(addr, &info))
        snprintf(line, sizeof(line), "  %s 0x%llx (main%+lld; %s, whose first segment is at 0x%llx)\n", label,
                 (unsigned long long)addr, (long long)offset, info.name, (unsigned long long)info.start);
    else
        snprintf(line, sizeof(line), "  %s 0x%llx (main%+lld)\n", label, (unsigned long long)addr,
                 (long long)offset);
    put(line);
}

/* the registers in the handler's context, at the layout whose fault address (a page fault's) or instruction pointer
 * (another fault's) is the signal's; offset 64 when neither says, as measured */
static const mcontext_t *registers_of(const void *context, uintptr_t fault, size_t *offset, bool *checked)
{
    for (size_t i = 0; i < sizeof(CONTEXT_OFFSETS) / sizeof(CONTEXT_OFFSETS[0]); ++i)
    {
        const mcontext_t *m = (const mcontext_t *)((const char *)context + CONTEXT_OFFSETS[i]);
        if (fault && ((uintptr_t)m->mc_addr == fault || (uintptr_t)m->mc_rip == fault))
        {
            *offset = CONTEXT_OFFSETS[i];
            *checked = true;
            return m;
        }
    }
    *offset = CONTEXT_OFFSETS[0];
    *checked = false;
    return (const mcontext_t *)((const char *)context + CONTEXT_OFFSETS[0]);
}

/* code the stack may return into: the title's, a listed module's, or any the kernel maps executable */
static bool code_address(uintptr_t v)
{
    if (title_end ? v >= title_start && v < title_end
                  : v > (uintptr_t)&main - 0x4000000 && v < (uintptr_t)&main + 0x4000000)
        return true;
    if (region_of(v))
        return true;
    void *start = NULL, *end = NULL;
    uint32_t protection = 0;
    return v >= 0x10000 && v < 0x800000000000ULL &&
           sceKernelQueryMemoryProtection((void *)v, &start, &end, &protection) == 0 && (protection & 4) &&
           v >= (uintptr_t)start && v < (uintptr_t)end;
}

static void on_crash(int sig, siginfo_t *info, void *context)
{
    char line[240];
    const uintptr_t fault = info ? (uintptr_t)info->si_addr : 0;
    snprintf(line, sizeof(line), "CRASH: signal %d at address %p during '%s'; main at %p\n", sig, (void *)fault,
             last_step, (void *)(uintptr_t)&main);
    put(line);
    if (context)
    {
        size_t offset = 0;
        bool checked = false;
        const mcontext_t *m = registers_of(context, fault, &offset, &checked);
        const uintptr_t rip = (uintptr_t)m->mc_rip;
        const uintptr_t rsp = (uintptr_t)m->mc_rsp;
        snprintf(line, sizeof(line), "  registers at context offset %zu (%s); trap %d, error %llx\n", offset,
                 checked ? "their fault address is the signal's" : "not confirmed by the fault address",
                 (int)m->mc_trapno, (unsigned long long)m->mc_err);
        put(line);
        report_address("rip", rip);
        report_address("fault", fault);
        snprintf(line, sizeof(line), "  rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx\n",
                 (unsigned long long)m->mc_rax, (unsigned long long)m->mc_rbx, (unsigned long long)m->mc_rcx,
                 (unsigned long long)m->mc_rdx, (unsigned long long)m->mc_rsi, (unsigned long long)m->mc_rdi,
                 (unsigned long long)m->mc_rbp);
        put(line);
        snprintf(line, sizeof(line), "  r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx\n",
                 (unsigned long long)m->mc_r8, (unsigned long long)m->mc_r9, (unsigned long long)m->mc_r10,
                 (unsigned long long)m->mc_r11, (unsigned long long)m->mc_r12, (unsigned long long)m->mc_r13,
                 (unsigned long long)m->mc_r14, (unsigned long long)m->mc_r15);
        put(line);
        /* the code around the faulting instruction, where it can be read (a system module's may not be) */
        void *code_start = NULL, *code_end = NULL;
        uint32_t code_protection = 0;
        if (sceKernelQueryMemoryProtection((void *)rip, &code_start, &code_end, &code_protection) == 0 &&
            (code_protection & 1) && rip >= (uintptr_t)code_start + 16 && rip + 16 <= (uintptr_t)code_end)
        {
            const uint8_t *code = (const uint8_t *)(rip - 16);
            int at = snprintf(line, sizeof(line), "  code at rip-16:");
            for (int i = 0; i < 32 && at < (int)sizeof(line) - 4; ++i)
                at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x", code[i]);
            snprintf(line + at, sizeof(line) - (size_t)at, "\n");
            put(line);
        }
        else
            put("  (the code at rip cannot be read)\n");
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
        const size_t words = mapped && !(rsp & 7) ? ((uintptr_t)stack_end - rsp) / sizeof(uintptr_t) : 0;
        const uintptr_t *stack = (const uintptr_t *)rsp;
        int found = 0;
        for (size_t i = 0; i < words && i < 1024 && found < 24; ++i)
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
    /* through the log itself (a write of this fd's at the file's start would be written over) */
    np_log("crash report: %d module regions listed, the title %s", region_count,
           title_end ? "among them" : "not among them");
    for (int i = 0; i < region_count; ++i)
        np_log("module: %s 0x%llx-0x%llx", regions[i].name, (unsigned long long)regions[i].start,
               (unsigned long long)regions[i].end);
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
