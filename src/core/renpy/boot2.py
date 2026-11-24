# -*- coding: utf-8 -*-
# Dulcamara - starts Ren'Py 7 inside Dulcamara's core: boot.py's work, in Python 2.7. src/core/renpy/python2.c runs
# this file, shipped as the Ren'Py 7 engine's dulcamara_boot.py, as __main__ on Ren'Py's own thread, with sys.argv =
# [this file, the engine folder, the game folder, the saves folder].
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ren'Py keeps the core's time (time.time reads the clock the frontend moves, and time.sleep on Ren'Py's thread waits
by it), writes to Dulcamara's log, has its errors logged in full (dulcamara_errors) and its saves go where the core
says. First, what the site.py of Ren'Py 7's own Python does (renpy-build's runtime/site2.py), as there is no site
module here: UTF-8 as the default and preferred encoding, the built-in modules with dotted names importable, and
queue as Queue."""

import imp
import locale
import os
import platform
import runpy
import sys
import thread
import time

import Queue

if sys.platform.startswith("freebsd"):
    sys.platform = "linux2"
    os.environ.setdefault("RENPY_PLATFORM", "linux-x86_64")
    _real_uname = platform.uname
    platform.system = lambda: "Linux"
    platform.platform = lambda aliased=0, terse=0: "Linux-x86_64" if terse else "Linux-6.0-x86_64"
    platform.uname = lambda: ("Linux",) + _real_uname()[1:]
    try:
        import errno as _errno
        import threading as _threading

        _real_sleep2 = time.sleep
        _real_time2 = time.time
        _real_start2 = _threading.Thread.start
        _real_boot2 = _threading.Thread._Thread__bootstrap_inner
        _burst_sem2 = _threading.Semaphore(4)

        def _ps5_start2(self):
            started = getattr(self, "_Thread__started", None)
            if started is not None and started.is_set():
                return _real_start2(self)
            acquired = False
            if _threading.active_count() >= 8:
                deadline = _real_time2() + 15.0
                while True:
                    if _burst_sem2.acquire(False):
                        acquired = True
                        self._dulcamara_burst_held = True
                        break
                    if _real_time2() >= deadline:
                        break
                    _real_sleep2(0.05)
            try:
                for attempt in range(8):
                    try:
                        return _real_start2(self)
                    except (RuntimeError, OSError, thread.error) as exc:
                        msg = str(exc).lower()
                        is_limit = "start new thread" in msg or getattr(exc, "errno", None) in (_errno.EAGAIN, _errno.ENOMEM)
                        if is_limit and attempt < 7:
                            _real_sleep2(0.05 * (attempt + 1))
                            continue
                        raise
            except BaseException:
                if acquired and getattr(self, "_dulcamara_burst_held", False):
                    self._dulcamara_burst_held = False
                    _burst_sem2.release()
                raise

        def _ps5_boot2(self):
            try:
                return _real_boot2(self)
            finally:
                if getattr(self, "_dulcamara_burst_held", False):
                    self._dulcamara_burst_held = False
                    _burst_sem2.release()

        _threading.Thread.start = _ps5_start2
        _threading.Thread._Thread__bootstrap_inner = _ps5_boot2
    except Exception:
        pass

sys.setdefaultencoding("utf-8")  # still there: site.py, which deletes it, did not run
locale.getpreferredencoding = lambda do_setlocale=True: "utf-8"


class BuiltinSubmodules(object):
    """Ren'Py's and pygame_sdl2's compiled modules are built in (inittab.c) under their dotted names, which Python 2
    looks for among the built-in modules only for top-level names."""

    def find_module(self, name, path=None):
        if path is not None and "." in name and name in sys.builtin_module_names:
            return self
        return None

    def load_module(self, name):
        if name in sys.modules:
            return sys.modules[name]
        return imp.load_module(name, *imp.find_module(name, None))


sys.meta_path.append(BuiltinSubmodules())
sys.modules["queue"] = Queue
# Ren'Py imports site (for what renpy-build's adds): this file is that site, and the standard library's would walk
# every module, which pygame_sdl2's stand-ins for the modules it is built without refuse.
site = imp.new_module("site")
site.__doc__ = "Dulcamara's: dulcamara_boot.py did its work."
sys.modules["site"] = site

import _dulcamara
import dulcamara_errors

engine, game, saves = sys.argv[1:4]
_dulcamara.step("the boot script's modules imported")


class Log(object):
    """sys.stdout (level 1) and sys.stderr (level 2): each line to Dulcamara's log, in UTF-8."""

    encoding = "utf-8"

    def __init__(self, level):
        self.level = level
        self.pending = u""

    def write(self, text):
        if isinstance(text, str):
            text = text.decode("utf-8", "replace")
        self.pending += text
        lines = self.pending.split(u"\n")
        self.pending = lines.pop()
        for line in lines:
            _dulcamara.log(self.level, line.encode("utf-8"))

    def flush(self):
        if self.pending:
            _dulcamara.log(self.level, self.pending.encode("utf-8"))
            self.pending = u""

    def isatty(self):
        return False


sys.stdout = Log(1)
sys.stderr = Log(2)

renpy_thread = thread.get_ident()
real_sleep = time.sleep


def sleep(seconds):
    if thread.get_ident() == renpy_thread:
        _dulcamara.sleep(seconds)
    else:
        real_sleep(seconds)


time.time = _dulcamara.time
time.clock = _dulcamara.monotonic
time.sleep = sleep

dulcamara_errors.install(game)
os.environ["RENPY_PATH_TO_SAVES"] = saves
# MultiPersistent data goes to ~/.renpy: with HOME the saves folder, it stays with the saves.
os.environ["HOME"] = saves
# Ren'Py copies each script it has just compiled into the saves folder, a developer's backup: on the console it only
# costs a game's start time and storage
os.environ["RENPY_DISABLE_BACKUPS"] = "I take responsibility for this."
# gl2, the renderer that draws into the framebuffer bound when it starts (the core's), as Ren'Py 8's only renderer
# does: never Ren'Py 7's older gl renderer, which saved preferences may name.
os.environ["RENPY_RENDERER"] = "gl2"
# DULCAMARA_RENPY_COMMAND: a command of Ren'Py's own instead of the game ("compile", run by tools/build-renpy7.sh)
sys.argv = [os.path.join(engine, "renpy.py"), game] + os.environ.get("DULCAMARA_RENPY_COMMAND", "").split()
_dulcamara.step("Ren'Py starting")
status = 0
try:
    runpy.run_path(sys.argv[0], run_name="__main__")
except SystemExit as e:
    status = e.code if isinstance(e.code, int) else 0
except BaseException:
    import traceback

    dulcamara_errors.log_error(traceback.format_exc())
    status = 1
finally:
    sys.stdout.flush()
    sys.stderr.flush()
_dulcamara.log(1, "Ren'Py ended (%d)" % status)
