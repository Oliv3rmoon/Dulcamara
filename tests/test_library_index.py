"""Exercise real asynchronous library and cover workers with temporary games."""
import os
import platform
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ['-std=c11', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror', '-UNDEBUG', '-D_DEFAULT_SOURCE']

class LibraryIndexTest(unittest.TestCase):
    def test_index_and_worker_contracts(self):
        cc = os.environ.get('CC', 'clang')
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            executable = work / 'index-check'
            vendor = work / 'vendor.o'
            stb = work / 'stb.o'
            for source, target in [('src/vendor.c', vendor), ('src/stb_impl.c', stb)]:
                subprocess.run([cc, *FLAGS, '-w', '-I' + str(ROOT / 'third_party/stb'), '-c',
                                str(ROOT / source), '-o', str(target)], check=True)
            sources = ['tests/library_index_check.c', 'src/library.c', 'src/rpgmaker.c',
                       'src/util.c', 'src/storage.c', 'src/userdata.c', 'src/covers.c']
            sources += [s for s in ['src/library_index.c', 'src/library_view.c'] if (ROOT / s).exists()]
            sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
            libs = ['-liconv'] if platform.system() == 'Darwin' else []
            subprocess.run([cc, *FLAGS, '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'third_party/stb'),
                            '-I' + str(ROOT / 'third_party/easyrpg-player/builds/libretro/libretro-common/include'),
                            *[str(ROOT / s) for s in sources], str(vendor), str(stb), *sdl, *libs,
                            '-o', str(executable)], check=True)
            for case in ['scan', 'cache', 'view', 'unicode', 'nested-renpy', 'covers']:
                with self.subTest(case=case), tempfile.TemporaryDirectory() as data:
                    env = os.environ.copy()
                    if platform.system() == 'Darwin':
                        env['DYLD_LIBRARY_PATH'] = subprocess.check_output(
                            ['pkg-config', '--variable=libdir', 'sdl2'], text=True).strip()
                    result = subprocess.run([str(executable), data, case], capture_output=True, text=True, timeout=120, env=env)
                    print(result.stdout.strip())
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
