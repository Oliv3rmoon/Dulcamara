"""Real synchronization/save protocol, sanitized."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class RuntimeTest(unittest.TestCase):
    def test_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'checkpoint'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror', '-pthread', '-I'+str(ROOT/'src'), str(ROOT/'tests/runtime_checkpoint_check.c'), str(ROOT/'src/core/renpy/handoff.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_host_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'host'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'src'), '-I'+str(ROOT/'third_party/easyrpg-player/builds/libretro/libretro-common/include'), str(ROOT/'tests/runtime_host_check.c'), str(ROOT/'src/core/host.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_transient_events(self):
        import shlex
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
        libdir = subprocess.check_output(['pkg-config', '--variable=libdir', 'sdl2'], text=True).strip()
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'input'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror', '-DSDL_MAIN_HANDLED', '-I'+str(ROOT/'src'), str(ROOT/'tests/runtime_input_check.c'), str(ROOT/'src/core/renpy/input.c'), '-o', str(binary), *flags], check=True)
            subprocess.run([str(binary)], check=True, timeout=30, env={**os.environ, 'DYLD_LIBRARY_PATH': libdir, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_restart_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'restart'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'src'), str(ROOT/'tests/runtime_restart_check.c'), str(ROOT/'src/restart.c'), str(ROOT/'src/storage.c'), str(ROOT/'src/util.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary), str(Path(directory)/'restart.txt')], check=True, timeout=30)
