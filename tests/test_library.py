#!/usr/bin/env python3
# Dulcamara - the library's game detection, built and run on the host.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds tests/library_check.c with src/library.c, src/rpgmaker.c and miniz under the address and
undefined-behaviour sanitizers, warnings as errors, and runs it: a games folder of made-up games (folders, names in
any case, a folder of games, .zip and .easyrpg archives, titles in windows-1252, Shift-JIS and windows-1251, title
pictures named by the database, XYZ pictures, things that are not games), Ren'Py's games, the library's sections
(RPG Maker's games, then Ren'Py's, each in its own order), then EasyRPG's TestGame when tools/fetch-testgame.sh can
fetch it. CC names another compiler (default: clang)."""

import os
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", "-D_DEFAULT_SOURCE"]
LIBRETRO = "third_party/easyrpg-player/builds/libretro/libretro-common/include"
SOURCES = ["tests/library_check.c", "src/library.c", "src/rpgmaker.c", "src/util.c"]


class LibraryTest(unittest.TestCase):
    def test_game_detection(self):
        cc = os.environ.get("CC", "clang")
        if not shutil.which(cc):
            self.skipTest(f"needs {cc}")
        testgame = []
        fetched = subprocess.run(["bash", str(ROOT / "tools/fetch-testgame.sh")], capture_output=True, text=True)
        if fetched.returncode == 0:
            testgame = [fetched.stdout.strip().splitlines()[-1]]
        else:
            print("TestGame not fetched: the made-up games only")
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            executable = work / "library-check"
            # miniz the way the app compiles it (src/vendor.c), without its own warnings
            miniz = work / "miniz.o"
            subprocess.run([cc, "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-w", "-c",
                            str(ROOT / "src/vendor.c"), "-o", str(miniz)], check=True)
            libs = ["-liconv"] if platform.system() == "Darwin" else []
            subprocess.run([cc, *FLAGS, "-I" + str(ROOT / "src"), "-I" + str(ROOT / LIBRETRO),
                            *[str(ROOT / s) for s in SOURCES], str(miniz),
                            *libs, "-o", str(executable)], check=True)
            data = work / "data"
            data.mkdir()
            result = subprocess.run([str(executable), str(data), *testgame], capture_output=True, text=True,
                                    errors="replace", timeout=120, check=False)
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)

    def test_the_app_is_dulcamara(self):
        header = (ROOT / "src/nightshade.h").read_text(encoding="utf-8")
        self.assertIn('#define NP_TITLE "Dulcamara"', header)
        self.assertIn('#define NP_TITLE_ID "PPSA99010"', header)
        self.assertIn('#define NP_DATA "/data/dulcamara"', header)

    def test_a_sandboxed_start_says_why_there_are_no_games(self):
        # The tester's "it sometimes shows 0 games when there are games": her log had
        # "storage: sandboxed (no unlock service answered ...)" and then "library: 0 game(s)" at every Look again.
        # A sandboxed app can't list folders, and /data is unlocked only first thing in main(), so Look again could
        # never find them. Now the library says why, and Square starts the app again, which asks again.
        shelf = (ROOT / "src/ui/shelf.c").read_text(encoding="utf-8")
        self.assertIn("if (app.sandboxed)\n    {\n        draw_sandboxed();", shelf)
        self.assertIn("Dulcamara can't see your games", shelf)
        self.assertIn("(pressed & BIT(BTN_SQUARE)) && app.sandboxed && app.library.count == 0", shelf)
        self.assertIn("app_unlock_again();", shelf)
        self.assertIn('"Start again"', shelf)
        main = (ROOT / "src/main.c").read_text(encoding="utf-8")
        unlock = main[main.index("void app_unlock_again(void)"):]
        unlock = unlock[:unlock.index("\n}\n")]
        self.assertIn("start_again();", unlock)  # the PS5 starts again: the unlock is asked before any thread
        self.assertIn("sandboxed, so its folders can't be listed", main)
        # the unlock itself stays where it was: first thing in main(), before SDL starts a thread
        self.assertLess(main.index("app.sandboxed = !plat_prepare_storage("), main.index("if (!plat_init())"))


if __name__ == "__main__":
    unittest.main()
