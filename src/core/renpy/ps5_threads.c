/*
 * Dulcamara - Ren'Py's cores on the PS5: the threads the core's libraries start. Ren'Py starts one to decode each
 * sound it plays (ffmedia.c, through SDL), Python its own, ps5-opengl its workers: each core's relocatable link sends
 * their pthread_create here (--wrap, tools/renpy/ps5.sh and tools/renpy7/ps5.sh). Each thread gets a stack of 2 MB at
 * least, where the console's default may be far smaller, and a stack of its own for signals: a thread that overflows
 * its stack is then reported by the app's crash handler (SA_ONSTACK) instead of ending the app without a word, as
 * 0.2.6 ended on a game's first hover, which plays the first sound after its music. A thread the console refuses is
 * logged, whatever the trace's count, with how many of the core's run: 0.2.8's had a movie's decoder refused them.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if defined(__PROSPERO__)

#include "log.h"

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>

#define LEAST_STACK ((size_t)2 << 20)
#define SIGNAL_STACK ((size_t)128 << 10)

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);

typedef struct
{
    void *(*start)(void *);
    void *arg;
} Start;

/* threads started and ended, in the log as they come (the first 200): a short sound's decode thread ends at once */
static int traced;

static bool trace_thread(void)
{
    return __atomic_fetch_add(&traced, 1, __ATOMIC_RELAXED) < 200;
}

static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static pthread_key_t signal_stack_key;
static bool key_made;

/* the core's threads that run now (those with a signal stack: nearly all), and the most there have been at once */
static int running, most;

/* a thread's signal stack, freed as it ends (by returning or by pthread_exit), once the system has let it go */
static void free_signal_stack(void *stack)
{
    if (trace_thread())
        log_now(1, "renpy: trace: a thread ends");
    __atomic_sub_fetch(&running, 1, __ATOMIC_RELAXED);
    stack_t off = {0};
    off.ss_flags = SS_DISABLE;
    sigaltstack(&off, NULL);
    free(stack);
}

static void make_key(void)
{
    key_made = pthread_key_create(&signal_stack_key, free_signal_stack) == 0;
}

static void *with_signal_stack(void *p)
{
    const Start s = *(Start *)p;
    free(p);
    stack_t own = {0};
    own.ss_sp = malloc(SIGNAL_STACK);
    own.ss_size = SIGNAL_STACK;
    if (own.ss_sp && key_made && sigaltstack(&own, NULL) == 0 && pthread_setspecific(signal_stack_key, own.ss_sp) == 0)
    {
        /* the most at once, in the log each time it passes another 8 */
        const int now = __atomic_add_fetch(&running, 1, __ATOMIC_RELAXED);
        int was = __atomic_load_n(&most, __ATOMIC_RELAXED);
        while (now > was && !__atomic_compare_exchange_n(&most, &was, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            ;
        if (now > was && now % 8 == 0)
            log_queue(1, "renpy: threads: %d of the core's run at once, the most yet", now);
    }
    else
        free(own.ss_sp);
    return s.start(s.arg);
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    pthread_once(&key_once, make_key);
    static bool noted;
    pthread_attr_t own;
    const bool made = !attr && pthread_attr_init(&own) == 0;
    pthread_attr_t *use = made ? &own : (pthread_attr_t *)attr;
    size_t size = 0;
    void *address = NULL;
    if (use && pthread_attr_getstacksize(use, &size) == 0 && size < LEAST_STACK &&
        (pthread_attr_getstackaddr(use, &address) != 0 || !address))
    {
        if (!noted)
        {
            noted = true;
            log_queue(1, "renpy: threads: a stack of %zu KB asked for or by default; the core's get %zu KB at least",
                      size >> 10, LEAST_STACK >> 10);
        }
        pthread_attr_setstacksize(use, LEAST_STACK);
    }
    Start *s = malloc(sizeof(*s));
    int rc;
    if (trace_thread())
        log_now(1, "renpy: trace: a thread starts at %p", (void *)start);
    if (s)
    {
        s->start = start;
        s->arg = arg;
        rc = __real_pthread_create(thread, use, with_signal_stack, s);
        if (rc)
            free(s);
    }
    else
        rc = __real_pthread_create(thread, use, start, arg);
    static int refused;
    if (rc && __atomic_fetch_add(&refused, 1, __ATOMIC_RELAXED) < 100)
        log_now(2, "renpy: threads: the console refused a thread (%d), with %d of the core's running (the most yet %d)",
                rc, __atomic_load_n(&running, __ATOMIC_RELAXED), __atomic_load_n(&most, __ATOMIC_RELAXED));
    if (made)
        pthread_attr_destroy(&own);
    return rc;
}

#endif
