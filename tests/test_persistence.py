#!/usr/bin/env python3
"""Compile real persistence modules with ASan/UBSan; use only temporary filesystem state."""
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


class PersistenceTest(unittest.TestCase):
    def test_persistence_contracts(self):
        cc = os.environ.get("CC", "clang")
        if not shutil.which(cc):
            self.skipTest(f"needs {cc}")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "persistence-check"
            sources = ["tests/persistence_check.c", "src/config.c", "src/util.c", "src/storage.c",
                       "src/library.c", "src/rpgmaker.c", "src/profile.c", "src/userdata.c"]
            miniz = Path(directory) / "miniz.o"
            subprocess.run([cc, *FLAGS, "-w", "-c", str(ROOT / "src/vendor.c"), "-o", str(miniz)], check=True)
            libs = ["-liconv"] if platform.system() == "Darwin" else []
            subprocess.run([cc, *FLAGS, "-I" + str(ROOT / "src"),
                            "-I" + str(ROOT / "third_party/easyrpg-player/builds/libretro/libretro-common/include"),
                            *[str(ROOT / s) for s in sources], str(miniz), *libs, "-o", str(executable)], check=True)
            for case in ["storage", "portable", "history", "profiles", "metadata", "selection"]:
                with self.subTest(case=case), tempfile.TemporaryDirectory() as data:
                    result = subprocess.run([str(executable), data, case], capture_output=True, text=True, timeout=120)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
