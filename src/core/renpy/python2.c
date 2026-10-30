/*
 * Dulcamara - Ren'Py 7's core: Ren'Py's own thread, with the Python 2.7 it runs in (python.h; python.c is Ren'Py 8's,
 * with Python 3). CPython 2.7 is linked in, its standard library the engine folder's lib/python27.zip, Ren'Py's and
 * pygame_sdl2's compiled modules built in (inittab.c, written by tools/build-renpy7.sh), and _dulcamara gives the boot
 * script (boot2.py, the engine's dulcamara_boot.py) the core's clock and log. As with Ren'Py 8, it runs one game each
 * time the app starts, and handles no signals.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "python.h"

#include <Python.h>
#include <pythread.h>

#include "audio.h"
#include "frames.h"
#include "handoff.h"
#include "hooks.h"
#include "input.h"
#include "log.h"

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

void renpy_extend_inittab(void); /* inittab.c */

static pthread_t thread;
static bool thread_live, ran_a_game;
static char engine[1024], game[1024], saves[1024];
static double wall_start; /* seconds since 1970 when the game started: time.time() is this plus the clock */

/* Whether another thread may raise an exception in Ren'Py's: only while its interpreter is up. */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_changed = PTHREAD_COND_INITIALIZER;
static bool interpreter_up, stopper_inside;
static long renpy_ident;

/* ---------------------------------------------------------------- _dulcamara */

static PyObject *py_time(PyObject *self, PyObject *unused)
{
    (void)self, (void)unused;
    return PyFloat_FromDouble(wall_start + (double)handoff_now_usec() / 1e6);
}

static PyObject *py_monotonic(PyObject *self, PyObject *unused)
{
    (void)self, (void)unused;
    return PyFloat_FromDouble((double)handoff_now_usec() / 1e6);
}

static PyObject *py_sleep(PyObject *self, PyObject *arg)
{
    (void)self;
    const double seconds = PyFloat_AsDouble(arg);
    if (seconds == -1.0 && PyErr_Occurred())
        return NULL;
    const int64_t until = handoff_now_usec() + (int64_t)(seconds > 0 ? seconds * 1e6 : 0);
    Py_BEGIN_ALLOW_THREADS
    hooks_sleep_until(until);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static PyObject *py_log(PyObject *self, PyObject *args)
{
    (void)self;
    int level;
    const char *text;
    if (!PyArg_ParseTuple(args, "is", &level, &text))
        return NULL;
    log_queue(level, "renpy: %s", text);
    Py_RETURN_NONE;
}

static PyMethodDef methods[] = {
    {"time", py_time, METH_NOARGS, "time.time() by the core's clock."},
    {"monotonic", py_monotonic, METH_NOARGS, "time.time() since the game started, by the core's clock."},
    {"sleep", py_sleep, METH_O, "time.sleep() on Ren'Py's thread, by the core's clock."},
    {"log", py_log, METH_VARARGS, "log(level, text): a line of Dulcamara's log (text in UTF-8)."},
    {NULL, NULL, 0, NULL},
};

static void init_dulcamara(void)
{
    Py_InitModule("_dulcamara", methods);
}

/* ---------------------------------------------------------------- Ren'Py's thread */

static void *renpy_main(void *unused)
{
    (void)unused;
    handoff_engine_begin();
    static char boot[1100], home[1024], path[4200], name[] = "renpy", encoding[] = "utf-8";
    snprintf(boot, sizeof(boot), "%s/dulcamara_boot.py", engine);
    snprintf(home, sizeof(home), "%s", engine);
    snprintf(path, sizeof(path), "%s/lib/python27.zip:%s/lib/site-packages:%s", engine, engine, engine);

    Py_NoSiteFlag = 1;
    Py_IgnoreEnvironmentFlag = 1;
    Py_DontWriteBytecodeFlag = 1;
    Py_NoUserSiteDirectory = 1;
    Py_FrozenFlag = 1; /* no warnings about the path */
    Py_FileSystemDefaultEncoding = encoding; /* file names are UTF-8, as the console's are */
    Py_SetProgramName(name);
    Py_SetPythonHome(home); /* the standard library: <engine>/lib/python27.zip */
    PyImport_AppendInittab("_dulcamara", init_dulcamara);
    renpy_extend_inittab();
    Py_InitializeEx(0);
    PyEval_InitThreads(); /* the GIL, for Ren'Py's own threads and for python_stop */
    char *argv[] = {boot, engine, game, saves};
    PySys_SetArgvEx(4, argv, 0);
    PySys_SetPath(path);

    pthread_mutex_lock(&state_lock);
    renpy_ident = PyThread_get_thread_ident();
    interpreter_up = true;
    pthread_mutex_unlock(&state_lock);

    /* runpy.run_path(boot, run_name="__main__"), unless a stop came before python_stop could reach Python (it raises
     * in Ren'Py's thread from here on) */
    PyObject *runpy = handoff_stopping() ? NULL : PyImport_ImportModule("runpy");
    PyObject *run_path = runpy ? PyObject_GetAttrString(runpy, "run_path") : NULL;
    PyObject *args = run_path ? Py_BuildValue("(s)", boot) : NULL;
    PyObject *kwargs = args ? Py_BuildValue("{s:s}", "run_name", "__main__") : NULL;
    PyObject *result = kwargs ? PyObject_Call(run_path, args, kwargs) : NULL;
    Py_XDECREF(kwargs);
    Py_XDECREF(args);
    Py_XDECREF(run_path);
    if (!result && PyErr_Occurred())
    {
        if (PyErr_ExceptionMatches(PyExc_SystemExit))
            PyErr_Clear();
        else
            PyErr_Print(); /* to sys.stderr: the log */
    }
    Py_XDECREF(result);
    Py_XDECREF(runpy);

    /* no other thread may be inside Python once it finalises */
    Py_BEGIN_ALLOW_THREADS
    pthread_mutex_lock(&state_lock);
    interpreter_up = false;
    while (stopper_inside)
        pthread_cond_wait(&state_changed, &state_lock);
    pthread_mutex_unlock(&state_lock);
    Py_END_ALLOW_THREADS
    PyThreadState_SetAsyncExc(renpy_ident, NULL); /* a stop that came as the boot script ended */
    Py_Finalize();

    input_detach();
    SDL_Quit();
    audio_close();
    handoff_engine_end();
    return NULL;
}

static double wall_now(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return (double)t.tv_sec + (double)t.tv_usec / 1e6;
}

bool python_start(const char *engine_dir, const char *game_dir, const char *saves_dir, char *error, size_t size)
{
    if (ran_a_game)
    {
        snprintf(error, size, "Ren'Py runs one game each time Dulcamara starts: Dulcamara starts again to run another.");
        return false;
    }
    snprintf(engine, sizeof(engine), "%s", engine_dir);
    snprintf(game, sizeof(game), "%s", game_dir);
    snprintf(saves, sizeof(saves), "%s", saves_dir);
    handoff_reset();
    frames_reset();
    hooks_reset();
    audio_close();
    wall_start = wall_now();
    SDL_SetHint("SDL_VIDEODRIVER", "dummy");
    SDL_SetHint("SDL_AUDIODRIVER", "dummy");
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 16u << 20); /* Python and Ren'Py recurse deeply */
    const int failed = pthread_create(&thread, &attributes, renpy_main, NULL);
    pthread_attr_destroy(&attributes);
    if (failed)
    {
        snprintf(error, size, "Ren'Py's thread could not start (%d).", failed);
        return false;
    }
    thread_live = ran_a_game = true;
    return true;
}

/* Logs where Ren'Py's thread is in its Python (on the stopping thread, which holds the GIL): for a stop that has not
 * come, the log then says what held it. */
static void log_where_renpy_is(void)
{
    char code[640];
    snprintf(code, sizeof(code),
             "import sys, traceback, _dulcamara\n"
             "frame = sys._current_frames().get(%ld)\n"
             "if frame is not None:\n"
             "    _dulcamara.log(2, \"Ren'Py has not stopped; its thread is at:\")\n"
             "    for line in ''.join(traceback.format_stack(frame)).splitlines():\n"
             "        _dulcamara.log(2, line)\n",
             renpy_ident);
    PyObject *globals = PyDict_New();
    if (globals && PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) == 0)
    {
        PyObject *done = PyRun_String(code, Py_file_input, globals, globals);
        Py_XDECREF(done);
    }
    if (PyErr_Occurred())
        PyErr_Clear();
    Py_XDECREF(globals);
}

/* Raises renpy.game.QuitException in Ren'Py's thread (SystemExit before Ren'Py is imported): Ren'Py then quits the
 * way its own Quit does. */
static void ask_renpy_to_quit(bool where)
{
    pthread_mutex_lock(&state_lock);
    const bool can = interpreter_up;
    if (can)
        stopper_inside = true;
    pthread_mutex_unlock(&state_lock);
    if (!can)
        return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *renpy_game = PyDict_GetItemString(PyImport_GetModuleDict(), "renpy.game"); /* only if imported */
    PyObject *quit = renpy_game ? PyObject_GetAttrString(renpy_game, "QuitException") : NULL;
    if (!quit)
    {
        PyErr_Clear();
        Py_INCREF(PyExc_SystemExit);
        quit = PyExc_SystemExit;
    }
    if (where)
        log_where_renpy_is();
    PyThreadState_SetAsyncExc(renpy_ident, quit);
    Py_DECREF(quit);
    PyGILState_Release(gil);
    pthread_mutex_lock(&state_lock);
    stopper_inside = false;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_lock);
}

void python_stop(void)
{
    if (!thread_live)
        return;
    handoff_stop();
    ask_renpy_to_quit(false);
    for (int i = 0; i < 1000 && !handoff_engine_ended(); ++i)
    {
        /* three seconds on: where Ren'Py is, and the quit once more, in case a handler of Ren'Py's caught the first */
        if (i == 300)
            ask_renpy_to_quit(true);
        usleep(10000);
    }
    if (handoff_engine_ended())
    {
        pthread_join(thread, NULL);
        thread_live = false;
    }
    else
        log_queue(2, "renpy: Ren'Py did not stop within ten seconds");
}
