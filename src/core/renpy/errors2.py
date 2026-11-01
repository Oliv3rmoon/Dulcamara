# -*- coding: utf-8 -*-
# Dulcamara - errors.py's work for Ren'Py 7, in Python 2.7 (with its skip_unseen preference): renpy.error.report_exception (which writes traceback.txt)
# and renpy.display.error.report_parse_errors are wrapped as their modules load, so that Ren'Py's errors are logged in
# full before Ren'Py shows them. Shipped as the Ren'Py 7 engine's dulcamara_errors.py; a module of its own, since Ren'Py
# pickles its modules' globals and must find the wrappers by name.
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import sys

import _dulcamara

originals = {}


def log_error(text):
    if isinstance(text, unicode):
        text = text.encode("utf-8")
    _dulcamara.log(2, "Ren'Py reported an error:")
    for line in text.splitlines():
        _dulcamara.log(2, line)


def report_exception(e, editor=True):
    # never the editor Ren'Py would open traceback.txt in (xdg-open, a process of its own): there is none on the
    # console, and the core's fork() refuses
    result = originals["report_exception"](e, False)
    try:
        with open(result[2], "rb") as f:  # (simple, full, traceback file)
            log_error(f.read())
    except Exception:
        log_error(repr(e))
    return result


def report_parse_errors(errors, error_fn, *args, **kwargs):
    log_error(u"\n".join(e if isinstance(e, unicode) else e.decode("utf-8", "replace") for e in errors))
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
    if not isinstance(text, unicode):
        text = text.decode("utf-8", "replace")
    for line in text.splitlines():
        if line.strip():
            log_lines[0] += 1
            _dulcamara.log(1, (u"renpy log: " + line).encode("utf-8"))


crumbs = {"sound": 0, "focus": 0, "image": 0}
seen_images = set()


def crumb(kind, limit, text):
    # what Ren'Py does as it does it, written at once (_dulcamara.now): a crash that leaves no report ends the log on
    # the last of these; the first `limit` of each kind
    if crumbs[kind] < limit:
        crumbs[kind] += 1
        _dulcamara.now(text[:300])


def describe(what):
    try:
        return repr(what)
    except Exception:
        return object.__repr__(what)


# A sound in one of a game's archives (.rpa) is read here, before Ren'Py's sound has it, as errors.py does for Ren'Py
# 8: Ren'Py would hand its decode thread the archive opened through SDL's own file reading, where Ren'Py 8 games ended
# without a word as their first archived sound started. Up to SOUND_IN_MEMORY bytes it goes to Ren'Py as a buffer; a
# larger one (a movie) is read through Python's own file as Ren'Py plays it.
SOUND_IN_MEMORY = int(os.environ.get("DULCAMARA_SOUND_IN_MEMORY", 32 << 20))  # the tests set it


class ArchivePart(object):
    """A file's part of an archive, through Python's own file. Not an io object, so that Ren'Py's sound reads it
    through Python (with the GIL, from its decode thread) instead of opening the archive again itself."""

    def __init__(self, path, base, length):
        self._file = open(path, "rb")
        self._base, self._length, self._at = base, length, 0

    def tell(self):
        return self._at

    def seek(self, offset, whence=0):
        start = (0, self._at, self._length)[whence]
        self._at = min(max(start + offset, 0), self._length)
        return self._at

    def read(self, size=-1):
        if size is None or size < 0 or size > self._length - self._at:
            size = self._length - self._at
        self._file.seek(self._base + self._at)
        data = self._file.read(size)
        self._at += len(data)
        return data


def sound_source(file):
    """Where a file Ren'Py's sound is to play comes from: (path, offset, length) for a part of an archive (renpy.loader
    gives it as an RWopsIO of the archive, in an io.BufferedReader), else None; and a word on it for the log."""
    raw = file
    try:
        while True:
            raw = raw.raw
    except AttributeError:
        pass
    path, base, length = (getattr(raw, a, None) for a in ("name", "base", "length"))
    if not isinstance(path, basestring):
        return None, "from a %s" % type(raw).__name__
    if isinstance(path, unicode):
        shown = path.encode("utf-8")
    else:
        shown = path
    if base is None or length is None:
        return None, ("from the file %s" if os.path.isfile(path) else "from %s") % shown
    how = "read into memory" if length <= SOUND_IN_MEMORY else "read through Python"
    return (path, base, length), "from %s at %d (%d bytes), %s" % (os.path.basename(shown), base, length, how)


def sound_file(file, part):
    """The file Ren'Py's sound gets for an archived one: its bytes, or an ArchivePart of a large one."""
    path, base, length = part
    try:
        if length <= SOUND_IN_MEMORY:
            with open(path, "rb") as f:
                f.seek(base)
                rv = sys.modules["renpy.loader"].RWopsIO.from_buffer(f.read(length), name=path)
        else:
            rv = ArchivePart(path, base, length)
    except Exception as e:
        _dulcamara.log(2, "renpy: could not read the sound from %s, so Ren'Py reads it: %s" % (describe(path),
                                                                                             describe(e)))
        return file
    file.close()
    return rv


last_sound = {}


def sound(action, channel, file, name):
    part, where = sound_source(file)
    # a sound queued again after itself is a loop: noted the first time only
    if action == "plays" or last_sound.get(channel) != name:
        crumb("sound", 100, "%s %s on channel %s, %s" % (action, describe(name), describe(channel), where))
    last_sound[channel] = name
    return sound_file(file, part) if part else file


def sound_play(channel, file, name, *args, **kwargs):
    file = sound("plays", channel, file, name)
    return originals["sound_play"](channel, file, name, *args, **kwargs)


def sound_queue(channel, file, name, *args, **kwargs):
    file = sound("queues", channel, file, name)
    return originals["sound_queue"](channel, file, name, *args, **kwargs)


# Every MEMORY_EVERY seconds (the core's clock), what the game holds: the app's heap and the direct memory ps5-opengl
# holds (on the console), Ren'Py's textures and image cache, Python's objects. The console went down after a long
# session; the next long log shows what grew.
MEMORY_EVERY = float(os.environ.get("DULCAMARA_MEMORY_EVERY", 180))  # the tests set it
memory_next = [0.0]


def log_memory():
    now = _dulcamara.monotonic()
    if now < memory_next[0]:
        return
    memory_next[0] = now + MEMORY_EVERY
    parts = []
    try:
        counts = _dulcamara.memory()
        if counts:
            parts.append("the heap %d MB, OpenGL's memory %d MB in %d blocks" % (counts[0] >> 20, counts[1] >> 20,
                                                                                 counts[2]))
        loader = getattr(sys.modules["renpy.display"].draw, "texture_loader", None)
        if loader is not None:
            size, count = loader.get_texture_size()
            parts.append("Ren'Py's textures %d (%d MB)" % (count, int(size) >> 20))
        cache = sys.modules["renpy.display.im"].cache
        parts.append("its image cache %d MB" % (cache.get_total_size() * 4 >> 20))
    except Exception as e:
        parts.append(describe(e))
    _dulcamara.now("memory: " + ", ".join(parts))


last_focus = [None]


def change_focus(newfocus, default=False):
    # Ren'Py asks again at each interaction: only a new focus is noted
    text = "focus on %s" % describe(newfocus)
    if text != last_focus[0]:
        last_focus[0] = text
        crumb("focus", 100, text)
    return originals["change_focus"](newfocus, default)


def image_get(cache, image, *args, **kwargs):
    try:
        new = image.identity not in seen_images
        if new:
            seen_images.add(image.identity)
    except Exception:
        new = False
    if new:
        crumb("image", 200, "image %s" % describe(image))
    return originals["image_get"](cache, image, *args, **kwargs)


def wrap(module):
    if module.__name__ == "renpy.error" and "report_exception" not in originals:
        originals["report_exception"] = module.report_exception
        module.report_exception = report_exception
    elif module.__name__ == "renpy.display.error" and "report_parse_errors" not in originals:
        originals["report_parse_errors"] = module.report_parse_errors
        module.report_parse_errors = report_parse_errors
    elif module.__name__ == "renpy.display.core" and "quit_event" not in originals:
        originals["quit_event"] = module.Interface.quit_event
        module.Interface.quit_event = quit_event
    elif module.__name__ == "renpy.audio.renpysound" and "sound_play" not in originals:
        originals["sound_play"] = module.play
        originals["sound_queue"] = module.queue
        module.play = sound_play
        module.queue = sound_queue
    elif module.__name__ == "renpy.display.focus" and "change_focus" not in originals:
        originals["change_focus"] = module.change_focus
        module.change_focus = change_focus
    elif module.__name__ == "renpy.display.im" and "image_get" not in originals:
        originals["image_get"] = module.Cache.get
        module.Cache.get = image_get
    elif module.__name__ == "renpy.log" and "log_write" not in originals:
        originals["log_write"] = module.LogFile.write
        module.LogFile.write = log_write
    elif module.__name__ == "renpy.preferences" and os.environ.get("DULCAMARA_SKIP_UNSEEN") == "1":
        module.Preferences.skip_unseen = property(skip_unseen_get, skip_unseen_set)
    elif module.__name__ == "renpy.config":
        module.periodic_callbacks.append(log_memory)


class Finder(object):
    """Python 2's import hook: the two modules are imported as they would be, then wrapped."""

    names = ("renpy.error", "renpy.display.error", "renpy.display.core", "renpy.preferences", "renpy.log",
             "renpy.audio.renpysound", "renpy.display.focus", "renpy.display.im", "renpy.config")
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
        originals.pop({"renpy.error": "report_exception", "renpy.display.error": "report_parse_errors",
                       "renpy.display.core": "quit_event", "renpy.log": "log_write"}.get(name), None)
        wrap(module)
        return module


def install():
    sys.meta_path.insert(0, Finder())
