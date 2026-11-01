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


# A sound in one of a game's archives (.rpa) is read here, before Ren'Py's sound has it: Ren'Py would hand its decode
# thread the archive opened through SDL's own file reading, and 0.2.6 and 0.2.7 ended without a word as the first
# archived sound started (a game's hover sound), and played on once the archive was gone. Up to SOUND_IN_MEMORY bytes
# it goes to Ren'Py as a buffer; a larger one (a movie) is read through Python's own file as Ren'Py plays it.
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
    if not isinstance(path, str):
        return None, "from a %s" % type(raw).__name__
    if base is None or length is None:
        return None, ("from the file %s" if os.path.isfile(path) else "from %s") % path
    how = "read into memory" if length <= SOUND_IN_MEMORY else "read through Python"
    return (path, base, length), "from %s at %d (%d bytes), %s" % (os.path.basename(path), base, length, how)


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
        _dulcamara.log(2, "renpy: could not read the sound from %s, so Ren'Py reads it: %s" % (path, describe(e)))
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
        parts.append("Python's blocks %d" % sys.getallocatedblocks())
    except Exception as e:
        parts.append(describe(e))
    _dulcamara.now("memory: " + ", ".join(parts))


# Ren'Py's presplash, drawn into a window of its own while the game loads, which the console never shows: each time
# Ren'Py draws it (as it starts, then as its progress bar moves), the window's picture, centred on black as it sits
# on a computer's screen, is the core's picture until the game's own first one.
shown_pump = [None]


def show_presplash(presplash):
    window = getattr(presplash, "window", None)
    pygame = getattr(presplash, "pygame", None) or getattr(presplash, "pygame_sdl2", None)
    if window is None or pygame is None:
        return
    try:
        surface = window.get_surface()
        w, h = surface.get_size()
        cw, ch = max(1280, w), max(720, h)
        canvas = pygame.Surface((cw, ch), 0, 32, (0xFF0000, 0xFF00, 0xFF, 0))
        canvas.fill((0, 0, 0))
        canvas.blit(surface, ((cw - w) // 2, (ch - h) // 2))
        shown = _dulcamara.picture(canvas._pixels_address, cw, ch, canvas.get_pitch())
        if shown_pump[0] is None:
            _dulcamara.log(1, "renpy: the presplash, %dx%d, is the picture (%dx%d)%s" % (
                w, h, cw, ch, "" if shown else ", but it could not be shown"))
    except Exception as e:
        if shown_pump[0] is None:
            _dulcamara.log(2, "renpy: the presplash could not be shown: %s" % describe(e))
    shown_pump[0] = getattr(presplash, "last_pump_time", 0)


def presplash_start(basedir, gamedir):
    rv = originals["presplash_start"](basedir, gamedir)
    show_presplash(sys.modules["renpy.display.presplash"])
    return rv


def presplash_pump():
    rv = originals["presplash_pump"]()
    presplash = sys.modules["renpy.display.presplash"]
    # the progress bar is drawn again at most 24 times a second, each time noted in last_pump_time
    if getattr(presplash, "progress_bar", None) is not None and \
            getattr(presplash, "last_pump_time", None) != shown_pump[0]:
        show_presplash(presplash)
    return rv


last_focus = [None]


def change_focus(newfocus, default=False):
    # Ren'Py asks again at each interaction: only a new focus is noted
    text = "focus on %s" % describe(newfocus)
    if text != last_focus[0]:
        last_focus[0] = text
        crumb("focus", 100, text)
    return originals["change_focus"](newfocus, default)


sized_images = set()


def note_size(image, loaded):
    # each image's size once Ren'Py has it, the first 200: which pictures are 64x64 or larger, whose textures Ren'Py
    # fills with a copy on the GPU (gl.c), and how large a game's backgrounds are
    try:
        if loaded is None or len(sized_images) >= 200 or image.identity in sized_images:
            return
        w, h = loaded.get_size()
        sized_images.add(image.identity)
        _dulcamara.log(1, "image %s is %dx%d as Ren'Py loads it" % (
            describe(getattr(image, "filename", image)), w, h))
    except Exception:
        pass


def image_get(cache, image, *args, **kwargs):
    try:
        new = image.identity not in seen_images
        if new:
            seen_images.add(image.identity)
    except Exception:
        new = False
    if new:
        crumb("image", 200, "image %s" % describe(image))
    loaded = originals["image_get"](cache, image, *args, **kwargs)
    note_size(image, loaded)
    return loaded


def wrap(module):
    if module.__name__ == "renpy.error":
        originals["report_exception"] = module.report_exception
        module.report_exception = report_exception
    elif module.__name__ == "renpy.config":
        # no mipmaps unless a game asks for them: ps5-opengl makes those of a picture whose sides are not powers of
        # two (nearly every one a visual novel has) on the CPU, pixel by pixel, while Ren'Py waits to draw; and the
        # two silent ends of 0.2.5 came right after it had
        module.mipmap = False
        module.periodic_callbacks.append(log_memory)
    elif module.__name__ == "renpy.audio.renpysound" and "sound_play" not in originals:
        originals["sound_play"] = module.play
        originals["sound_queue"] = module.queue
        module.play = sound_play
        module.queue = sound_queue
    elif module.__name__ == "renpy.display.focus" and "change_focus" not in originals:
        originals["change_focus"] = module.change_focus
        module.change_focus = change_focus
    elif module.__name__ == "renpy.display.presplash" and "presplash_start" not in originals:
        originals["presplash_start"] = module.start
        originals["presplash_pump"] = module.pump_window
        module.start = presplash_start
        module.pump_window = presplash_pump
    elif module.__name__ == "renpy.display.im" and "image_get" not in originals:
        originals["image_get"] = module.Cache.get
        module.Cache.get = image_get
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
             "renpy.config", "renpy.audio.renpysound", "renpy.display.focus", "renpy.display.im",
             "renpy.display.presplash")

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
