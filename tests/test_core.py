#!/usr/bin/env python3
# Dulcamara - EasyRPG Player's core, built for this computer and run headless on EasyRPG's TestGame.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the libretro core the PS5 app links (tools/build-easyrpg.sh host: the same EasyRPG Player and liblcf
revisions, liblcf with the same iconv patch, the same options) with the headless frontend of tests/core, fetches
TestGame at its pinned commit (tools/fetch-testgame.sh) and runs it on the RPG Maker 2000 and 2003 test suites,
from their folders and from a .zip: each must load, give a picture every frame, change the picture, and go on
when Decision is pressed. Also checks liblcf's encodings without ICU (tests/core/encodings.cpp)."""

import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADLESS = ROOT / "build/easyrpg-host/core-check/easyrpg-headless"
ENCODINGS = ROOT / "build/easyrpg-host/core-check/lcf-encodings"
FRAMES = "900"


def run(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, text=True, **kwargs)


class CoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cmake"):
            raise unittest.SkipTest("needs cmake")
        cls.testgame = Path(run(["bash", str(ROOT / "tools/fetch-testgame.sh")]).stdout.strip().splitlines()[-1])
        build = subprocess.run(["bash", str(ROOT / "tools/build-easyrpg.sh"), "host"], capture_output=True,
                               text=True)
        if build.returncode != 0:
            raise AssertionError("tools/build-easyrpg.sh host failed:\n" + build.stdout[-4000:] + build.stderr[-4000:])
        print(build.stdout.strip().splitlines()[-1])

    def play(self, game, then=None):
        with tempfile.TemporaryDirectory() as system:
            result = subprocess.run([str(HEADLESS), str(game), system, FRAMES] + ([str(then)] if then else []),
                                    capture_output=True, text=True, errors="replace", timeout=300,
                                    check=False, env={**os.environ, "DULCAMARA_RUNTIME_PROBE": "1"})
        print(f"\n{game.relative_to(self.testgame.parent) if game.is_relative_to(self.testgame.parent) else game.name}:")
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)
        self.assertIn("XRGB8888", result.stdout)
        self.assertIn("runtime: initial checkpoint held native writes", result.stdout)
        self.assertIn("runtime: clean stop finished writers", result.stdout)
        expected_root = str(game).replace(".zip/", ".zip.save/")
        self.assertIn("native-save: 3 1 2 " + expected_root, result.stdout)
        self.assertIn("the picture changed after Decision", result.stdout)
        return result.stdout

    def test_native_save_restore_folder_and_archive(self):
        # Both RPG Maker fixtures: a native slot written, snapshotted, restored and loaded back by the engine.
        for version, archived in ((v, a) for v in ("2000", "2003") for a in (False, True)):
            with self.subTest(version=version, archive=archived), tempfile.TemporaryDirectory() as directory:
                work = Path(directory).resolve()
                source = self.testgame / ("TestGame-" + version)
                if archived:
                    archive = work / "game.zip"
                    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
                        for path in source.rglob("*"):
                            if path.is_file():
                                out.write(path, "inside/" + path.relative_to(source).as_posix())
                    game = archive / "inside"
                else:
                    game = work / "game"
                    shutil.copytree(source, game)
                result = subprocess.run([str(HEADLESS), str(game), str(work / "system"), FRAMES, str(game)],
                    capture_output=True, text=True, errors="replace", timeout=300,
                    env={**os.environ, "DULCAMARA_RUNTIME_PROBE": "1", "DULCAMARA_EASY_SAVE": "1",
                         "DULCAMARA_SAVE_STORE": str(work / "manager"), "DULCAMARA_SAVE_CORE": "0"})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
                self.assertIn("native slot restored and loaded with variable 999 = 4242", result.stdout)
                self.assertIn("PASS", result.stdout)

    def test_encodings_without_icu(self):
        result = subprocess.run([str(ENCODINGS)], capture_output=True, text=True, timeout=60, check=False)
        print("\nliblcf encodings:")
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)

    def test_rpg_maker_2000(self):
        self.play(self.testgame / "TestGame-2000")

    def test_rpg_maker_2003(self):
        self.play(self.testgame / "TestGame-2003")

    def test_a_second_game_in_the_same_process(self):
        # The app quits one game and starts the next without restarting: the core must start over cleanly.
        output = self.play(self.testgame / "TestGame-2000", then=self.testgame / "TestGame-2003")
        self.assertIn("second game: loaded and ran", output)

    def test_rpg_maker_2003_from_a_zip(self):
        # How the library starts a .zip game: the archive's path, then the folder inside it that holds the game.
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "TestGame-2003.zip"
            source = self.testgame / "TestGame-2003"
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
                for path in sorted(source.rglob("*")):
                    if path.is_file():
                        out.write(path, "TestGame-2003/" + path.relative_to(source).as_posix())
            self.play(archive / "TestGame-2003")


if __name__ == "__main__":
    unittest.main()
