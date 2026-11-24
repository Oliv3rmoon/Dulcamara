# -*- coding: utf-8 -*-
# Dulcamara - errors.py's work for Ren'Py 7, in Python 2.7 (with its skip_unseen preference): renpy.error.report_exception (which writes traceback.txt)
# and renpy.display.error.report_parse_errors are wrapped as their modules load, so that Ren'Py's errors are logged in
# full before Ren'Py shows them; and Sounds for this game (DULCAMARA_SOUNDS) as errors.py has it. Shipped as the
# Ren'Py 7 engine's dulcamara_errors.py; a module of its own, since Ren'Py pickles its modules' globals and must find
# the wrappers by name.
# SPDX-License-Identifier: GPL-3.0-or-later

import io
import math
import os
import re
import sys
import threading
import time

import _dulcamara

originals = {}


def save_error(text):
    """Ren'Py's error as its error screen shows it, also in <logs>/renpy-errors.txt beside dulcamara.log (the newest
    last, begun again once past 1 MB), headed by the game's folder: a tester sends it and plays on past the screen."""
    folder = os.environ.get("DULCAMARA_LOGS")
    if not folder:
        return
    try:
        if not os.path.isdir(folder):
            os.makedirs(folder)
        path = os.path.join(folder, "renpy-errors.txt")
        mode = "ab" if os.path.exists(path) and os.path.getsize(path) < (1 << 20) else "wb"
        config = sys.modules.get("renpy.config")
        game = os.path.basename(os.path.normpath(config.basedir)) if getattr(config, "basedir", None) else "?"
        if isinstance(game, unicode):
            game = game.encode("utf-8")
        with open(path, mode) as f:
            f.write("-- " + game + "\n" + text + "\n\n")
    except Exception as e:
        _dulcamara.log(2, "renpy: the error could not be written to %s: %s" % (folder, describe(e)))


def log_error(text):
    if isinstance(text, unicode):
        text = text.encode("utf-8")
    _dulcamara.log(2, "Ren'Py reported an error:")
    for line in text.splitlines():
        _dulcamara.log(2, line)
    save_error(text)


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


# DULCAMARA_PICTURE_HEIGHT=<h> (renpy-test.txt, on a console): a game taller than h is drawn h high (its width to
# match), as its window would be that size on a computer, and the picture the core reads back each time is smaller. A
# tester's Ren'Py games of 1920x1080 drew 16 to 22 pictures a second, 28 ms of each reading it back, and she sees the
# black flashes where they run below 60.
def picture_height():
    setting = os.environ.get("DULCAMARA_PICTURE_HEIGHT", "")
    return int(setting) if setting.isdigit() and int(setting) >= 240 else 0


sizes_noted = set()


def physical_size_get(preferences):
    height = picture_height()
    config = sys.modules["renpy.config"]
    width_v, height_v = config.screen_width, config.screen_height
    if height and height_v > height:
        size = (int(round(width_v * height / float(height_v))), height)
        # noted as the window is made (Ren'Py reads its preferences before the game sets its size)
        if getattr(sys.modules.get("renpy.game"), "interface", None) is not None and size not in sizes_noted:
            sizes_noted.add(size)
            _dulcamara.log(1, "renpy: the game's %dx%d is drawn at %dx%d (DULCAMARA_PICTURE_HEIGHT)" % (
                width_v, height_v, size[0], size[1]))
        return size
    # A window size the game saved (on a computer, or by its own resolution option) reads as unset: Ren'Py's resize
    # would make the picture that size whatever its shape, and pad the game inside it with black bars, which the app
    # then fits to the TV with bars of its own. The game's own size keeps its shape; the app scales it.
    return None


def physical_size_set(preferences, value):
    preferences.__dict__["physical_size"] = value


# Maximized reads as off, as Fullscreen does: a maximized window keeps a saved size's shape where Ren'Py otherwise
# makes it the game's (select_physical_size). The game's own value is kept for a computer.
def maximized_get(preferences):
    return False


def maximized_set(preferences, value):
    preferences.__dict__["maximized"] = value


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

    def close(self):
        # Ren'Py 7's sound (pygame_sdl2's python_close) closes the file it was given as the sound ends; without this it
        # failed to, and kept this object and its open file for good: one more for each play and each loop
        self._file.close()


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
        with io.open(listed + ".new", "w", encoding="utf-8", errors="replace", newline="\n") as f:
            # a name not in UTF-8 (bytes) as near as it comes, not the whole list lost
            f.write(u"".join((name if isinstance(name, unicode) else name.decode("utf-8", "replace")) + u"\n"
                             for name in names))
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
                # the file's own name, which the loader finds whatever the case the game gives it in
                import unicodedata
                key = unicodedata.normalize("NFC", (prefix + found).lower())
                return name, prefix + found, loader.lower_map.get(key, prefix + found)
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


# Each copy of a sound that plays at once is a decode thread of its own, and the console refuses threads past some 31:
# a tester's fast cursor over a title menu played its hover sound on 35 channels at once (Ren'Py's audio channel opens
# another while the others play), the threads ran out, and the game stopped answering. A sound plays on
# DULCAMARA_SAME_SOUND_AT_ONCE channels at once at most (2 unless that is set): another copy stops the one started
# first.
SAME_SOUND_AT_ONCE = max(1, int(os.environ.get("DULCAMARA_SAME_SOUND_AT_ONCE", "2") or 2))
started = {}  # channel: (the sound's name, when it was started, counted)
started_count = [0]
copies_stopped = [0]


def fewer_copies(channel, name):
    renpysound = sys.modules["renpy.audio.renpysound"]
    copies = []
    for other, (other_name, order) in list(started.items()):
        if other == channel or other_name != name:
            continue
        try:
            playing = renpysound.playing_name(other) == name
        except Exception:
            playing = False
        if playing:
            copies.append((order, other))
        else:
            started.pop(other, None)
    copies.sort()
    while len(copies) >= SAME_SOUND_AT_ONCE:
        _order, oldest = copies.pop(0)
        started.pop(oldest, None)
        try:
            renpysound.stop(oldest)
        except Exception:
            continue
        copies_stopped[0] += 1
        if copies_stopped[0] <= 20 or copies_stopped[0] % 500 == 0:
            _dulcamara.log(1, "renpy: %s plays on %d channels at once: the copy on channel %s stopped (%d so far)" % (
                describe(name), SAME_SOUND_AT_ONCE + 1, describe(oldest), copies_stopped[0]))
    started_count[0] += 1
    started[channel] = (name, started_count[0])


def sound_play(channel, file, name, *args, **kwargs):
    file = sound("plays", channel, file, name, kwargs)
    try:
        fewer_copies(channel, name)
    except Exception as e:
        _dulcamara.log(2, "renpy: the copies of %s were not counted: %s" % (describe(name), describe(e)))
    try:
        return originals["sound_play"](channel, file, name, *args, **kwargs)
    except Exception as e:
        sound_failed("play", name, channel, e)
        raise


def sound_queue(channel, file, name, *args, **kwargs):
    file = sound("queues", channel, file, name, kwargs)
    try:
        return originals["sound_queue"](channel, file, name, *args, **kwargs)
    except Exception as e:
        sound_failed("queue", name, channel, e)
        raise


# A sound Ren'Py cannot load, play or queue: Ren'Py says nothing and, if its channel loops it (a game's music), takes it
# off the loop for good, so that the music plays once (a tester's games, on the console only: on the build machine they
# loop). In the log, the first 20 and every 100th; a looping track's failures as loop_periodic tells them.
sounds_failed = [0]
# while a channel's periodic call runs (loop_periodic), on its thread: what failed in it, for loop_periodic to report
periodic_failures = [None]


def sound_failed(what, name, channel, e):
    held = periodic_failures[0]
    if held is not None and held[0] is threading.current_thread():
        held[1].append((what, name, channel, e))
        return
    log_sound_failure(what, name, channel, e)


def log_sound_failure(what, name, channel, e):
    sounds_failed[0] += 1
    if sounds_failed[0] <= 20 or sounds_failed[0] % 100 == 0:
        where = " on channel %s" % describe(channel) if channel is not None else ""
        _dulcamara.log(2, "renpy: Ren'Py could not %s %s%s, and stops looping it if it did: %s (%d so far)"
                       % (what, describe(name), where, describe(e), sounds_failed[0]))


def audio_load(fn, *args, **kwargs):
    try:
        return originals["audio_load"](fn, *args, **kwargs)
    except Exception as e:
        sound_failed("load", fn, None, e)
        raise


# A looping track (a game's music) that Ren'Py could not load, play or queue again: Ren'Py takes it off its channel's
# loop for good (renpy/audio/audio.py, Channel.periodic), so that a hiccup once (an archive read, a decoder, a thread
# the console would not start) ended the music for the rest of the scene. The track stays on the loop and is tried
# again LOOP_RETRY_FIRST seconds later, the wait doubling up to LOOP_RETRY_LONGEST; after LOOP_RETRIES failures in a
# row (its file really gone) Ren'Py's own choice stands. In the log: each track's first error, and the end of its tries.
# The seconds are the game's (the core's clock, as Ren'Py's own thread last saw it as it asked its audio to catch up):
# the channels' periodic calls may run on a thread of their own, whose clock is this computer's.
LOOP_RETRIES = 6
LOOP_RETRY_FIRST, LOOP_RETRY_LONGEST = 0.25, 2.0
audio_clock = [0.0]
# channel: [the track, failures in a row, its entry, the loop it had, the loop left, when it is tried, the loop as left]
loop_retries = {}
loop_logged = set()  # (channel name, track): its error is in the log


def audio_periodic(*args, **kwargs):
    audio_clock[0] = _dulcamara.monotonic()
    return originals["audio_periodic"](*args, **kwargs)


def loop_periodic(self):
    retry = loop_retries.get(self)
    if retry is not None and retry[5] is not None and audio_clock[0] >= retry[5]:
        if self.loop is retry[4] and list(self.loop) == retry[6]:
            # the game has not changed the channel's music meanwhile: the track goes back on the loop and the queue
            self.loop[:] = retry[3]
            self.queue.insert(0, retry[2])
            retry[5] = None
        else:
            loop_retries.pop(self, None)
            retry = None
    loop_before, queue_before = self.loop, list(self.queue)
    loop_copy = list(loop_before)
    failures = []
    outer = periodic_failures[0]
    periodic_failures[0] = (threading.current_thread(), failures)
    try:
        return originals["channel_periodic"](self)
    finally:
        periodic_failures[0] = outer
        try:
            loop_after(self, retry, loop_before, loop_copy, queue_before, failures)
        except Exception as e:
            _dulcamara.log(2, "renpy: the loop on channel %s was not looked after: %s" % (
                describe(getattr(self, "name", None)), describe(e)))


def loop_after(self, retry, loop_before, loop_copy, queue_before, failures):
    gone = [name for name in loop_copy if name not in self.loop] if self.loop is loop_before else []
    entry = None
    for each in queue_before:
        if gone and each not in self.queue and getattr(each, "filename", None) == gone[0]:
            entry = each
            break
    if entry is None:
        for what, name, channel, e in failures:
            log_sound_failure(what, name, channel, e)
        if retry is not None and retry[5] is None and retry[2] not in self.queue and self.loop is retry[4]:
            # the track tried again went on to Ren'Py's sound
            loop_retries.pop(self, None)
            _dulcamara.log(1, "renpy: %s loops again on channel %s, after %d failure%s" % (
                describe(retry[0]), describe(self.name), retry[1], "" if retry[1] == 1 else "s"))
        return
    track = entry.filename
    count = retry[1] + 1 if retry is not None and retry[0] == track else 1
    what, error = "queue", "no error given"
    if failures:
        what, error = failures[-1][0], describe(failures[-1][3])
    key = (self.name, describe(track))
    if key not in loop_logged:
        loop_logged.add(key)
        _dulcamara.log(2, "renpy: Ren'Py could not %s %s, looping on channel %s: %s; it stays on the loop and is "
                       "tried again (%d times at most)" % (what, describe(track), describe(self.name), error,
                                                         LOOP_RETRIES - 1))
    if count >= LOOP_RETRIES:
        loop_retries.pop(self, None)
        _dulcamara.log(2, "renpy: %s failed %d times in a row on channel %s: no more tries, and Ren'Py stops "
                       "looping it" % (describe(track), count, describe(self.name)))
        return
    wait = min(LOOP_RETRY_LONGEST, LOOP_RETRY_FIRST * 2 ** (count - 1))
    loop_retries[self] = [track, count, entry, loop_copy, self.loop, audio_clock[0] + wait, list(self.loop)]


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
            if len(counts) > 3 and counts[3] >= 0:
                parts.append("the console's system memory %d MB free" % (counts[3] >> 20))
        loader = getattr(sys.modules["renpy.display"].draw, "texture_loader", None)
        if loader is not None:
            size, count = loader.get_texture_size()
            parts.append("Ren'Py's textures %d (%d MB)" % (count, int(size) >> 20))
        cache = sys.modules["renpy.display.im"].cache
        parts.append("its image cache %d MB" % (cache.get_total_size() * 4 >> 20))
        parts.append("%d files open, %d Python threads" % (open_files(), threading.active_count()))
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
    presplash = sys.modules["renpy.display.presplash"]
    try:
        rv = originals["presplash_start"](basedir, gamedir)
    except Exception as e:
        # a presplash Ren'Py cannot read ended the launch there: the game starts without it
        _dulcamara.log(2, "renpy: the presplash could not be read, and the game starts without it: %s" % describe(e))
        window = getattr(presplash, "window", None)
        presplash.window = presplash.progress_bar = None
        try:
            if window is not None:
                window.destroy()
        except Exception:
            pass
        return None
    show_presplash(presplash)
    return rv


# Ren'Py's presplash files (presplash.png or .jpg, or presplash_foreground and presplash_background, renpy/display/
# presplash.py), which it looks for with os.path.exists: on Windows, where games are made, in any case; the console's
# file system tells Presplash.JPG from presplash.jpg. Found here as on Windows, Ren'Py's own order kept.
PRESPLASH_EXTENSIONS = (".png", ".jpg")


def presplash_find_file(base_name, root):
    try:
        names = sorted(os.listdir(root))
    except Exception:
        return originals["presplash_find_file"](base_name, root)
    for ext in PRESPLASH_EXTENSIONS:
        wanted = base_name + ext
        if os.path.exists(os.path.join(root, wanted)):
            return os.path.join(root, wanted)
        for name in names:
            if name.lower() == wanted.lower() and os.path.isfile(os.path.join(root, name)):
                _dulcamara.log(1, "renpy: the presplash's %s is %s" % (wanted, describe(name)))
                return os.path.join(root, name)
    return None


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
    try:
        loaded = originals["image_get"](cache, image, *args, **kwargs)
    except Exception as e:
        image_failed(image, e)
        raise
    note_size(image, loaded)
    return loaded


# An image Ren'Py cannot load (no file descriptor left, no memory, a file it cannot open or read) is shown as nothing
# by a released game, which goes on without a word (config.raise_image_exceptions is off), and its preloader leaves it
# for good: a tester's characters went missing while the game ran. Each such image is logged once with why, the first
# 20 and every 100th after.
images_failed = {"count": 0, "names": set()}


def image_failed(image, e):
    try:
        name = getattr(image, "filename", None) or image.identity
        if name in images_failed["names"]:
            return
        images_failed["names"].add(name)
        images_failed["count"] += 1
        n = images_failed["count"]
        if n <= 20 or n % 100 == 0:
            where = "its preloader" if threading.current_thread() is getattr(
                sys.modules["renpy.display.im"].cache, "preload_thread", None) else "Ren'Py's own thread"
            _dulcamara.log(2, "renpy: Ren'Py could not load the image %s (on %s), and shows it as nothing: %s "
                              "(%d images so far; %d files open)" % (describe(name), where, describe(e), n, open_files()))
    except Exception:
        pass


def open_files():
    """how many files the process has open (of the first 4096)"""
    count = 0
    for fd in range(4096):
        try:
            os.fstat(fd)
            count += 1
        except OSError:
            pass
    return count


# A movie with a mask of its own (Movie(play=..., mask=...)) is two streams, each decoded on its own thread. Ren'Py
# uploads a colour frame whose mask frame has not come yet as it is (get_movie_texture): such a movie's colour frames
# are black wherever its mask makes it clear, so a box's animated border flashed black each time the mask's frame came
# late on the console, and stayed black when the mask's stream did not open. Here each stream's frames wait for the
# other's, and the oldest of each are drawn together, whichever comes first (two of each kept at most); a movie whose
# mask never comes is not drawn.
movie_frames = {}  # channel: (the name playing on it, its colour frames waiting, its mask frames waiting)
movie_waits = [0]


def movie_texture(channel, mask_channel=None, side_mask=False, mipmap=None):
    import renpy

    video = sys.modules["renpy.display.video"]
    if not mask_channel or side_mask or renpy.emscripten:
        return originals["get_movie_texture"](channel, mask_channel, side_mask, mipmap)
    playing = renpy.audio.music.get_playing(channel)
    if not playing:
        movie_frames.pop(channel, None)
        return None, False
    if mipmap is None:
        mipmap = renpy.config.mipmap_movies
    surf = renpy.audio.music.get_channel(channel).read_video()
    mask_surf = renpy.audio.music.get_channel(mask_channel).read_video()
    name, colours, masks = movie_frames.get(channel, (None, [], []))
    if name != playing:  # another movie on the channel: none of the last one's frames
        name, colours, masks = playing, [], []
    if surf is not None:
        colours = (colours + [surf])[-2:]
    if mask_surf is not None:
        masks = (masks + [mask_surf])[-2:]
    surf = mask_surf = None
    if colours and masks:
        surf, mask_surf = colours.pop(0), masks.pop(0)
    elif colours:
        movie_waits[0] += 1
        if movie_waits[0] <= 20 or movie_waits[0] % 600 == 0:
            _dulcamara.log(1, "renpy: movie %s: a picture waits for its mask's (%d so far)" % (channel, movie_waits[0]))
    movie_frames[channel] = (name, colours, masks)
    if surf is not None:
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


# Ren'Py's own threads, which it starts as a game loads: its image preloader (as renpy.display.im is imported), its
# save scanner (twice) and its sound's periodic work. The console refuses a thread when its system memory for them has
# run out, and Ren'Py then ended in its error screen as the game loaded ("can't start new thread"), which a relaunch put
# right. A refused thread is asked for again a few times, a moment apart (real time); one of those three still refused
# is done without its thread, as Ren'Py does where it has no threads (the web): the preloader's passes and the sound's
# periodic work on Ren'Py's own thread, and the scanner, which only notices saves changed outside the game, not at all.
# Each is said once in the log. Any other thread refused raises as before (the autosave's is handled below).
real_sleep = time.sleep  # (the boot script makes time.sleep the core's after it imports this)
threads_refused = [0]
services_without_thread = set()
preloader = {"inline": False, "retry": 0.0, "passes": 0}
SERVICE_THREADS = {
    "preloader": "image preloader no thread: Ren'Py's own thread preloads the images and keeps its image cache "
                 "within its size",
    "run_scan_thread": "save scanner no thread: saves changed outside the game are noticed at its next start",
    "periodic_thread_main": "sound's periodic work no thread: Ren'Py's own thread does it",
}


def is_refusal(e):
    return "can't start new thread" in str(e)


def service_of(thread):
    if getattr(thread, "name", None) == "preloader":
        return "preloader"
    target = getattr(thread, "_target", None) or getattr(thread, "_Thread__target", None)
    name = getattr(target, "__name__", None)
    return name if name in SERVICE_THREADS else None


def start_thread(thread):
    tries = 0
    while True:
        try:
            rv = originals["thread_start"](thread)
            if tries:
                _dulcamara.log(1, "renpy: the console gave a thread (%s) on try %d" % (
                    service_of(thread) or getattr(thread, "name", "?"), tries + 1))
            return rv
        except Exception as e:
            if not is_refusal(e):
                raise
            tries += 1
            threads_refused[0] += 1
            service = service_of(thread)
            if tries <= 4:
                real_sleep(0.02 * tries)
                continue
            if service is None:
                raise
            break
    # never started: there is nothing to wait for as Ren'Py quits it
    thread.join = lambda timeout=None: None
    if service == "periodic_thread_main":
        sys.modules["renpy.config"].audio_periodic_thread = False
    elif service == "preloader":
        preloader["inline"] = True
        preloader["retry"] = real_clock() + 10
    if service not in services_without_thread:
        services_without_thread.add(service)
        _dulcamara.log(2, "renpy: the console gave Ren'Py's %s (%d threads refused so far)"
                       % (SERVICE_THREADS[service], threads_refused[0]))


def preload_inline():
    """Each periodic callback with the preloader's thread refused: a preload pass on Ren'Py's own thread (it loads
    what Ren'Py predicted and cleans out the image cache), and every ten seconds another try at the thread"""
    if not preloader["inline"]:
        return
    cache = sys.modules["renpy.display.im"].cache
    if cache.preload_thread is not None and not cache.preload_thread.is_alive():
        cache.preload_thread = None  # as where Ren'Py has no threads
    if real_clock() >= preloader["retry"]:
        preloader["retry"] = real_clock() + 10
        thread = threading.Thread(target=cache.preload_thread_main, name="preloader")
        thread.daemon = True
        try:
            originals["thread_start"](thread)
        except Exception as e:
            if not is_refusal(e):
                raise
        else:
            cache.preload_thread = thread
            preloader["inline"] = False
            _dulcamara.log(1, "renpy: Ren'Py's image preloader has its thread again, after %d passes on Ren'Py's own"
                           % preloader["passes"])
            with cache.preload_lock:
                cache.preload_lock.notify()
            return
    preloader["passes"] += 1
    cache.in_preload_pass = True
    try:
        cache.preload_thread_pass()
    finally:
        cache.in_preload_pass = False


# A background autosave the console gives no thread for (Ren'Py's force_autosave raised "can't start new thread" and
# showed its error screen: 0.2.10's Ren'Py 7, at a choice, late in a session) is made at once on Ren'Py's own thread,
# as Ren'Py makes a blocking one. Its flag still said an autosave ran, and its thread, never started, could not be
# joined: both are put right first.
autosaves_inline = [0]


def force_autosave(take_screenshot=False, block=False):
    try:
        return originals["force_autosave"](take_screenshot, block)
    except Exception as e:
        if block or "can't start new thread" not in str(e):
            raise
    loadsave = sys.modules["renpy.loadsave"]
    loadsave.autosave_thread = None
    loadsave.autosave_not_running.set()
    autosaves_inline[0] += 1
    if autosaves_inline[0] <= 20 or autosaves_inline[0] % 100 == 0:
        _dulcamara.log(2, "renpy: the console gave the autosave no thread: it is made on Ren'Py's own (%d so far)"
                       % autosaves_inline[0])
    return originals["force_autosave"](take_screenshot, True)


# Volume sliders as they were for a game made for a Ren'Py before 8.1 (7.6 on Ren'Py 7's line). Those releases made a
# mixer slider decibels: half is -20 dB, main and music at half -40 dB (a hundredth, silent on a TV), and
# config.default_music_volume and the like are read the same way. Before them a slider was quadratic: half is a quarter
# of the amplitude (-12 dB). Ren'Py keeps that as config.quadratic_volumes, which it sets itself for a game that ships
# game/script_version.txt (00compat.rpy) and which its sliders (MixerValue) and default volumes (_vol) follow; this
# sets it for an older game found by its renpy/ folder too, and makes preferences.set_mixer and get_mixer (the game's
# Preference("music volume", 0.5), which do not follow it) quadratic as well. A game made for 8.1, 7.6 or later, or
# one that says nothing, keeps Ren'Py's own decibels.
old_volumes = [None]  # the (major, minor, patch) of an older game, whose sliders are quadratic


def named(folder, name):
    """folder/name, its name in any case; None when there is none (or no folder)."""
    if not folder:
        return None
    try:
        for entry in os.listdir(folder):
            if entry.lower() == name.lower():
                return os.path.join(folder, entry)
    except (IOError, OSError, TypeError):
        pass
    return None


def game_renpy_version(game):
    """The (major, minor, patch) of the Ren'Py a game was made with: its renpy/vc_version.py's version, else its
    renpy/__init__.py's version_tuple (Ren'Py 7.4 and older), else its game/script_version.txt; None when nothing
    says (some repacks)."""
    renpy_dir, game_dir = named(game, "renpy"), named(game, "game")
    sources = [(named(renpy_dir, "vc_version.py"), r"^version\s*=\s*u?['\"](\d+)\.(\d+)\.(\d+)"),
               (named(renpy_dir, "__init__.py"), r"^version_tuple\s*=\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)"),
               (named(game_dir, "script_version.txt"), r"^\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)")]
    for path, pattern in sources:
        if not path:
            continue
        try:
            with open(path, "rb") as f:
                text = f.read(65536).decode("utf-8", "replace")
        except (IOError, OSError):
            continue
        match = re.search(pattern, text, re.M)
        if match:
            return tuple(int(n) for n in match.groups())
    return None


def old_volumes_for(game):
    version = game_renpy_version(game) if game else None
    if version and (version[0] < 7 or (version[0] == 7 and version[1] < 6) or (version[0] == 8 and version[1] < 1)):
        old_volumes[0] = version
        _dulcamara.log(1, "renpy: the game was made for Ren'Py %d.%d.%d, before volume sliders were decibels (8.1, "
                          "7.6): its sliders are quadratic, as they were then" % version)


def quadratic_set_mixer(preferences, mixer, volume):
    # a slider's 0 to 1, squared: the amplitude (at most 1)
    volume = min(max(volume, 0.0), 1.0)
    preferences.set_volume(mixer, volume * volume)


def quadratic_get_mixer(preferences, mixer):
    return math.sqrt(min(max(preferences.get_volume(mixer), 0.0), 1.0))


class QuadraticVolumes(object):
    """A step of Ren'Py's init, after its own at -1500 and before the default volumes are applied (at 1500): Ren'Py 8
    runs execute_init, Ren'Py 7 calls it."""

    filename = "dulcamara_errors.py"
    linenumber = 0

    def execute_init(self):
        sys.modules["renpy.config"].quadratic_volumes = True  # MixerValue and _vol square, a bar runs from 0 to 1

    def __call__(self):
        self.execute_init()


def load_script(script, *args, **kwargs):
    rv = originals["load_script"](script, *args, **kwargs)
    at = 0
    while at < len(script.initcode) and script.initcode[at][0] <= -1500:
        at += 1
    script.initcode.insert(at, (-1500, QuadraticVolumes()))
    return rv


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
        originals["presplash_find_file"] = module.find_file
        module.start = presplash_start
        module.find_file = presplash_find_file
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
    elif module.__name__ == "renpy.audio.audio" and "audio_load" not in originals:
        # a sound Ren'Py cannot load is in the log (a loop drops it)
        originals["audio_load"] = module.load
        module.load = audio_load
        # a looping track Ren'Py could not queue again stays on the loop (loop_periodic)
        originals["channel_periodic"] = module.Channel.__dict__["periodic"]
        module.Channel.periodic = loop_periodic
        originals["audio_periodic"] = module.periodic
        module.periodic = audio_periodic
    elif module.__name__ == "renpy.loadsave":
        # Ren'Py 7.3 exposed dumps alongside loads. Both still use the same Python 2 save protocol.
        from renpy.compat.pickle import dumps
        module.dumps = dumps
        # each time Ren'Py loads it: an autosave the console gives no thread for is made on Ren'Py's own
        originals["force_autosave"] = module.force_autosave
        module.force_autosave = force_autosave
    elif module.__name__ == "renpy.preferences":
        # Fullscreen (a game's Options > Display, or its key) reads as off: Ren'Py would make its window the
        # display's size (3840x2160 on the console, by SDL's account) and the picture with it, and the screen went
        # black. The console's picture is the game's own size, scaled by the app.
        module.Preferences.fullscreen = property(fullscreen_get, fullscreen_set)
        module.Preferences.maximized = property(maximized_get, maximized_set)
        module.Preferences.physical_size = property(physical_size_get, physical_size_set)
        if os.environ.get("DULCAMARA_SKIP_UNSEEN") == "1":
            module.Preferences.skip_unseen = property(skip_unseen_get, skip_unseen_set)
        if old_volumes[0]:
            module.Preferences.set_mixer = quadratic_set_mixer
            module.Preferences.get_mixer = quadratic_get_mixer
    elif module.__name__ == "renpy.script" and old_volumes[0] and "load_script" not in originals:
        originals["load_script"] = module.Script.load_script
        module.Script.load_script = load_script
    elif module.__name__ == "renpy.config":
        # Ren'Py's framebuffer, where it makes images ready and draws pictures into textures, is the size of its largest
        # texture (config.max_texture_size, 4096x4096: 64 MB, and as much again for its depth), cleared whole for each
        # picture it draws into a texture. On the console 2048x2048, or a larger game's own size and a border (Ren'Py's
        # own sizing): some 96 MB of graphics memory back, which a tester's game seemed to run out of.
        # (and fbo_size, which 00compat.rpy raises it to)
        size = max_texture_size()
        if size:
            module.max_texture_size = module.fbo_size = (size, size)
        # DULCAMARA_MIPMAPS=0 (renpy-test.txt, on a console): images without mipmaps, drawn smaller from their own
        # pixels; the console's driver makes mipmaps partly on the GPU and partly on the CPU
        if os.environ.get("DULCAMARA_MIPMAPS") == "0":
            module.max_mipmap_level = 0
            _dulcamara.log(1, "renpy: images have no mipmaps (DULCAMARA_MIPMAPS=0)")
        module.periodic_callbacks.append(log_memory)
        module.periodic_callbacks.append(preload_inline)
        module.periodic_callbacks.append(list_sounds)


class Finder(object):
    """Python 2's import hook: the two modules are imported as they would be, then wrapped."""

    names = ("renpy.error", "renpy.display.error", "renpy.display.core", "renpy.preferences", "renpy.log",
             "renpy.audio.renpysound", "renpy.display.focus", "renpy.display.im", "renpy.display.presplash",
             "renpy.config", "renpy.display.video", "renpy.loadsave", "renpy.audio.audio",
             "renpy.script")
    loading = set()  # each is imported as it would be, so it skips itself; one it imports is wrapped in turn

    def find_module(self, name, path=None):
        if name in self.names and name not in self.loading:
            return self
        return None

    def load_module(self, name):
        Finder.loading.add(name)
        try:
            __import__(name)
        finally:
            Finder.loading.discard(name)
        module = sys.modules[name]
        originals.pop({"renpy.error": "report_exception", "renpy.display.error": "report_parse_errors",
                       "renpy.display.core": "quit_event", "renpy.log": "log_write"}.get(name), None)
        wrap(module)
        return module


def install(game=None):
    old_volumes_for(game)
    sys.meta_path.insert(0, Finder())
    if "thread_start" not in originals:
        originals["thread_start"] = threading.Thread.start
        threading.Thread.start = start_thread
