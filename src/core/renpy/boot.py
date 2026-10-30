# Dulcamara - starts Ren'Py 8 inside Dulcamara's core. src/core/renpy/python.c runs this file, shipped as the
# engine's dulcamara_boot.py, as __main__ on Ren'Py's own thread, with sys.argv = [this file, the engine folder, the
# game folder, the saves folder].
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ren'Py keeps the core's time: time.time and the other clocks read the clock the frontend moves, and time.sleep
on Ren'Py's thread waits by it, giving control back meanwhile. Its output goes to Dulcamara's log, its errors are
logged in full before it shows them, and its saves go where the core says."""

import importlib.abc
import io
import os
import runpy
import sys
import threading
import time

import _dulcamara

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


def log_error(text):
    _dulcamara.log(2, "Ren'Py reported an error:")
    for line in text.splitlines():
        _dulcamara.log(2, line)


def wrap_error_reports(module):
    """renpy.error.report_exception writes traceback.txt; renpy.display.error.report_parse_errors shows the script's
    errors. Each is logged in full, then Ren'Py goes on to show it as it would."""
    if module.__name__ == "renpy.error":
        report_exception = module.report_exception

        def logged_report_exception(e, *args, **kwargs):
            te = report_exception(e, *args, **kwargs)
            try:
                with open(te.traceback_fn, encoding="utf-8", errors="replace") as f:
                    log_error(f.read())
            except Exception:
                log_error(repr(e))
            return te

        module.report_exception = logged_report_exception
    elif module.__name__ == "renpy.display.error":
        report_parse_errors = module.report_parse_errors

        def logged_report_parse_errors(errors, error_fn, *args, **kwargs):
            log_error("\n".join(errors))
            return report_parse_errors(errors, error_fn, *args, **kwargs)

        module.report_parse_errors = logged_report_parse_errors


class ErrorReports(importlib.abc.MetaPathFinder):
    """Wraps the error reports as their modules load: Ren'Py imports them itself, and again when it reloads."""

    names = ("renpy.error", "renpy.display.error")

    def find_spec(self, name, path, target=None):
        if name not in self.names:
            return None
        for finder in sys.meta_path:
            if finder is self or not hasattr(finder, "find_spec"):
                continue
            spec = finder.find_spec(name, path, target)
            if spec is not None and spec.loader is not None:
                exec_module = spec.loader.exec_module

                def exec_and_wrap(module, exec_module=exec_module):
                    exec_module(module)
                    wrap_error_reports(module)

                spec.loader.exec_module = exec_and_wrap
                return spec
        return None


sys.meta_path.insert(0, ErrorReports())
os.environ["RENPY_PATH_TO_SAVES"] = saves
# MultiPersistent data goes to ~/.renpy (or ~/Library/RenPy on macOS): with HOME the saves folder, it stays with
# the saves, and the host check never writes into the user's own home.
os.environ["HOME"] = saves
sys.argv = [os.path.join(engine, "renpy.py"), game]
status = 0
try:
    runpy.run_path(sys.argv[0], run_name="__main__")
except SystemExit as e:
    status = e.code if isinstance(e.code, int) else 0
except BaseException:
    import traceback

    log_error(traceback.format_exc())
    status = 1
finally:
    sys.stdout.flush()
    sys.stderr.flush()
_dulcamara.log(1, f"Ren'Py ended ({status})")
