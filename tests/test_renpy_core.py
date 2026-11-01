#!/usr/bin/env python3
# Dulcamara - Ren'Py 8's core, built for this computer and run headless on the test game of tests/renpy-game.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the libretro core the PS5 app links (tools/build-renpy.sh host: the same Ren'Py, CPython, FFmpeg and
Assimp, and the same renamed SDL calls) with the headless frontend of tests/core/renpy_headless.c, and plays
tests/renpy-game: it must load, give a picture, change it when Cross is pressed, play its sound, take a menu choice
and save, and a second run must load that save. Also runs the core's own checks (tests/core/renpy_unit.c): the
handoff and its clock, the pictures, the sound and the OpenGL read-back."""

import math
import os
import pickle
import shutil
import struct
import subprocess
import tempfile
import unittest
import wave
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "tests/renpy-game"


def write_png(path, width, height):
    """The test game's picture: a gradient, so that no two rows are alike."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows += bytes(((x + y) & 255, x & 255, y & 255))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows)))
                     + chunk(b"IEND", b""))


def write_tone(path, seconds=1.0, rate=48000, hz=440):
    """The test game's sound: a second of a 440 Hz tone, which the game loops."""
    samples = b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * hz * i / rate)))
                       for i in range(int(seconds * rate)))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(samples)


def write_rpa(path, files):
    """A Ren'Py archive (RPA-3.0) of `files` (name: bytes): its header, the files, then its index (a pickle, zlib'd),
    whose offsets and lengths are XORed with the header's key."""
    key, data, index = 0x42424242, b"", {}
    for name, blob in files.items():
        index[name] = [((34 + len(data)) ^ key, len(blob) ^ key, b"")]
        data += blob
    path.write_bytes(b"RPA-3.0 %016x %08x\n" % (34 + len(data), key) + data
                     + zlib.compress(pickle.dumps(index, protocol=2)))


def make_game(directory, width=1280, height=720, sound_rate=48000, broken=False, archived=False):
    """A copy of the test game in `directory`, with its picture, its sound and this run's screen size; `archived`, its
    sound in an archive (game/audio.rpa) instead of its own file."""
    shutil.copytree(GAME, directory)
    game = directory / "game"
    (game / "images").mkdir(exist_ok=True)
    (game / "audio").mkdir(exist_ok=True)
    write_png(game / "images/dulcamara.png", 256, 256)
    write_tone(game / "audio/tone.wav")
    if archived:
        write_rpa(game / "audio.rpa", {"audio/tone.wav": (game / "audio/tone.wav").read_bytes()})
        (game / "audio/tone.wav").unlink()
    (game / "size.rpy").write_text(f"define config.screen_width = {width}\n"
                                   f"define config.screen_height = {height}\n"
                                   f"define config.sound_sample_rate = {sound_rate}\n", encoding="utf-8")
    if broken:
        # a line that is no Ren'Py statement: the script cannot be parsed
        (game / "broken.rpy").write_text("this is not a statement\n", encoding="utf-8")
    return directory


# RENPY_EDIT_PY's editor: it notes the file it is asked to open, beside itself
PROBE_EDITOR = """
class Editor(object):
    def begin(self, new_window=False, **kwargs):
        pass

    def open(self, filename, line=None, **kwargs):
        with open(__file__ + ".opened", "w") as f:
            f.write(filename)

    def end(self, **kwargs):
        pass
"""


class RenpyCoreTest(unittest.TestCase):
    # the build script and its host build (tests/test_renpy7_core.py runs the same tests on Ren'Py 7's)
    SCRIPT = "tools/build-renpy.sh"
    BUILD = ROOT / "build/renpy-host"

    @classmethod
    def setUpClass(cls):
        if not shutil.which(os.environ.get("CC", "cc")) or not shutil.which("cmake"):
            raise unittest.SkipTest("needs a C compiler and cmake")
        build = subprocess.run(["bash", str(ROOT / cls.SCRIPT), "host"], capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError(f"{cls.SCRIPT} host failed:\n" + build.stdout[-4000:] + build.stderr[-4000:])
        print(build.stdout.strip().splitlines()[-1])

    def run_core(self, mode, game, data, *more, env=None):
        headless, assets = self.BUILD / "core-check/renpy-headless", self.BUILD / "assets"
        result = subprocess.run([str(headless), str(assets), mode, str(game), str(data), *map(str, more)],
                                capture_output=True, text=True, errors="replace", timeout=600, check=False,
                                env={**os.environ, **(env or {})})
        print(f"\n{mode} {game.name}:")
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout[-6000:] + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)
        self.assertNotIn("did not stop", result.stdout, "the core stopped Ren'Py within its ten seconds")
        return result.stdout

    def play_and_check(self, game, data):
        output = self.run_core("play", game, data)
        for line in ("XRGB8888", "input: the picture changed after Cross", "sound: playing",
                     "DULCAMARA-TEST: choice=right", "DULCAMARA-TEST: saved", "ended by the game"):
            self.assertIn(line, output)
        return output

    def test_the_core_checks(self):
        # under the sanitizers; leaks are not checked: the OpenGL driver's own allocations would be reported
        result = subprocess.run([str(self.BUILD / "core-check/renpy-unit")], capture_output=True, text=True,
                                timeout=120, check=False, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        print("\ncore checks:")
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)

    def test_play_save_and_load(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game, data = make_game(work / "Test Game"), work / "data"
            output = self.play_and_check(game, data)
            self.assertIn("picture: 1280x720", output)
            self.assertTrue(list((data / "saves/renpy/dulcamara-test-game").glob("1-1-*.save")),
                            "the save is in <data>/saves/renpy/<config.save_directory>")
            # Ren'Py saves at the last checkpoint (the menu) and replays the choice after it loads
            output = self.run_core("load", game, data, env={"DULCAMARA_TEST_LOAD": "1"})
            self.assertIn("DULCAMARA-TEST: loaded", output)
            self.assertIn("DULCAMARA-TEST: choice=right", output.split("DULCAMARA-TEST: loaded", 1)[1])

    def test_a_folder_named_in_japanese_with_spaces(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            self.play_and_check(make_game(work / "テスト ゲーム (体験版)"), work / "データ")

    def test_a_second_game_in_the_same_process(self):
        # Ren'Py's compiled modules keep their state in C statics, so Python cannot start over for a second game in
        # the same process: the core refuses it with a reason, and the app starts itself again for it.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            first, second = make_game(work / "First"), make_game(work / "Second", 1920, 1080)
            output = self.run_core("play", first, work / "data", "--then", second)
            self.assertIn("ended by the game", output)
            self.assertIn("Ren'Py runs one game each time Dulcamara starts", output)
            self.assertIn("second game: refused", output)

    def test_a_game_larger_than_the_picture(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Large", 2560, 1440), work / "data")
            self.assertIn("picture: 1920x1080", output)

    def test_a_sound_in_an_archive(self):
        # A sound in a game's archive (.rpa) is read before Ren'Py's sound has it: into memory, or, when it is large,
        # through Python's own file as it plays (DULCAMARA_SOUND_IN_MEMORY=0 makes every sound large). The log says
        # where each sound comes from.
        archived = r"from audio\.rpa at 34 \(\d+ bytes\), "
        for is_archived, limit, source in ((False, None, r"from the file .*tone\.wav"),
                                           (True, None, archived + "read into memory"),
                                           (True, "0", archived + "read through Python")):
            with tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                game = make_game(work / "Archived", archived=is_archived)
                env = {"RENPY_HEADLESS_VERBOSE": "1", **({"DULCAMARA_SOUND_IN_MEMORY": limit} if limit else {})}
                output = self.run_core("play", game, work / "data", env=env)
                self.assertIn("sound: playing", output)
                self.assertRegex(output, r"plays u?'audio/tone\.wav' on channel 0, " + source)
                self.assertNotIn("could not read the sound", output)

    def test_the_presplash(self):
        # A game's presplash (game/presplash.png, or presplash_foreground and _background, a progress bar) is the
        # picture while the game loads, centred on black: Ren'Py draws it into a window of its own the console never
        # shows.
        for files in (("presplash",), ("presplash_foreground", "presplash_background")):
            with tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                game = make_game(work / "Presplash")
                for name in files:
                    write_png(game / "game" / f"{name}.png", 400, 250)
                output = self.run_core("start", game, work / "data", env={"RENPY_HEADLESS_VERBOSE": "1"})
                self.assertIn("the presplash, 400x250, is the picture (1280x720)", output)
                self.assertNotIn("could not be shown", output)

    def test_the_memory_line(self):
        # Every few minutes (DULCAMARA_MEMORY_EVERY seconds; here, at each of Ren'Py's periodic calls) the log says what
        # the game holds; the heap and ps5-opengl's memory only on the console.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("play", make_game(work / "Memory"), work / "data",
                                   env={"RENPY_HEADLESS_VERBOSE": "1", "DULCAMARA_MEMORY_EVERY": "0"})
            self.assertRegex(output, r"memory: Ren'Py's textures \d+ \(\d+ MB\), its image cache \d+ MB")

    def test_a_game_at_44100_hz(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            self.play_and_check(make_game(work / "Slow sound", sound_rate=44100), work / "data")

    def test_a_press_of_the_pointer_clicks(self):
        # The touchpad is Ren'Py's mouse: the frontend reports a tap as a press of the pointer.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("click", make_game(work / "Click"), work / "data")
            self.assertIn("pointer: the dialogue moved on after a press", output)

    def test_fast_forward_skips_the_dialogue(self):
        # While the frontend fast-forwards, the core holds Ren'Py's skip (its Ctrl key).
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("skip", make_game(work / "Skip"), work / "data", env={"DULCAMARA_TEST_SKIP": "1"})
            self.assertIn("fast-forward: the dialogue went by without Cross", output)
            self.assertIn("DULCAMARA-TEST: choice=", output)

    def test_a_game_that_catches_its_quit(self):
        # A game whose own handler catches the quit the core raises in Ren'Py's thread: three seconds on, the core logs
        # where Ren'Py is and raises it again, and Ren'Py stops.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Holds on"), work / "data",
                                   env={"DULCAMARA_TEST_HOLD": "1"})
            self.assertIn("DULCAMARA-TEST: held on to a quit", output)
            self.assertIn("Ren'Py has not stopped; its thread is at:", output)

    def test_a_game_closed_with_its_thread_running(self):
        # A game's own thread (renpy.invoke_in_thread) still running as the game is closed: Ren'Py stops all the same
        # (Python 2.7, which could crash finalising under it, is then left as it is).
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Threaded"), work / "data",
                                   env={"DULCAMARA_TEST_THREAD": "1"})
            self.assertIn("stopped", output)
            self.assertEqual("still running" in output, self.SCRIPT.endswith("renpy7.sh"), output[-2000:])

    def test_an_error_opens_no_editor(self):
        # Ren'Py opens traceback.txt in an editor for an error that escapes it (a process of its own, which the console
        # cannot start): the core's error log tells it not to. RENPY_EDIT_PY's stand-in editor shows whether it would.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            probe = work / "probe.edit.py"
            probe.write_text(PROBE_EDITOR, encoding="utf-8")
            opened = work / "probe.edit.py.opened"
            env = {"RENPY_EDIT_PY": str(probe)}
            self.run_core("start", make_game(work / "Editor"), work / "data1",
                          env={**env, "DULCAMARA_TEST_REPORT": "editor"})
            self.assertTrue(opened.exists(), "the stand-in editor sees Ren'Py open an editor")
            opened.unlink()
            output = self.run_core("start", make_game(work / "Reported"), work / "data2",
                                   env={**env, "DULCAMARA_TEST_REPORT": "error"})
            self.assertIn("Ren'Py reported an error", output)
            self.assertFalse(opened.exists(), "Ren'Py opened an editor for the error")

    def test_fast_forward_skips_unread_text_when_set(self):
        # Settings > Ren'Py's "skip unread text too": the game's own preference (here, the default: unread text stops
        # the skip) reads as on.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("skip", make_game(work / "Skip all"), work / "data",
                                   env={"RENPY_HEADLESS_SKIP_UNSEEN": "on"})
            self.assertIn("fast-forward: the dialogue went by without Cross", output)

    def test_a_games_own_key_on_l2(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game, data = make_game(work / "Test Game"), work / "data"
            output = self.run_core("keys", game, data, env={"renpy_key_l2": "return"})
            self.assertIn("keys: the dialogue moved on after L2 (Return)", output)

    def test_the_cursor(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game, data = make_game(work / "Test Game"), work / "data"
            output = self.run_core("cursor", game, data)
            self.assertIn("cursor: drawn on the picture", output)
            self.assertIn("cursor: the dialogue moved on after a click with Cross", output)

    def test_a_name_typed(self):
        # A game asks for a name (renpy.input): the core tells the frontend it wants text and where its input is (here
        # at the foot of the picture, so the on-screen keyboard goes at the top), takes what the keyboard types through
        # its keyboard callback (characters, Backspace, Return), and tells it once Ren'Py has its text. Typing then
        # goes nowhere, and the controller is the game's again.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("text", make_game(work / "Name"), work / "data", env={"DULCAMARA_TEST_INPUT": "1"})
            self.assertRegex(output, r"text: the core asked for text, its input ([5-9]\d|100)-\d+ % down the picture")
            self.assertRegex(output, r"Ren'Py asks for text, its input \d+-\d+ % down the picture: the frontend's "
                                     r"keyboard opens")
            self.assertIn("DULCAMARA-TEST: name=Ada", output)
            self.assertIn("text: the game got the name typed (Ada)", output)
            self.assertIn("Ren'Py has its text: the frontend's keyboard closes", output)
            self.assertIn("text: a Return typed after the input ended went nowhere", output)
            self.assertIn("text: Cross moved the dialogue on afterwards", output)

    def test_a_game_with_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("error", make_game(work / "Broken", broken=True), work / "data")
            self.assertIn("Ren'Py reported an error", output)
            self.assertIn("broken.rpy", output)


class PS5LibcTest(unittest.TestCase):
    """The cores' gmtime_r, localtime_r and mbstowcs on the PS5 (src/core/renpy/ps5_time.h, ps5_utf8.h): the host
    build uses the build machine's own, so tests/ps5_libc_check.c checks the console's against them. Dulcamara 0.2.3
    crashed on a PS5 as Ren'Py opened its log: its gmtime_r left tm_zone NULL, which time.gmtime() hands to strlen."""

    def test_times_with_their_zones_and_utf8_text(self):
        cc = os.environ.get("CC", "clang")
        if not shutil.which(cc):
            self.skipTest(f"needs {cc}")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "ps5-libc-check"
            subprocess.run([cc, "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/ps5_libc_check.c"), "-o", str(executable)],
                           check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=120, check=False)
        self.assertEqual(result.returncode, 0, result.stdout[-3000:] + result.stderr[-3000:])
        self.assertTrue(result.stdout.startswith("ok "), result.stdout)
        print(f"the cores' gmtime_r, localtime_r and mbstowcs: {result.stdout.split()[-1]} checks passed")


if __name__ == "__main__":
    unittest.main()
