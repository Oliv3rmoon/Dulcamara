/*
 * Dulcamara - Ren'Py's core: Ren'Py's own thread, with the Python it runs in. CPython is linked in (its standard
 * library is the engine folder's lib/python312.zip), Ren'Py's compiled modules are built in (inittab.c, written by
 * tools/build-renpy.sh), and _dulcamara gives the boot script (boot.py, the engine's dulcamara_boot.py) the core's
 * clock and log. Python is started on this thread (so it is Python's main one) and finalised when the game ends, which
 * frees its memory. It runs one game each time the app starts: Ren'Py's compiled modules keep their module objects
 * in C statics, which a second interpreter would find dead, so a second game is refused and the app starts itself
 * again for it. Python handles no signals: those are the frontend's.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "python.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

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
static unsigned long renpy_ident;

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
    {"monotonic", py_monotonic, METH_NOARGS, "time.monotonic() by the core's clock."},
    {"sleep", py_sleep, METH_O, "time.sleep() on Ren'Py's thread, by the core's clock."},
    {"log", py_log, METH_VARARGS, "log(level, text): a line of Dulcamara's log."},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "_dulcamara", NULL, -1, methods,
                                    NULL, NULL, NULL, NULL};

static PyObject *init_dulcamara(void)
{
    return PyModule_Create(&module);
}

/* ---------------------------------------------------------------- Ren'Py's thread */

static bool ok(PyStatus status, const char *what)
{
    if (!PyStatus_Exception(status))
        return true;
    log_queue(2, "renpy: Python could not %s: %s", what, status.err_msg ? status.err_msg : "no reason given");
    return false;
}

static void search_path(PyConfig *config, const char *path)
{
    wchar_t *wide = Py_DecodeLocale(path, NULL);
    if (wide)
    {
        PyWideStringList_Append(&config->module_search_paths, wide);
        PyMem_RawFree(wide);
    }
}

static bool start_interpreter(const char *boot)
{
    PyPreConfig pre;
    PyPreConfig_InitIsolatedConfig(&pre);
    pre.utf8_mode = 1; /* file names are UTF-8, as the console's are */
    if (!ok(Py_PreInitialize(&pre), "start"))
        return false;
    PyImport_AppendInittab("_dulcamara", init_dulcamara);
    renpy_extend_inittab();

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.install_signal_handlers = 0;
    config.write_bytecode = 0;
    config.site_import = 0;
    config.use_hash_seed = 1; /* no random seed needed to start */
    config.hash_seed = 0;
    config.buffered_stdio = 0;
    config.configure_c_stdio = 0;
    config.parse_argv = 0;
    config.pathconfig_warnings = 0;
    config.module_search_paths_set = 1;
    char path[1100];
    snprintf(path, sizeof(path), "%s/lib/python312.zip", engine);
    search_path(&config, path);
    snprintf(path, sizeof(path), "%s/lib/site-packages", engine);
    search_path(&config, path);
    search_path(&config, engine);
    char *argv[] = {(char *)boot, engine, game, saves};
    const bool up = ok(PyConfig_SetBytesString(&config, &config.home, engine), "set its home") &&
                    ok(PyConfig_SetBytesString(&config, &config.program_name, "renpy"), "set its name") &&
                    ok(PyConfig_SetBytesArgv(&config, 4, argv), "set its arguments") &&
                    ok(Py_InitializeFromConfig(&config), "start");
    PyConfig_Clear(&config);
    return up;
}

static void *renpy_main(void *unused)
{
    (void)unused;
    handoff_engine_begin();
    char boot[1100];
    snprintf(boot, sizeof(boot), "%s/dulcamara_boot.py", engine);
    if (start_interpreter(boot))
    {
        pthread_mutex_lock(&state_lock);
        renpy_ident = PyThread_get_thread_ident();
        interpreter_up = true;
        pthread_mutex_unlock(&state_lock);

        /* runpy.run_path(boot, run_name="__main__"), unless a stop came before python_stop could reach Python (it
         * raises in Ren'Py's thread from here on) */
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
        /* The inittab is reset as Python finalises: the next game's start_interpreter extends it again. */
        if (Py_FinalizeEx() < 0)
            log_queue(2, "renpy: Python reported an error as it ended");
    }
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
    if (thread_live)
    {
        if (!handoff_engine_ended())
        {
            snprintf(error, size, "Ren'Py is still closing the last game.");
            return false;
        }
        pthread_join(thread, NULL);
        thread_live = false;
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
             "frame = sys._current_frames().get(%lu)\n"
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

/* Raises renpy.game.QuitException in Ren'Py's thread (SystemExit before Ren'Py is imported): Ren'Py then quits
 * the way its own Quit does, closing its sound and display. */
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
    PyObject *name = PyUnicode_FromString("renpy.game");
    PyObject *renpy_game = name ? PyImport_GetModule(name) : NULL; /* only if Ren'Py has imported it */
    Py_XDECREF(name);
    PyObject *quit = renpy_game ? PyObject_GetAttrString(renpy_game, "QuitException") : NULL;
    if (!quit)
    {
        PyErr_Clear();
        quit = Py_NewRef(PyExc_SystemExit);
    }
    if (where)
        log_where_renpy_is();
    PyThreadState_SetAsyncExc(renpy_ident, quit);
    Py_DECREF(quit);
    Py_XDECREF(renpy_game);
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
