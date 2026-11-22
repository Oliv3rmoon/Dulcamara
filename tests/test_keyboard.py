#!/usr/bin/env python3
# Dulcamara - the on-screen keyboard's keys, built and run on the host.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds tests/keyboard_check.c with src/keyboard.c (the keys of the on-screen keyboard a game's text entry gets: a
Ren'Py game's name) under the address and undefined-behaviour sanitizers, warnings as errors, and runs it: the keys
with Shift and without, the highlight's moves round the rows and over the wide keys, Shift's three states, each
button's and each key's press, the touchpad. CC names another compiler (default: clang)."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG"]


class KeyboardTest(unittest.TestCase):
    def test_the_keys(self):
        cc = os.environ.get("CC", "clang")
        if not shutil.which(cc):
            self.skipTest(f"needs {cc}")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "keyboard-check"
            subprocess.run([cc, *FLAGS, "-I" + str(ROOT / "src"), str(ROOT / "tests/keyboard_check.c"),
                            str(ROOT / "src/keyboard.c"), "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=120, check=False)
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
