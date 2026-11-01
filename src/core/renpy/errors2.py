# -*- coding: utf-8 -*-
# Dulcamara - errors.py's work for Ren'Py 7, in Python 2.7 (with its skip_unseen preference): renpy.error.report_exception (which writes traceback.txt)
# and renpy.display.error.report_parse_errors are wrapped as their modules load, so that Ren'Py's errors are logged in
# full before Ren'Py shows them; and Sounds for this game (DULCAMARA_SOUNDS) as errors.py has it. Shipped as the
# Ren'Py 7 engine's dulcamara_errors.py; a module of its own, since Ren'Py pickles its modules' globals and must find
# the wrappers by name.
# SPDX-License-Identifier: GPL-3.0-or-later

import io
import os
import re
import sys
import time

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


def fullscreen_get(preferences):
    return False


def fullscreen_set(preferences, value):
    # the game's own value, kept for a computer; noted the first time it turns on
    if value and not preferences.__dict__.get("fullscreen"):
        _dulcamara.log(1, "renpy: the game asked for fullscreen: its picture stays its own size")
    preferences.__dict__["fullscreen"] = value


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


# Sounds for this game (Dulcamara's menu), as errors.py does it for Ren'Py 8: DULCAMARA_SOUNDS is the path of the
# game's two files without their extension. Once a run, as the game first waits, <it>.list gets every sound the game
# can load, from its archives and its folder, in order, in UTF-8. A sound that <it>.txt has off is a second of silence
# from memory for Ren'Py's sound, and the file the game asked for is closed unread. The file is read again when it
# changes (looked at once a second at most).
SOUNDS = os.environ.get("DULCAMARA_SOUNDS", "")
SOUND_EXTENSIONS = (".ogg", ".opus", ".oga", ".mp3", ".wav", ".flac")
sounds_listed = [False]


def list_sounds():
    if sounds_listed[0] or not SOUNDS:
        return
    sounds_listed[0] = True
    listed = SOUNDS + ".list"
    try:
        names = sorted(set(name for _folder, name in sys.modules["renpy.loader"].listdirfiles(False)
                           if os.path.splitext(name)[1].lower() in SOUND_EXTENSIONS and
                           u"\n" not in name and u"\r" not in name))
        folder = os.path.dirname(listed)
        if folder and not os.path.isdir(folder):
            os.makedirs(folder)
        with io.open(listed + ".new", "w", encoding="utf-8", newline="\n") as f:
            f.write(u"".join(name + u"\n" for name in names))
        if os.path.exists(listed):
            os.remove(listed)
        os.rename(listed + ".new", listed)
        _dulcamara.log(1, "renpy: the game's %d sounds listed in %s (Sounds for this game)" % (len(names), listed))
    except Exception as e:
        _dulcamara.log(2, "renpy: could not list the game's sounds in %s: %s" % (listed, describe(e)))


# the real clock, kept before the boot script makes time's the core's: that one stands still while the game is paused,
# and Ren'Py's sound thread has another
real_clock = time.time
sounds_off = {"names": frozenset(), "file": None, "checked": None}


def off_sounds():
    """The sounds <SOUNDS>.txt has off: each line "off = <name>", the name as it is but for the line's ending (as
    config.c reads it). A missing file has none."""
    if not SOUNDS:
        return sounds_off["names"]
    now, checked = real_clock(), sounds_off["checked"]
    if checked is not None and checked <= now < checked + 1:
        return sounds_off["names"]
    sounds_off["checked"] = now
    path = SOUNDS + ".txt"
    try:
        st = os.stat(path)
        stamp = (st.st_mtime, st.st_size, st.st_ino)
    except OSError:
        stamp = None
    if stamp == sounds_off["file"]:
        return sounds_off["names"]
    sounds_off["file"] = stamp
    names = set()
    try:
        if stamp is not None:
            with io.open(path, encoding="utf-8", errors="replace", newline="") as f:
                for line in f.read().split(u"\n"):
                    key, equals, name = (line[:-1] if line.endswith(u"\r") else line).partition(u"=")
                    name = name[1:] if name.startswith(u" ") else name
                    if equals and name and key.strip(u" \t") == u"off":
                        names.add(name)
    except Exception as e:
        _dulcamara.log(2, "renpy: could not read the sounds off in %s: %s" % (path, describe(e)))
        return sounds_off["names"]
    if names != sounds_off["names"]:
        _dulcamara.log(1, "renpy: %d of the game's sounds off in %s (Sounds for this game)" % (len(names), path))
    sounds_off["names"] = frozenset(names)
    return sounds_off["names"]


def played_names(channel, name):
    """The names a sound given to Ren'Py's sound may have in the game's list: as given, without a playback spec
    (<loop 1.0>) and with / for \\; and as the channel and Ren'Py's loader find its file (in audio/, say, when the
    game leaves that out)."""
    m = re.match(u"<(.*)>(.*)", name)
    name = (m.group(2) if m else name).replace(u"\\", u"/")
    found = name
    try:
        # the channel by its _number: its number property would give a number to each channel the game has not used
        # yet, which Ren'Py's sound then makes room for as it mixes (renpysound_core.c's check_channel, unlocked)
        for c in sys.modules["renpy.audio.audio"].all_channels:
            if getattr(c, "_number", None) == channel:
                found = c.file_prefix + found + c.file_suffix
                break
        loader = sys.modules["renpy.loader"]
        found = re.sub(u"/+", u"/", found).lstrip(u"/")
        for prefix in loader.get_prefixes(directory="audio"):
            if loader.loadable_core(prefix + found):
                return name, prefix + found
    except Exception:
        pass
    return name, found


def sound_is_off(channel, name):
    """The game's list's name for the sound Ren'Py's sound is given, when it is off; else None."""
    off = off_sounds()
    if isinstance(name, str):
        name = name.decode("utf-8", "replace")
    if not off or not isinstance(name, unicode):
        return None
    for each in played_names(channel, name):
        if each in off:
            return each
    return None


silent_wav = []


def silence():
    """A second of silence for Ren'Py's sound, from memory, as sound_file gives a sound: a WAV at 48 kHz, 16-bit,
    stereo, made once."""
    if not silent_wav:
        import wave
        data = io.BytesIO()
        out = wave.open(data, "wb")
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(48000)
        out.writeframes(b"\0" * (48000 * 4))
        out.close()
        silent_wav.append(data.getvalue())
    return sys.modules["renpy.loader"].RWopsIO.from_buffer(silent_wav[0], name="silence.wav")


last_sound = {}
last_silenced = {}
silenced = [0]


def sound(action, channel, file, name, kwargs):
    off = sound_is_off(channel, name)
    if off is not None:
        try:
            rv = silence()
        except Exception as e:
            _dulcamara.log(2, "renpy: no silence for %s, so it plays: %s" % (describe(off), describe(e)))
            off = None
    if off is not None:
        # noted the first 50 times, a loop (silenced again after itself) only the first; from its start, which a
        # second of silence has (a loop's start further on would end it at once, and queue it again at once)
        if (action == "plays" or last_silenced.get(channel) != name) and silenced[0] < 50:
            silenced[0] += 1
            _dulcamara.log(1, (u"renpy: plays nothing for '%s' on channel %s: it is off (Sounds for this game)" % (
                off, describe(channel))).encode("utf-8"))
        last_sound[channel] = last_silenced[channel] = name
        if "start" in kwargs:
            kwargs["start"] = 0
        if hasattr(file, "close"):
            file.close()
        return rv
    last_silenced.pop(channel, None)
    part, where = sound_source(file)
    # a sound queued again after itself is a loop: noted the first time only
    if action == "plays" or last_sound.get(channel) != name:
        crumb("sound", 100, "%s %s on channel %s, %s" % (action, describe(name), describe(channel), where))
    last_sound[channel] = name
    return sound_file(file, part) if part else file


def sound_play(channel, file, name, *args, **kwargs):
    file = sound("plays", channel, file, name, kwargs)
    return originals["sound_play"](channel, file, name, *args, **kwargs)


def sound_queue(channel, file, name, *args, **kwargs):
    file = sound("queues", channel, file, name, kwargs)
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


# A movie with a mask of its own (Movie(play=..., mask=...)) is two streams, each decoded on its own thread. Ren'Py
# uploads a colour frame whose mask frame has not come yet as it is (get_movie_texture): such a movie's colour frames
# are black wherever its mask makes it clear, so a box's animated border flashed black each time the mask's frame came
# late on the console, and stayed black when the mask's stream did not open. Here a colour frame waits for its mask's
# (the movie keeps its last picture meanwhile), and is drawn with it when it comes.
movie_waiting = {}  # channel: its colour frame that waits for the mask's
movie_waits = [0]


def movie_texture(channel, mask_channel=None, side_mask=False, mipmap=None):
    import renpy

    video = sys.modules["renpy.display.video"]
    if not mask_channel or side_mask or renpy.emscripten:
        return originals["get_movie_texture"](channel, mask_channel, side_mask, mipmap)
    if not renpy.audio.music.get_playing(channel):
        movie_waiting.pop(channel, None)
        return None, False
    if mipmap is None:
        mipmap = renpy.config.mipmap_movies
    surf = renpy.audio.music.get_channel(channel).read_video()
    mask_surf = renpy.audio.music.get_channel(mask_channel).read_video()
    if surf is None and mask_surf is not None:
        surf = movie_waiting.pop(channel, None)
    elif surf is not None and mask_surf is None:
        movie_waiting[channel] = surf
        surf = None
        movie_waits[0] += 1
        if movie_waits[0] <= 20 or movie_waits[0] % 600 == 0:
            _dulcamara.log(1, "renpy: movie %s: a picture waits for its mask's (%d so far)" % (channel, movie_waits[0]))
    elif surf is not None:
        movie_waiting.pop(channel, None)
    if surf is not None and mask_surf is not None:
        renpy.display.module.alpha_munge(mask_surf, surf, renpy.display.im.identity)
        renpy.display.render.mutated_surface(surf)
        tex = renpy.display.draw.load_texture(surf, True, {"mipmap": mipmap})
        video.texture[channel] = tex
        return tex, True
    return video.texture.get(channel, None), False


def max_texture_size():
    """The size of Ren'Py's largest texture and its framebuffer to ask for: 2048 on the console unless
    DULCAMARA_MAX_TEXTURE_SIZE says otherwise, anywhere; 0 to leave Ren'Py's own."""
    setting = os.environ.get("DULCAMARA_MAX_TEXTURE_SIZE", "")
    if setting.isdigit() and int(setting) >= 1024:
        return int(setting)
    return 2048 if _dulcamara.memory() is not None else 0


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
    elif module.__name__ == "renpy.display.video":
        # each time Ren'Py loads it (again as it reloads): a movie's picture waits for its mask's
        originals["get_movie_texture"] = module.get_movie_texture
        module.get_movie_texture = movie_texture
    elif module.__name__ == "renpy.preferences":
        # Fullscreen (a game's Options > Display, or its key) reads as off: Ren'Py would make its window the
        # display's size (3840x2160 on the console, by SDL's account) and the picture with it, and the screen went
        # black. The console's picture is the game's own size, scaled by the app.
        module.Preferences.fullscreen = property(fullscreen_get, fullscreen_set)
        if os.environ.get("DULCAMARA_SKIP_UNSEEN") == "1":
            module.Preferences.skip_unseen = property(skip_unseen_get, skip_unseen_set)
    elif module.__name__ == "renpy.config":
        # Ren'Py's framebuffer, where it makes images ready and draws pictures into textures, is the size of its largest
        # texture (config.max_texture_size, 4096x4096: 64 MB, and as much again for its depth), cleared whole for each
        # picture it draws into a texture. On the console 2048x2048, or a larger game's own size and a border (Ren'Py's
        # own sizing): some 96 MB of graphics memory back, which a tester's game seemed to run out of.
        # (and fbo_size, which 00compat.rpy raises it to)
        size = max_texture_size()
        if size:
            module.max_texture_size = module.fbo_size = (size, size)
        module.periodic_callbacks.append(log_memory)
        module.periodic_callbacks.append(list_sounds)


class Finder(object):
    """Python 2's import hook: the two modules are imported as they would be, then wrapped."""

    names = ("renpy.error", "renpy.display.error", "renpy.display.core", "renpy.preferences", "renpy.log",
             "renpy.audio.renpysound", "renpy.display.focus", "renpy.display.im", "renpy.display.presplash",
             "renpy.config", "renpy.display.video")
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
