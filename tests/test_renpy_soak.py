#!/usr/bin/env python3
# Dulcamara - a long session on both Ren'Py cores, run headless: scene after scene, its characters, text, music, saves
# and rollbacks, and whether each scene's characters reach the picture while nothing the core runs on piles up.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""A tester played one Ren'Py game for some four hours, and then its characters no longer appeared while the game ran
on. This plays the test game of tests/renpy-game with a script of its own (game/soak.rpy, written here): each scene a
background and up to three characters (each a colour of its own), a line of text, now and then a transition, a change
of music (from an archive), a sound, a save, an autosave on its thread and a rollback, and many more images than
Ren'Py's image cache holds. For each scene the game says which colours are to be where; the headless frontend
(renpy_headless.c, "soak") looks at the picture and counts each scene whose background, characters and text are all
there. Every few scenes it prints a row: the game's time, Ren'Py's textures and image cache, Python's objects and
threads, and the process's threads, open files and resident memory.

The normal suite plays a short session (SHORT_SCENES); `python3 tests/test_renpy_soak.py --long` (or
DULCAMARA_SOAK_LONG=1) plays LONG_SCENES, over six hours of the game's time, and prints the whole table.
DULCAMARA_SOAK_CSV=<file> writes the table there too."""

import colorsys
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

try:
    from tests import test_renpy_core as renpy8
except ImportError:  # unittest discover -s tests
    import test_renpy_core as renpy8

ROOT = renpy8.ROOT
SHORT_SCENES, LONG_SCENES = 120, 6000
WIDTH, HEIGHT = 640, 360
BACKGROUNDS, CHARACTERS = 60, 40
SLOTS = [(20, 10), (260, 10), (500, 10)]  # each character's top left, 120x160
FRAME_USEC = 100000  # a tenth of a second of the game's time each frame: hours pass in minutes


def solid_png(path, width, height, rgb, border=0):
    """A picture of one colour; with a border of that many clear pixels round it (RGBA)."""
    r, g, b = (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255
    if border:
        clear_row = b"\0" + b"\0\0\0\0" * width
        row = b"\0" + b"\0\0\0\0" * border + bytes((r, g, b, 255)) * (width - 2 * border) + b"\0\0\0\0" * border
        raw = clear_row * border + row * (height - 2 * border) + clear_row * border
        kind = 6
    else:
        raw = (b"\0" + bytes((r, g, b)) * width) * height
        kind = 2

    def chunk(name, data):
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, kind, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw))
                     + chunk(b"IEND", b""))


def colours(count, saturation, value, offset):
    """`count` colours far enough apart, none near white or black"""
    rv = []
    for i in range(count):
        r, g, b = colorsys.hsv_to_rgb(((i * 0.618034) + offset) % 1.0, saturation, value - 0.25 * (i % 3) / 2)
        rv.append((int(r * 255) << 16) | (int(g * 255) << 8) | int(b * 255))
    return rv


SOAK_RPY = r'''
# tests/test_renpy_soak.py's long session (written by the test)
init python:
    import gc
    import os
    import sys
    import threading
    import _dulcamara

    soak_n = int(os.environ.get("DULCAMARA_TEST_SOAK", "0"))
    soak_every = int(os.environ.get("DULCAMARA_SOAK_EVERY", "25"))
    soak_pause = float(os.environ.get("DULCAMARA_SOAK_PAUSE", "2.0"))
    SOAK_BG = %(backgrounds)r
    SOAK_SP = %(characters)r
    SOAK_SLOTS = %(slots)r
    # rollbacks done, kept where a rollback does not take them back
    sys.dulcamara_soak_rolled = set()

    def soak_scene(i):
        bg = (i * 7) %% len(SOAK_BG)
        sprites = [(i * 5 + k * 11) %% len(SOAK_SP) for k in range(i %% 4)]
        return bg, sprites, [None, dissolve, fade, Dissolve(0.3)][i %% 4]

    def soak_expect(bg, sprites):
        checks = ["200,90=%%06x" %% SOAK_BG[bg]]
        for k, (x, y) in enumerate(SOAK_SLOTS):
            colour = SOAK_SP[sprites[k]] if k < len(sprites) else SOAK_BG[bg]
            checks.append("%%d,%%d=%%06x" %% (x + 60, y + 80, colour))
        checks.append("40,200,600,260#30=ffffff")
        return " ".join(checks)

    def soak_starve():
        held = []
        while len(held) < 100000:
            try:
                held.append(open(os.devnull, "rb"))
            except (IOError, OSError):
                break
        return held

    def soak_stats(i):
        loader = getattr(renpy.display.draw, "texture_loader", None)
        size, count = loader.get_texture_size() if loader is not None else (0, -1)
        cache = renpy.display.im.cache
        objects = gc.get_objects()
        surfaces = sum(1 for o in objects if type(o).__name__ == "Surface")
        preloader = getattr(cache, "preload_thread", None)
        return ("scene=%%d hours=%%.3f textures=%%d texture_mb=%%d cache_entries=%%d cache_mb=%%d objects=%%d "
                "surfaces=%%d py_threads=%%d preloader=%%s" %% (
                    i, _dulcamara.monotonic() / 3600.0, count, int(size) >> 20, len(cache.cache),
                    cache.get_total_size() * 4 >> 20, len(objects), surfaces, threading.active_count(),
                    "alive" if preloader is not None and preloader.is_alive() else "dead"))

init 1 python:
    config.has_autosave = True
    # a timed pause after a rollback waits, as a line of dialogue would (else the game, which has none, runs on at once)
    config.pause_after_rollback = True
    if os.environ.get("DULCAMARA_SOAK_CACHE_MB"):
        config.image_cache_size_mb = int(os.environ["DULCAMARA_SOAK_CACHE_MB"])

label dulcamara_soak:
    $ soak_i = 0
    $ dulcamara_test("soak-stats " + soak_stats(0))
    while soak_i < soak_n:
        # DULCAMARA_SOAK_REFUSE_AT=<scene>: from that scene on, the system refuses every thread (the refuse_threads
        # library, told to while DULCAMARA_SOAK_REFUSE_FILE exists), as the console does once its memory for them is gone
        if soak_i == int(os.environ.get("DULCAMARA_SOAK_REFUSE_AT", "-1")):
            $ open(os.environ["DULCAMARA_SOAK_REFUSE_FILE"], "w").close()
            $ dulcamara_test("soak-refusing threads from scene %%d" %% soak_i)
        # DULCAMARA_SOAK_STARVE_AT=<scene>: from that scene on, the process has no file descriptor left (the game
        # takes every one it can open, at each scene again), as when something leaks them for hours
        if 0 <= int(os.environ.get("DULCAMARA_SOAK_STARVE_AT", "-1")) <= soak_i:
            $ sys.dulcamara_starved = getattr(sys, "dulcamara_starved", []) + soak_starve()
        $ soak_bg, soak_sprites, soak_with = soak_scene(soak_i)
        scene expression "soak_bg_%%d" %% soak_bg
        with soak_with
        $ soak_k = 0
        while soak_k < len(soak_sprites):
            $ renpy.show("soak_sp_%%d" %% soak_sprites[soak_k], tag="slot%%d" %% soak_k,
                         at_list=[Position(xpos=SOAK_SLOTS[soak_k][0], ypos=SOAK_SLOTS[soak_k][1], xanchor=0, yanchor=0)])
            $ soak_k += 1
        with dissolve
        # now and then a rollback to the scene before, which is shown again; then this one again, as the player rolls
        # forward (before this scene's expectation: Ren'Py may skip through a scene it rolls forward through)
        if soak_i %% 50 == 49 and soak_i not in sys.dulcamara_soak_rolled:
            $ sys.dulcamara_soak_rolled.add(soak_i)
            $ dulcamara_test("soak-rollback %%d" %% soak_i)
            $ renpy.rollback(force=True, checkpoints=1)
        if soak_i %% 10 == 0:
            play music "audio/soak_%%d.wav" %% (soak_i // 10 %% 3)
        if soak_i %% 6 == 3:
            play sound "audio/soak_%%d.wav" %% (soak_i %% 3)
        $ renpy.say(None, "Scene %%d, said in white." %% soak_i, interact=False)
        $ dulcamara_test("soak %%d expect %%s" %% (soak_i, soak_expect(soak_bg, soak_sprites)))
        $ renpy.pause(soak_pause, hard=True, checkpoint=True)
        if soak_i %% 25 == 24:
            $ renpy.save("soak")
        if soak_i %% 30 == 29:
            $ renpy.force_autosave()
        $ soak_i += 1
        if soak_i %% soak_every == 0:
            $ dulcamara_test("soak-stats " + soak_stats(soak_i))
    $ dulcamara_test("soak-stats " + soak_stats(soak_i))
    $ dulcamara_test("soak done")
    $ renpy.quit()
'''


def make_soak_game(directory):
    game_dir = renpy8.make_game(directory, width=WIDTH, height=HEIGHT)
    game = game_dir / "game"
    backgrounds = colours(BACKGROUNDS, 0.55, 0.85, 0.0)
    characters = colours(CHARACTERS, 0.95, 0.95, 0.31)
    for i, rgb in enumerate(backgrounds):
        solid_png(game / f"images/soak_bg_{i}.png", WIDTH, HEIGHT, rgb)
    for i, rgb in enumerate(characters):
        solid_png(game / f"images/soak_sp_{i}.png", 120, 160, rgb, border=4)
    tones = {}
    for i, hz in enumerate((330, 440, 550)):
        path = game / f"audio/soak_{i}.wav"
        renpy8.write_tone(path, seconds=0.5, hz=hz)
        tones[f"audio/soak_{i}.wav"] = path.read_bytes()
        path.unlink()
    renpy8.write_rpa(game / "soak.rpa", tones)
    (game / "soak.rpy").write_text(SOAK_RPY % {"backgrounds": backgrounds, "characters": characters,
                                               "slots": SLOTS}, encoding="utf-8")
    return game_dir


ROW = re.compile(r"^soak-row: (.*)$", re.M)


def rows(output):
    """The table's rows, each a dict of its numbers (and words)"""
    table = []
    for line in ROW.findall(output):
        row = {}
        for key, value in (part.split("=", 1) for part in line.split() if "=" in part):
            try:
                row[key] = float(value) if "." in value else int(value)
            except ValueError:
                row[key] = value
        table.append(row)
    return table


def long_mode():
    return os.environ.get("DULCAMARA_SOAK_LONG") == "1"


class Renpy8SoakTest(unittest.TestCase):
    SCRIPT = "tools/build-renpy.sh"
    BUILD = ROOT / "build/renpy-host"
    setUpClass = classmethod(renpy8.RenpyCoreTest.setUpClass.__func__)

    def soak(self, scenes, env=None, files=None):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game = make_soak_game(work / "Soak")
            headless, assets = self.BUILD / "core-check/renpy-headless", self.BUILD / "assets"
            environment = {**os.environ, "DULCAMARA_TEST_SOAK": str(scenes), "RENPY_HEADLESS_FRAME_USEC": str(FRAME_USEC),
                           "DULCAMARA_SOAK_EVERY": str(max(5, scenes // 40)), "DULCAMARA_SOAK_CACHE_MB": "8",
                           "DULCAMARA_SOUND_IN_MEMORY": "1000", "DULCAMARA_MEMORY_EVERY": "600",
                           "DULCAMARA_SOAK_PAUSE": "3.0" if scenes >= LONG_SCENES else "2.0", **(env or {})}
            def limit_files():  # `files` descriptors at most for the process, as a console has its few
                import resource
                resource.setrlimit(resource.RLIMIT_NOFILE, (files, resource.getrlimit(resource.RLIMIT_NOFILE)[1]))

            result = subprocess.run([str(headless), str(assets), "soak", str(game), str(work / "data")],
                                    capture_output=True, text=True, errors="replace", timeout=6 * 3600, check=False,
                                    env=environment, preexec_fn=limit_files if files else None)
        output = result.stdout
        table = rows(output)
        print(f"\nsoak, {scenes} scenes ({self.BUILD.name}):")
        for line in output.splitlines():
            if line.startswith(("soak", "FAIL", "PASS")) or (line.startswith("log:") and "console" in line):
                print(line)
        csv = os.environ.get("DULCAMARA_SOAK_CSV")
        if csv and table:
            keys = list(table[0])
            with open(f"{csv}.{self.BUILD.name}.csv", "w", encoding="utf-8") as f:
                f.write(",".join(keys) + "\n")
                for row in table:
                    f.write(",".join(str(row.get(k, "")) for k in keys) + "\n")
        return result, output, table

    def assert_flat(self, table, key, slack):
        """the column after the first quarter of the session grows by `slack` at most"""
        values = [row[key] for row in table if isinstance(row.get(key), (int, float))]
        self.assertGreater(len(values), 4, f"too few rows for {key}")
        settled = values[len(values) // 4]
        self.assertLessEqual(max(values[len(values) // 4:]), settled + slack,
                             f"{key} grew through the session: {values}")

    def assert_drawn(self, output, scenes):
        """every scene's background, characters and text were drawn (but those Ren'Py rolled forward through after a
        rollback, which it need not show)"""
        match = re.search(r"soak: (\d+) scenes, (\d+) as expected, (\d+) late, (\d+) missing, (\d+) rolled forward",
                          output)
        self.assertTrue(match, output[-4000:])
        seen, expected, late, missing, skipped = map(int, match.groups())
        self.assertGreaterEqual(seen, scenes)
        self.assertEqual((late, missing), (0, 0), "every scene's characters and text were drawn")
        self.assertEqual(expected + skipped, seen)
        self.assertLessEqual(skipped, 3 * (scenes // 50 + 1))

    def test_a_long_session(self):
        scenes = LONG_SCENES if long_mode() else SHORT_SCENES
        result, output, table = self.soak(scenes)
        self.assertEqual(result.returncode, 0, output[-6000:] + result.stderr[-3000:])
        # (a rollback shows scenes again: more are seen than the session has)
        self.assert_drawn(output, scenes)
        self.assertEqual(table[-1].get("preloader"), "alive", "Ren'Py's image preloader still runs")
        self.assert_flat(table, "threads", 2)
        self.assert_flat(table, "py_threads", 1)
        self.assert_flat(table, "files", 2)
        self.assert_flat(table, "textures", 40)
        self.assert_flat(table, "cache_entries", 40)


    def test_a_session_after_the_threads_run_out(self):
        # a tester's Ren'Py 7 games: "the console refuses new threads after a certain point". From scene 20 on every
        # thread is refused: each scene is still drawn whole, its autosaves are made on Ren'Py's own thread, and the
        # game ends and is closed as it would be
        refuse = renpy8.build_thread_refuser(Path(tempfile.mkdtemp()))
        if refuse is None:
            self.skipTest("needs a C compiler that makes a preloadable library")
        flag = Path(tempfile.mkdtemp()) / "refuse"
        result, output, table = self.soak(80, env={**refuse, "DULCAMARA_REFUSE_THREADS": f"file:{flag}",
                                                   "DULCAMARA_SOAK_REFUSE_AT": "20", "DULCAMARA_SOAK_REFUSE_FILE": str(flag),
                                                   "RENPY_HEADLESS_VERBOSE": "1"})
        self.assertEqual(result.returncode, 0, output[-6000:] + result.stderr[-3000:])
        self.assertIn("refuse-threads: refused thread", result.stderr)
        self.assertIn("the console gave the autosave no thread", output)
        self.assertNotIn("did not stop", output)
        self.assert_drawn(output, 80)


    def test_a_sound_closed_late_as_the_game_ends(self):
        # The soak's one crash (a full suite's run, 2026-11-10): as the game ended, Ren'Py stopped its music, whose
        # decode thread (ffmedia.c) then closes its file through Python on its own time; once that came after Python
        # had ended, the thread took the GIL of an interpreter that was gone (SIGSEGV in take_gil/new_threadstate,
        # under rwobject's close). Here each decode thread is slow to close (half a second), as a busy console may
        # make it, and the process runs on after Python, as Dulcamara does: Python ends only once the closes are done.
        slow = renpy8.build_preloaded(Path(tempfile.mkdtemp()), "slow_media_close")
        if slow is None:
            self.skipTest("needs a C compiler that makes a preloadable library")
        result, output, table = self.soak(12, env={**slow, "DULCAMARA_SLOW_MEDIA_CLOSE": "500",
                                                   "RENPY_HEADLESS_VERBOSE": "1"})
        self.assertEqual(result.returncode, 0, output[-6000:] + result.stderr[-3000:])
        self.assertIn("PASS", output)
        self.assertNotIn("sound files still closing", output)

    def test_images_that_cannot_be_loaded_are_logged(self):
        # With no file descriptor left (here the game takes every one from scene 10 on; on Ren'Py 7 a large archived
        # sound's leaked one each loop until 22e8700), a scene's images cannot be loaded, and Ren'Py, as a released game
        # does, shows the scene without them and goes on without a word: characters gone while the game runs. The log
        # now says so, with why, a line for each image at most (20, then every 100th), and the memory line counts the
        # process's open files and Python's threads.
        result, output, table = self.soak(30, env={"DULCAMARA_SOAK_STARVE_AT": "10", "DULCAMARA_MEMORY_EVERY": "10",
                                                   "RENPY_HEADLESS_VERBOSE": "1", "RENPY_HEADLESS_SOAK_FRAMES": "3000"},
                                          files=256)
        self.assertRegex(output, r"soak: scene 1\d not as expected", "the scenes after are drawn without their images")
        # (Ren'Py's loader says it cannot find a file it could not open: the count of files open says why)
        self.assertRegex(output, r"Ren'Py could not load the image u?'images/soak_\w+\.png' \(on Ren'Py's own thread\), "
                                 r"and shows it as nothing: [^\n]*\(\d+ images so far; 25\d files open\)")
        self.assertLessEqual(output.count("Ren'Py could not load the image"), 2 * 21, "a line an image at most")
        self.assertRegex(output, r"memory: [^\n]*\d+ files open, \d+ Python threads")


class Renpy7SoakTest(Renpy8SoakTest):
    SCRIPT = "tools/build-renpy7.sh"
    BUILD = ROOT / "build/renpy7-host"


if __name__ == "__main__":
    if "--long" in sys.argv:
        sys.argv.remove("--long")
        os.environ["DULCAMARA_SOAK_LONG"] = "1"
    unittest.main()
