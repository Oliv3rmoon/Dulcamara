"""Production save/transaction operations on isolated files, including killed workers."""
import os
import importlib.util
import sys
import types
from unittest.mock import patch
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class SavesTest(unittest.TestCase):
    def run_checks(self, mode=None):
        with tempfile.TemporaryDirectory() as d:
            exe, obj = Path(d) / 'saves', Path(d) / 'saves.o'
            cc = [os.environ.get('CC', 'clang'), '-std=c11', '-D_DEFAULT_SOURCE', '-D_POSIX_C_SOURCE=200809L', '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror', '-pthread', '-I'+str(ROOT/'src')]
            # Interpose only the saves translation unit, never the test's real libc call.
            subprocess.run(cc + ['-Dreaddir=saves_test_readdir', '-Dunlink=saves_test_unlink', '-Dmkdir=saves_test_mkdir', '-Dfread=saves_test_fread', '-Drename=saves_test_rename', '-c', str(ROOT/'src/saves.c'), '-o', str(obj)], check=True)
            subprocess.run(cc + [str(ROOT/'tests/saves_check.c'), str(ROOT/'src/maintenance.c'), str(ROOT/'examples/self-update/self_update_sha256.c'), str(obj), '-o', str(exe)], check=True)
            args = [str(exe), str(Path(d).resolve())] + ([mode] if mode else [])
            if mode == 'repeated':
                base = Path(d).resolve()
                native = base / 'native'
                (native / 'nested').mkdir(parents=True)
                declarations = base / 'declarations.txt'
                records = []
                bridge = types.SimpleNamespace(save_path=lambda *entry: records.append(entry))
                renpy = types.SimpleNamespace(savegame_suffix='.custom', exports=types.SimpleNamespace(fsencode=lambda x: x))
                with patch.dict(sys.modules, {'_dulcamara': bridge, 'renpy': renpy}):
                    spec = importlib.util.spec_from_file_location('production_savepaths', ROOT/'src/core/renpy/savepaths.py')
                    helper = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(helper)
                    with declarations.open('w') as stream:
                        for process in range(40):
                            records.clear()
                            helper.slot(str(native), 'nested/explicit', '.%d.tmp' % (100000 + process))
                            stream.write(str(len(records)) + '\n')
                            for path, kind, scope, policy, suffix in records:
                                stream.write('%d %d %d\t%s\t%s\n' % (kind, scope, policy, path, suffix))
                args.append(str(declarations))
            subprocess.run(args, check=True, timeout=120)

    def test_repeated_native_temporary_suffixes(self):
        self.run_checks('repeated')

    def test_storage(self):
        self.run_checks()

    def test_descendant_scopes(self):
        self.run_checks('descendant')

    def test_directory_read_failures(self):
        self.run_checks('readdir')

    def test_recovery_directory_failure(self):
        self.run_checks('recovery')

def build_cli(directory):
    exe = Path(directory) / 'save-restore'
    subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', '-D_DEFAULT_SOURCE', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread', '-I'+str(ROOT/'src'), str(ROOT/'tests/saves_cli.c'), str(ROOT/'src/maintenance.c'), str(ROOT/'src/saves.c'), str(ROOT/'src/sha256.c'), '-o', str(exe)], check=True)
    return exe
