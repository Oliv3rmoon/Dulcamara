#!/usr/bin/env python3
"""Real GL regression for Ren'Py's CPU premultiplication of unpack-buffer uploads.

The nonzero-offset case catches interpreting a GL buffer offset as a CPU pointer.
The zero-offset control and literal destination pixels establish the working GL
context and exercise the production wrapper with the actual CGL/EGL backend.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RenpyTextureUpload(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="dulcamara-texture-upload-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "texture-upload"
        backend = "gl_cgl.c" if sys.platform == "darwin" else "gl_egl.c"
        libs = ["-framework", "OpenGL"] if sys.platform == "darwin" else ["-lEGL", "-ldl"]
        source = ROOT / "src/core/renpy"
        command = shlex.split(os.environ.get("CC", "clang")) + [
            "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-g", "-O1",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(source), str(ROOT / "tests/core/renpy_texture_upload.c"),
            *[str(source / name) for name in ("gl.c", backend, "frames.c", "log.c")],
            *libs, "-pthread", "-o", str(cls.binary),
        ]
        subprocess.run(command, check=True, capture_output=True, text=True)

    def check_offset(self, offset, mode="offset"):
        result = subprocess.run(
            [str(self.binary), str(offset), mode], capture_output=True, text=True, timeout=30,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
        )
        if result.returncode == 77:
            self.skipTest(result.stderr.strip())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"PASS: real GL upload and premultiplied copy at unpack offset {offset}", result.stdout)

    def test_zero_unpack_offset_control(self):
        self.check_offset(0)

    def test_nonzero_unpack_offset_is_not_a_cpu_address(self):
        for offset in (4, 16, 48):
            with self.subTest(offset=offset):
                self.check_offset(offset)

    def test_unpack_buffer_keeps_pixels_when_client_storage_is_reused(self):
        self.check_offset(0, "buffer-reuse")

    def test_direct_upload_keeps_pixels_when_client_storage_is_reused(self):
        self.check_offset(0, "client-reuse")

    def test_unpack_upload_uses_current_buffer_contents(self):
        self.check_offset(0, "buffer-subdata")

    def test_unpack_upload_honours_aligned_row_stride(self):
        self.check_offset(0, "aligned-rows")


if __name__ == "__main__":
    unittest.main()
