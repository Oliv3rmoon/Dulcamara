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
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
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

/* The game's clock on Ren'Py's thread; the real one on the others (a game's own threads, a download): their timeouts
 * then pass while the game is paused or being closed, and nothing on them is compared with Ren'Py's own times. */
static double wall_now(void);

static PyObject *py_time(PyObject *self, PyObject *unused)
{
    (void)self, (void)unused;
    if (!handoff_on_engine_thread())
        return PyFloat_FromDouble(wall_now());
    return PyFloat_FromDouble(wall_start + (double)handoff_now_usec() / 1e6);
}

static PyObject *py_monotonic(PyObject *self, PyObject *unused)
{
    (void)self, (void)unused;
    if (!handoff_on_engine_thread())
    {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return PyFloat_FromDouble((double)t.tv_sec + (double)t.tv_nsec / 1e9);
    }
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

static PyObject *py_stopping(PyObject *self, PyObject *unused)
{
    (void)self, (void)unused;
    return PyBool_FromLong(handoff_stopping());
}

static PyObject *py_step(PyObject *self, PyObject *args)
{
    (void)self;
    const char *text;
    if (!PyArg_ParseTuple(args, "s", &text))
        return NULL;
    log_step("%s", text);
    Py_RETURN_NONE;
}

/* a line written at once, as the steps are: what Ren'Py was doing when a crash leaves no report (dulcamara_errors) */
static PyObject *py_now(PyObject *self, PyObject *args)
{
    (void)self;
    const char *text;
    if (!PyArg_ParseTuple(args, "s", &text))
        return NULL;
    log_now(1, "renpy: %s", text);
    Py_RETURN_NONE;
}

#if defined(__PROSPERO__)
size_t ps5_heap_mapped_bytes(void); /* the app's heap, src/platform/ps5_heap.c */
int64_t ps5_memory_direct_bytes(void);
int64_t ps5_memory_direct_blocks(void);
#endif

/* (the app's heap, the direct memory the core's libraries hold, in how many allocations), in bytes; None off the
 * console: dulcamara_errors logs it every few minutes */
static PyObject *py_memory(PyObject *self, PyObject *unused)
{
    (void)self, (void)unused;
#if defined(__PROSPERO__)
    return Py_BuildValue("(LLL)", (long long)ps5_heap_mapped_bytes(), (long long)ps5_memory_direct_bytes(),
                         (long long)ps5_memory_direct_blocks());
#else
    Py_RETURN_NONE;
#endif
}

/* picture(address, w, h, pitch): the w x h XRGB8888 pixels at address (a pygame surface's, pitch bytes a row) are the
 * newest picture. Ren'Py's presplash, which it draws into a window of its own before OpenGL starts (dulcamara_errors) */
static PyObject *py_picture(PyObject *self, PyObject *args)
{
    (void)self;
    unsigned long long address;
    int w, h, pitch;
    if (!PyArg_ParseTuple(args, "Kiii", &address, &w, &h, &pitch))
        return NULL;
    uint32_t *out = w > 0 && h > 0 && pitch >= w * 4 && address ? frames_back(w, h) : NULL;
    if (!out)
        Py_RETURN_FALSE;
    for (int y = 0; y < h; ++y)
        memcpy(out + (size_t)y * (size_t)w, (const uint8_t *)(uintptr_t)address + (size_t)y * (size_t)pitch,
               (size_t)w * 4);
    frames_publish();
    Py_RETURN_TRUE;
}

static PyMethodDef methods[] = {
    {"time", py_time, METH_NOARGS, "time.time() by the core's clock."},
    {"monotonic", py_monotonic, METH_NOARGS, "time.time() since the game started, by the core's clock."},
    {"sleep", py_sleep, METH_O, "time.sleep() on Ren'Py's thread, by the core's clock."},
    {"stopping", py_stopping, METH_NOARGS, "Whether the frontend is closing the game: Ren'Py is to quit."},
    {"step", py_step, METH_VARARGS, "step(text): a step of starting Ren'Py, written to Dulcamara's log at once."},
    {"now", py_now, METH_VARARGS, "now(text): a line of Dulcamara's log, written at once."},
    {"log", py_log, METH_VARARGS, "log(level, text): a line of Dulcamara's log (text in UTF-8)."},
    {"memory", py_memory, METH_NOARGS, "The app's heap and the direct memory the core holds (PS5)."},
    {"picture", py_picture, METH_VARARGS, "picture(address, w, h, pitch): XRGB8888 pixels as the newest picture."},
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
    log_step("Ren'Py's thread running");
#if defined(__PROSPERO__)
    /* the game's folder as the working one, which the core keeps (ps5_libc.c): Python asks for it as it starts */
    if (chdir(game) == 0)
        log_step("the working folder is the game's");
    else
        log_step("the working folder stays /: the game's could not be made it (%s)", strerror(errno));
#endif
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
    log_step("Python starting");
    Py_InitializeEx(0);
    log_step("Python started");
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
    if (kwargs)
        log_step("the boot script running");
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
    /* a quit asked for as the boot script ended: raised and dropped here, not as Python finalises */
    if (Py_MakePendingCalls() < 0)
        PyErr_Clear();
    /* Python 2.7 finalising under a thread still running (a game's renpy.invoke_in_thread, say) can crash: then it is
     * left up, holding the GIL, so that thread waits for good. A process runs one game, so nothing else needs it. */
    PyObject *threading = PyDict_GetItemString(PyImport_GetModuleDict(), "threading"); /* only if imported */
    PyObject *count = threading ? PyObject_CallMethod(threading, "active_count", NULL) : NULL;
    const long threads = count ? PyInt_AsLong(count) : 1;
    Py_XDECREF(count);
    PyErr_Clear();
    if (threads <= 1)
        Py_Finalize();
    else
        log_queue(1, "renpy: %ld of the game's threads still running: Python is left as it is", threads - 1);

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
    /* SDL drops a controller's presses while it has windows and none of them has the keyboard's focus; Ren'Py's are
     * hidden ones, on SDL's dummy driver */
    SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
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

/* Logs where Ren'Py's thread is in its Python: for a stop that has not come, the log then says what held it. On a
 * thread of its own (stopper_inside keeps Python up meanwhile), which may wait for the GIL as long as Ren'Py holds
 * it: the frontend's thread never waits on Ren'Py. */
static void *log_where_renpy_is(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&state_lock);
    const bool can = interpreter_up;
    if (can)
        stopper_inside = true;
    pthread_mutex_unlock(&state_lock);
    if (!can)
        return NULL;
    PyGILState_STATE gil = PyGILState_Ensure();
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
    PyGILState_Release(gil);
    pthread_mutex_lock(&state_lock);
    stopper_inside = false;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_lock);
    return NULL;
}

/* Run by Python on Ren'Py's thread, between two bytecodes (Py_AddPendingCall): renpy.game.QuitException raised
 * there (SystemExit before Ren'Py is imported), as Ren'Py's own Quit raises it. */
static int quit_on_renpy_thread(void *unused)
{
    (void)unused;
    PyObject *renpy_game = PyDict_GetItemString(PyImport_GetModuleDict(), "renpy.game"); /* only if imported */
    PyObject *quit = renpy_game ? PyObject_GetAttrString(renpy_game, "QuitException") : NULL;
    if (!quit)
    {
        PyErr_Clear();
        Py_INCREF(PyExc_SystemExit);
        quit = PyExc_SystemExit;
    }
    PyErr_SetNone(quit);
    Py_DECREF(quit);
    return -1;
}

/* Asks Python to raise the quit on Ren'Py's thread; without the GIL (Py_AddPendingCall needs none). With where, also
 * logs where Ren'Py is, from a thread of its own. */
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

void python_stop(void)
{
    if (!thread_live)
        return;
    /* Ren'Py quits at its next look for events (its waits now answer SDL_QUIT, and dulcamara_errors makes its
     * quit_event quit at once): between two of its statements, not inside one, as an exception raised in its thread
     * from here would be (inside Python 2's threading.RLock, it left the lock taken and Ren'Py 7 waiting on it for
     * good). Only a Ren'Py that does not look for events within a second is asked by that exception, and again after
     * three, with where it is, in case a handler of Ren'Py's caught the first. */
    handoff_stop();
    for (int i = 0; i < 1000 && !handoff_engine_ended(); ++i)
    {
        if (i == 100)
            ask_renpy_to_quit(false);
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

/* ---------------------------------------------------------------- the text Ren'Py wants typed */

/* An item of a sequence as a number; 0 for one that is none. */
static double number_at(PyObject *sequence, Py_ssize_t i)
{
    PyObject *item = PySequence_GetItem(sequence, i);
    double v = item ? PyFloat_AsDouble(item) : 0;
    Py_XDECREF(item);
    if (PyErr_Occurred())
        PyErr_Clear(), v = 0;
    return v;
}

static float fraction(double v)
{
    return v < 0 ? 0.0f : v > 1 ? 1.0f : (float)v;
}

/* As Ren'Py 8's (python.c): Ren'Py 7's interface keeps old_text_rect the same way, and pygame_sdl2's
 * start_text_input does not reach SDL either. */
bool python_text_field(float *top, float *bottom)
{
    *top = *bottom = 0;
    pthread_mutex_lock(&state_lock);
    const bool up = interpreter_up;
    pthread_mutex_unlock(&state_lock);
    if (!up || !handoff_on_engine_thread())
        return false;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);
    PyObject *modules = PyImport_GetModuleDict(); /* borrowed, as the two modules are */
    PyObject *display = PyDict_GetItemString(modules, "renpy.display");
    PyObject *config = PyDict_GetItemString(modules, "renpy.config");
    PyObject *interface = display ? PyObject_GetAttrString(display, "interface") : NULL;
    PyObject *rect = interface ? PyObject_GetAttrString(interface, "old_text_rect") : NULL;
    PyObject *height = config ? PyObject_GetAttrString(config, "screen_height") : NULL;
    PyErr_Clear(); /* an attribute not there yet: Ren'Py is still starting */
    const bool wanted = rect && rect != Py_None && PyObject_IsTrue(rect) == 1;
    const double screen_h = height ? PyFloat_AsDouble(height) : 0;
    PyErr_Clear();
    if (wanted && screen_h > 0 && PySequence_Check(rect) && PySequence_Size(rect) == 4)
    {
        const double y = number_at(rect, 1), h = number_at(rect, 3);
        *top = fraction(y / screen_h);
        *bottom = fraction((y + h) / screen_h);
    }
    PyErr_Clear();
    Py_XDECREF(height);
    Py_XDECREF(rect);
    Py_XDECREF(interface);
    PyErr_Restore(type, value, traceback);
    PyGILState_Release(gil);
    return wanted;
}

/* pygame_sdl2's event module, which Ren'Py has imported, or NULL: borrowed */
static PyObject *pygame_event(void)
{
    return PyDict_GetItemString(PyImport_GetModuleDict(), "pygame_sdl2.event");
}

/* As Ren'Py 8's (python.c): pygame_sdl2's own TEXTINPUT event, its text a unicode string as pygame_sdl2 makes it. */
bool python_type_text(const char *utf8)
{
    pthread_mutex_lock(&state_lock);
    const bool up = interpreter_up;
    pthread_mutex_unlock(&state_lock);
    if (!up || !handoff_on_engine_thread())
        return false;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);
    PyObject *module = pygame_event();
    PyObject *make = module ? PyObject_GetAttrString(module, "Event") : NULL;
    PyObject *post = module ? PyObject_GetAttrString(module, "post") : NULL;
    PyObject *text = PyUnicode_DecodeUTF8(utf8, (Py_ssize_t)strlen(utf8), "replace");
    PyObject *args = make && post && text ? Py_BuildValue("(I)", (unsigned)SDL_TEXTINPUT) : NULL;
    PyObject *kwargs = args ? Py_BuildValue("{s:O}", "text", text) : NULL;
    PyObject *event = kwargs ? PyObject_Call(make, args, kwargs) : NULL;
    PyObject *done = event ? PyObject_CallFunctionObjArgs(post, event, NULL) : NULL;
    const bool given = done != NULL;
    PyErr_Clear();
    Py_XDECREF(done);
    Py_XDECREF(event);
    Py_XDECREF(kwargs);
    Py_XDECREF(args);
    Py_XDECREF(text);
    Py_XDECREF(post);
    Py_XDECREF(make);
    PyErr_Restore(type, value, traceback);
    PyGILState_Release(gil);
    return given;
}
