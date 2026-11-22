# Dulcamara - the kernel library the app's imports are bound to must not depend on the builder's locale.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Candidates ecl200-n1 to n5 were linked on a Mac whose shell sorted the SDK's stub libraries without regard to
# case: libkernel_stub_weak.so, whose module name is libkernel_web, came before libkernel.so and the linker bound
# all 233 kernel imports to libkernel_web. Every build proven on a console binds them to libkernel; n5, the first
# of the five to reach one, stopped at launch (CE-108255-1) before it could write a log.

import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUBS = ["libSceNet.so", "libSceLibcInternal.so", "libc_stub_weak.so", "libkernel.so", "libkernel_stub_weak.so",
         "libkernel_sys.so", "libkernel_web.so", "libmonosgen-2.0.so"]


class StubOrderTests(unittest.TestCase):
    def order(self, **env):
        with tempfile.TemporaryDirectory(prefix="stub order ") as directory:
            for name in STUBS:
                (Path(directory) / name).write_bytes(b"")
            (Path(directory) / "libc.a").write_bytes(b"")
            (Path(directory) / "._libkernel.so").write_bytes(b"")  # what a Mac's copy can leave beside a file
            (Path(directory) / "old.so").mkdir()
            result = subprocess.run(
                ["bash", "-c", 'source "$1/tools/sdk-stubs.sh"; sdk_stub_libraries "$2"', "test", str(ROOT), directory],
                env={**{k: v for k, v in os.environ.items() if not k.startswith("LC_") and k != "LANG"}, **env},
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return [Path(line).name for line in result.stdout.splitlines()]

    def test_byte_order_in_every_locale(self):
        expected = sorted(STUBS)  # Python's is byte order for these names
        self.assertLess(expected.index("libkernel.so"), expected.index("libkernel_stub_weak.so"))
        for env in ({}, {"LC_ALL": "C"}, {"LC_ALL": "en_US.UTF-8"}, {"LANG": "en_US.UTF-8"},
                    {"LC_COLLATE": "en_US.UTF-8"}):
            with self.subTest(env=env):
                self.assertEqual(self.order(**env), expected)

    def test_build_script_links_the_stubs_only_in_that_order(self):
        text = (ROOT / "tools/build.sh").read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"target/lib/?\"?/?\*\.so", text),
                          "build.sh must take the SDK's stubs from sdk_stub_libraries, never from a glob")
        self.assertNotRegex(text, r"\| sort -z", "build.sh's sorted lists are in byte order (LC_ALL=C sort)")
        self.assertIn("sdk_stub_libraries", text)


class ImportGateTests(unittest.TestCase):
    def check(self, *modules):
        with tempfile.TemporaryDirectory() as directory:
            eboot = Path(directory) / "eboot.bin"
            eboot.write_bytes(b"\0".join(name.encode() + b".prx\0" + name.encode() for name in modules) + b"\0")
            return subprocess.run([sys.executable, str(ROOT / "tools/check-imports.py"), str(eboot)],
                                  capture_output=True, text=True)

    def test_a_title_bound_to_libkernel_passes(self):
        result = self.check("libSceVideoOut", "libSceLibcInternal", "libkernel", "libkernel_sys")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_imports_bound_to_libkernel_web_fail(self):
        result = self.check("libSceVideoOut", "libSceLibcInternal", "libkernel_web", "libkernel_sys")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libkernel_web", result.stderr)

    def test_no_libkernel_fails(self):
        result = self.check("libSceVideoOut", "libSceLibcInternal", "libkernel_sys")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libkernel", result.stderr)


class WeakImportGateTests(unittest.TestCase):
    def check(self, nm_output):
        with tempfile.TemporaryDirectory(prefix="weak gate ") as directory:
            nm = Path(directory) / "nm"
            nm.write_text("#!/bin/sh\ncat <<'END'\n" + nm_output + "END\n", encoding="utf-8")
            nm.chmod(0o755)
            return subprocess.run(["bash", str(ROOT / "tools/check-weak-imports.sh"), str(nm), "app.elf"],
                                  capture_output=True, text=True)

    def test_strong_imports_pass(self):
        result = self.check("                 U fclose\n                 U sceKernelUsleep\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_weak_import_fails_and_is_named(self):
        result = self.check("                 w fclose\n                 U fflush\n                 v environ\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("fclose", result.stderr)
        self.assertIn("environ", result.stderr)
        self.assertNotIn("fflush", result.stderr)

    def test_the_build_runs_the_gate_on_the_linked_app(self):
        text = (ROOT / "tools/build.sh").read_text(encoding="utf-8")
        self.assertRegex(text, r"check-weak-imports\.sh\" .*llvm-pie\.elf")


if __name__ == "__main__":
    unittest.main()
