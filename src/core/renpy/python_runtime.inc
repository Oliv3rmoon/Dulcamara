/* Shared native-save bindings and lifecycle implementation for CPython 2 and 3.
 * Included once by each version adapter after its private thread/interpreter state.
 * Each engine compiles its own copy against its Python ABI; statics and archive symbol
 * isolation remain per engine. The only callbacks below use version-specific Python APIs.
 * SPDX-License-Identifier: GPL-3.0-or-later */
static int quit_on_renpy_thread(void *unused);
static void *log_where_renpy_is(void *unused);

static PyObject *py_save_path(PyObject *self, PyObject *args)
{
    (void)self;
    const char *path, *suffix = "";
    unsigned kind, scope, policy;
    if (!PyArg_ParseTuple(args, "sIII|s", &path, &kind, &scope, &policy, &suffix))
        return NULL;
    struct dr_save_entry entry = {.kind = kind, .scope = scope, .policy = policy};
    bool valid = strlen(path) < sizeof(entry.path) && strlen(suffix) < sizeof(entry.suffix);
    if (valid)
    {
        strcpy(entry.path, path);
        strcpy(entry.suffix, suffix);
    }
    bool ok;
    Py_BEGIN_ALLOW_THREADS ok = handoff_save_paths(&entry, valid ? 1 : 0, false);
    Py_END_ALLOW_THREADS if (!ok)
    {
        PyErr_SetNone(PyExc_SystemExit);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *py_save_paths(PyObject *self, PyObject *arg)
{
    (void)self;
    PyObject *seq = PySequence_Fast(arg, "native save entries must be a sequence");
    if (!seq)
        return NULL;
    Py_ssize_t count = PySequence_Fast_GET_SIZE(seq);
    struct dr_save_entry entries[DR_SAVE_MAX] = {{0}};
    bool valid = count > 0 && count <= DR_SAVE_MAX;
    for (Py_ssize_t i = 0; valid && i < count; ++i)
    {
        const char *path, *suffix = "";
        unsigned kind, scope, policy;
        if (!PyArg_ParseTuple(PySequence_Fast_GET_ITEM(seq, i), "sIII|s", &path, &kind, &scope,
                              &policy, &suffix))
        {
            Py_DECREF(seq);
            return NULL;
        }
        valid =
            strlen(path) < sizeof(entries[i].path) && strlen(suffix) < sizeof(entries[i].suffix);
        if (valid)
        {
            strcpy(entries[i].path, path);
            strcpy(entries[i].suffix, suffix);
            entries[i].kind = kind;
            entries[i].scope = scope;
            entries[i].policy = policy;
        }
    }
    Py_DECREF(seq);
    bool ok;
    Py_BEGIN_ALLOW_THREADS ok = handoff_save_paths(entries, valid ? (uint32_t)count : 0, true);
    Py_END_ALLOW_THREADS if (!ok)
    {
        PyErr_SetNone(PyExc_SystemExit);
        return NULL;
    }
    Py_RETURN_NONE;
}

static void wait_for_interpreter_threads(void)
{
    /* Python-owned workers may still be in native file I/O with the GIL released.
     * Wait for every interpreter thread state, including raw _thread users, before
     * finalizing Python or releasing SDL/audio resources. The frontend polls a real
     * monotonic timeout independently; a surviving worker never certifies clean. */
    bool waiting_logged = false;
    for (;;)
    {
        PyThreadState *current = PyThreadState_Get();
        unsigned others = 0;
        for (PyThreadState *item = PyInterpreterState_ThreadHead(current->interp); item;
             item = PyThreadState_Next(item))
            if (item != current)
                ++others;
        if (!others)
            break;
        if (!waiting_logged)
        {
            log_queue(1, "renpy: waiting for %u interpreter threads; writers are not finished",
                      others);
            waiting_logged = true;
        }
        Py_BEGIN_ALLOW_THREADS usleep(10000);
        Py_END_ALLOW_THREADS
    }
}

/* Ren'Py's ffmedia.c (tools/patches/renpy-ffmedia-close-before-python-ends.diff): waits, without the GIL, for the
 * decode threads asked to close their files so far; a close asked afterwards leaves its file unclosed. */
int media_python_ending(double seconds);

/* A movie's or sound's decode thread closes its file once Ren'Py has closed the media (the game's music, stopped as it
 * ends), on its own time, and through Python for a sound of the game's archives: once Python had ended, that took the
 * GIL of an interpreter that was gone, and crashed the process, which goes on after a game. Called with the GIL, just
 * before the interpreter threads are waited for and Python ends. */
static void wait_for_media_closes(void)
{
    int unfinished;
    Py_BEGIN_ALLOW_THREADS unfinished = media_python_ending(5.0);
    Py_END_ALLOW_THREADS if (unfinished)
        log_queue(1, "renpy: %d sound files still closing after five seconds are left unclosed as Python ends",
                  unfinished);
}

/* Asks Python to raise the quit on Ren'Py's thread; without the GIL (Py_AddPendingCall needs none).
 * With where, also logs where Ren'Py is, from a thread of its own. */
static void ask_renpy_to_quit(bool where)
{
    pthread_mutex_lock(&state_lock);
    if (interpreter_up)
        Py_AddPendingCall(quit_on_renpy_thread, NULL);
    pthread_mutex_unlock(&state_lock);
    pthread_t logger;
    if (where && pthread_create(&logger, NULL, log_where_renpy_is, NULL) == 0)
        pthread_detach(logger);
}

static double stop_started;
static unsigned stop_escalation;
static bool stop_timed_out;
static double real_monotonic(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
void python_request_stop(void)
{
    if (stop_started || !thread_live)
        return;
    stop_started = real_monotonic();
    stop_escalation = 0;
    handoff_stop();
}
enum dr_stop_result python_poll_stop(void)
{
    /* Timeout is sticky: no later completion silently grants backup/relaunch. */
    if (stop_timed_out)
        return DR_STOP_TIMEOUT;
    if (!thread_live)
        return DR_STOP_CLEAN;
    if (handoff_engine_ended())
    {
        pthread_join(thread, NULL);
        thread_live = false;
        return DR_STOP_CLEAN;
    }
    if (!stop_started)
        return DR_STOP_RUNNING;
    double elapsed = real_monotonic() - stop_started;
    if (elapsed >= 1 && stop_escalation == 0)
    {
        ask_renpy_to_quit(false);
        stop_escalation = 1;
    }
    if (elapsed >= 3 && stop_escalation == 1)
    {
        ask_renpy_to_quit(true);
        stop_escalation = 2;
    }
    if (elapsed >= 10)
    {
        stop_timed_out = true;
        log_queue(2, "renpy: Ren'Py did not stop within ten seconds; writers remain unsafe");
        return DR_STOP_TIMEOUT;
    }
    return DR_STOP_PENDING;
}
bool python_stop(void)
{
    python_request_stop();
    enum dr_stop_result result;
    while ((result = python_poll_stop()) == DR_STOP_PENDING)
        usleep(10000);
    return result == DR_STOP_CLEAN;
}
