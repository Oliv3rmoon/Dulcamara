# Dulcamara - starts Ren'Py 8 inside Dulcamara's core. src/core/renpy/python.c runs this file, shipped as the
# engine's dulcamara_boot.py, as __main__ on Ren'Py's own thread, with sys.argv = [this file, the engine folder, the
# game folder, the saves folder].
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ren'Py keeps the core's time: time.time and the other clocks read the clock the frontend moves, and time.sleep
on Ren'Py's thread waits by it, giving control back meanwhile. Its output goes to Dulcamara's log, its errors are
logged in full before it shows them (dulcamara_errors), and its saves go where the core says."""

import io
import os
import runpy
import sys
import threading
import time

import _dulcamara
import dulcamara_errors

engine, game, saves = sys.argv[1:4]


class Log(io.TextIOBase):
    """sys.stdout (level 1) and sys.stderr (level 2): each line to Dulcamara's log."""

    def __init__(self, level):
        super().__init__()
        self.level = level
        self.pending = ""

    def write(self, text):
        self.pending += text
        *lines, self.pending = self.pending.split("\n")
        for line in lines:
            _dulcamara.log(self.level, line)
        return len(text)

    def flush(self):
        if self.pending:
            _dulcamara.log(self.level, self.pending)
            self.pending = ""

    def isatty(self):
        return False


sys.stdout = Log(1)
sys.stderr = Log(2)

renpy_thread = threading.get_ident()
real_sleep = time.sleep


def sleep(seconds):
    if threading.get_ident() == renpy_thread:
        _dulcamara.sleep(seconds)
    else:
        real_sleep(seconds)


time.time = _dulcamara.time
time.monotonic = _dulcamara.monotonic
time.perf_counter = _dulcamara.monotonic
time.sleep = sleep


dulcamara_errors.install()
os.environ["RENPY_PATH_TO_SAVES"] = saves
# MultiPersistent data goes to ~/.renpy (or ~/Library/RenPy on macOS): with HOME the saves folder, it stays with
# the saves, and the host check never writes into the user's own home.
os.environ["HOME"] = saves
# DULCAMARA_RENPY_COMMAND: a command of Ren'Py's own instead of the game ("compile", which tools/build-renpy.sh
# runs to compile Ren'Py's common scripts into the engine)
sys.argv = [os.path.join(engine, "renpy.py"), game] + os.environ.get("DULCAMARA_RENPY_COMMAND", "").split()
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
_dulcamara.log(1, f"Ren'Py ended ({status})")
