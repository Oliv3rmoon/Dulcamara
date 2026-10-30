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
import shutil
import struct
import subprocess
import tempfile
import unittest
import wave
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/renpy-host"
HEADLESS = BUILD / "core-check/renpy-headless"
UNIT = BUILD / "core-check/renpy-unit"
ASSETS = BUILD / "assets"
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


def make_game(directory, width=1280, height=720, sound_rate=48000, broken=False):
    """A copy of the test game in `directory`, with its picture, its sound and this run's screen size."""
    shutil.copytree(GAME, directory)
    game = directory / "game"
    (game / "images").mkdir(exist_ok=True)
    (game / "audio").mkdir(exist_ok=True)
    write_png(game / "images/dulcamara.png", 256, 256)
    write_tone(game / "audio/tone.wav")
    (game / "size.rpy").write_text(f"define config.screen_width = {width}\n"
                                   f"define config.screen_height = {height}\n"
                                   f"define config.sound_sample_rate = {sound_rate}\n", encoding="utf-8")
    if broken:
        # a line that is no Ren'Py statement: the script cannot be parsed
        (game / "broken.rpy").write_text("this is not a statement\n", encoding="utf-8")
    return directory


class RenpyCoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which(os.environ.get("CC", "cc")) or not shutil.which("cmake"):
            raise unittest.SkipTest("needs a C compiler and cmake")
        build = subprocess.run(["bash", str(ROOT / "tools/build-renpy.sh"), "host"], capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError("tools/build-renpy.sh host failed:\n" + build.stdout[-4000:] + build.stderr[-4000:])
        print(build.stdout.strip().splitlines()[-1])

    def run_core(self, mode, game, data, *more, env=None):
        result = subprocess.run([str(HEADLESS), str(ASSETS), mode, str(game), str(data), *map(str, more)],
                                capture_output=True, text=True, errors="replace", timeout=600, check=False,
                                env={**os.environ, **(env or {})})
        print(f"\n{mode} {game.name}:")
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout[-6000:] + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)
        return result.stdout

    def play_and_check(self, game, data):
        output = self.run_core("play", game, data)
        for line in ("XRGB8888", "input: the picture changed after Cross", "sound: playing",
                     "DULCAMARA-TEST: choice=right", "DULCAMARA-TEST: saved", "ended by the game"):
            self.assertIn(line, output)
        return output

    def test_the_core_checks(self):
        # under the sanitizers; leaks are not checked: the OpenGL driver's own allocations would be reported
        result = subprocess.run([str(UNIT)], capture_output=True, text=True, timeout=120, check=False,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
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

    def test_a_game_at_44100_hz(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            self.play_and_check(make_game(work / "Slow sound", sound_rate=44100), work / "data")

    def test_a_game_with_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("error", make_game(work / "Broken", broken=True), work / "data")
            self.assertIn("Ren'Py reported an error", output)
            self.assertIn("broken.rpy", output)


if __name__ == "__main__":
    unittest.main()
