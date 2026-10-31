# Dulcamara - logs Ren'Py's errors in full before Ren'Py shows them: renpy.error.report_exception (which writes
# traceback.txt) and renpy.display.error.report_parse_errors (the script's errors) are wrapped as their modules load.
# With Settings > Ren'Py's "skip unread text too" (DULCAMARA_SKIP_UNSEEN=1), Ren'Py's skip_unseen preference reads
# as on, while the game's own value is kept for when it is off again.
# Shipped as the engine's dulcamara_errors.py and installed by the boot script (boot.py). It is a module of its own
# because Ren'Py pickles its modules' globals as a backup: the wrappers must be functions it can find by name.
# SPDX-License-Identifier: GPL-3.0-or-later

import importlib.abc
import os
import sys

import _dulcamara

originals = {}


def log_error(text):
    _dulcamara.log(2, "Ren'Py reported an error:")
    for line in text.splitlines():
        _dulcamara.log(2, line)


def report_exception(e, editor=True):
    # never the editor Ren'Py would open traceback.txt in (xdg-open, a process of its own): there is none on the
    # console, and the core's fork() refuses
    te = originals["report_exception"](e, editor=False)
    try:
        with open(te.traceback_fn, encoding="utf-8", errors="replace") as f:
            log_error(f.read())
    except Exception:
        log_error(repr(e))
    return te


def report_parse_errors(errors, error_fn, *args, **kwargs):
    log_error("\n".join(errors))
    return originals["report_parse_errors"](errors, error_fn, *args, **kwargs)


def skip_unseen_get(preferences):
    return True


def skip_unseen_set(preferences, value):
    preferences.__dict__["skip_unseen"] = value


def quit_event(interface):
    # the frontend closing the game: quit, without the "are you sure" Ren'Py asks a player
    if _dulcamara.stopping():
        sys.modules["renpy.exports"].quit(save=True)
    return originals["quit_event"](interface)


log_lines = [0]


def log_write(log, s, *args):
    # Ren'Py's own log.txt (its start's stages and how long each took, its warnings) to Dulcamara's log too, its
    # first 200 lines
    originals["log_write"](log, s, *args)
    if getattr(log, "name", None) != "log" or getattr(log, "raw_write", False) or log_lines[0] >= 200:
        return
    try:
        text = s % args if args else s
    except Exception:
        text = repr((s,) + args)
    for line in str(text).splitlines():
        if line.strip():
            log_lines[0] += 1
            _dulcamara.log(1, "renpy log: " + line)


def wrap(module):
    if module.__name__ == "renpy.error":
        originals["report_exception"] = module.report_exception
        module.report_exception = report_exception
    elif module.__name__ == "renpy.config":
        # no mipmaps unless a game asks for them: ps5-opengl makes those of a picture whose sides are not powers of
        # two (nearly every one a visual novel has) on the CPU, pixel by pixel, while Ren'Py waits to draw; and the
        # two silent ends of 0.2.5 came right after it had
        module.mipmap = False
    elif module.__name__ == "renpy.log" and "log_write" not in originals:
        originals["log_write"] = module.LogFile.write
        module.LogFile.write = log_write
    elif module.__name__ == "renpy.display.error":
        originals["report_parse_errors"] = module.report_parse_errors
        module.report_parse_errors = report_parse_errors
    elif module.__name__ == "renpy.display.core" and "quit_event" not in originals:
        originals["quit_event"] = module.Interface.quit_event
        module.Interface.quit_event = quit_event
    elif module.__name__ == "renpy.preferences" and os.environ.get("DULCAMARA_SKIP_UNSEEN") == "1":
        module.Preferences.skip_unseen = property(skip_unseen_get, skip_unseen_set)


class Loader(importlib.abc.Loader):
    """The module's own loader, then the wrapping."""

    def __init__(self, loader):
        self.loader = loader

    def create_module(self, spec):
        return self.loader.create_module(spec)

    def exec_module(self, module):
        self.loader.exec_module(module)
        wrap(module)


class Finder(importlib.abc.MetaPathFinder):
    """Finds the two modules as the other finders do, with Loader around their loader. Ren'Py imports them itself,
    and again when it reloads."""

    names = ("renpy.error", "renpy.display.error", "renpy.display.core", "renpy.preferences", "renpy.log",
             "renpy.config")

    def find_spec(self, name, path, target=None):
        if name not in self.names:
            return None
        for finder in sys.meta_path:
            if finder is self or not hasattr(finder, "find_spec"):
                continue
            spec = finder.find_spec(name, path, target)
            if spec is not None and spec.loader is not None:
                spec.loader = Loader(spec.loader)
                return spec
        return None


def install():
    sys.meta_path.insert(0, Finder())
