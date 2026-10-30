# -*- coding: utf-8 -*-
# Dulcamara - errors.py's work for Ren'Py 7, in Python 2.7: renpy.error.report_exception (which writes traceback.txt)
# and renpy.display.error.report_parse_errors are wrapped as their modules load, so that Ren'Py's errors are logged in
# full before Ren'Py shows them. Shipped as the Ren'Py 7 engine's dulcamara_errors.py; a module of its own, since Ren'Py
# pickles its modules' globals and must find the wrappers by name.
# SPDX-License-Identifier: GPL-3.0-or-later

import sys

import _dulcamara

originals = {}


def log_error(text):
    if isinstance(text, unicode):
        text = text.encode("utf-8")
    _dulcamara.log(2, "Ren'Py reported an error:")
    for line in text.splitlines():
        _dulcamara.log(2, line)


def report_exception(e, *args, **kwargs):
    result = originals["report_exception"](e, *args, **kwargs)
    try:
        with open(result[2], "rb") as f:  # (simple, full, traceback file)
            log_error(f.read())
    except Exception:
        log_error(repr(e))
    return result


def report_parse_errors(errors, error_fn, *args, **kwargs):
    log_error(u"\n".join(e if isinstance(e, unicode) else e.decode("utf-8", "replace") for e in errors))
    return originals["report_parse_errors"](errors, error_fn, *args, **kwargs)


def wrap(module):
    if module.__name__ == "renpy.error" and "report_exception" not in originals:
        originals["report_exception"] = module.report_exception
        module.report_exception = report_exception
    elif module.__name__ == "renpy.display.error" and "report_parse_errors" not in originals:
        originals["report_parse_errors"] = module.report_parse_errors
        module.report_parse_errors = report_parse_errors


class Finder(object):
    """Python 2's import hook: the two modules are imported as they would be, then wrapped."""

    names = ("renpy.error", "renpy.display.error")
    busy = False

    def find_module(self, name, path=None):
        if name in self.names and not self.busy:
            return self
        return None

    def load_module(self, name):
        Finder.busy = True
        try:
            __import__(name)
        finally:
            Finder.busy = False
        module = sys.modules[name]
        originals.pop("report_exception" if name == "renpy.error" else "report_parse_errors", None)
        wrap(module)
        return module


def install():
    sys.meta_path.insert(0, Finder())
