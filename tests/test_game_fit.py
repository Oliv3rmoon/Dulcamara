#!/usr/bin/env python3
# Dulcamara - the game picture's place on screen, built and run on the host.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds tests/game_fit_check.c with src/platform/game_fit.h (plat_draw_game's size of the game picture) under the
address and undefined-behaviour sanitizers and runs it: Ren'Py's 1280x720, 1366x768, 1600x900, 1920x1080 and 800x600
pictures on the PS5's 3840x2160 output with integer scale on and off, and RPG Maker's 320x240. CC names another
compiler (default: clang)."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG"]


class GameFitTest(unittest.TestCase):
    def test_hd_pictures_fill_the_output_with_integer_scale(self):
        cc = os.environ.get("CC", "clang")
        if not shutil.which(cc):
            self.skipTest(f"needs {cc}")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "game-fit-check"
            subprocess.run([cc, *FLAGS, "-I" + str(ROOT / "src"), str(ROOT / "tests/game_fit_check.c"),
                            "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60, check=False)
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
