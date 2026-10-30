# Dulcamara - logs Ren'Py's errors in full before Ren'Py shows them: renpy.error.report_exception (which writes
# traceback.txt) and renpy.display.error.report_parse_errors (the script's errors) are wrapped as their modules load.
# Shipped as the engine's dulcamara_errors.py and installed by the boot script (boot.py). It is a module of its own
# because Ren'Py pickles its modules' globals as a backup: the wrappers must be functions it can find by name.
# SPDX-License-Identifier: GPL-3.0-or-later

import importlib.abc
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


def wrap(module):
    if module.__name__ == "renpy.error":
        originals["report_exception"] = module.report_exception
        module.report_exception = report_exception
    elif module.__name__ == "renpy.display.error":
        originals["report_parse_errors"] = module.report_parse_errors
        module.report_parse_errors = report_parse_errors


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

    names = ("renpy.error", "renpy.display.error")

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
