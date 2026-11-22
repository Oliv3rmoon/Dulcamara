"""Real snapshot publication durability, with inode-selected fsync faults."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class SnapshotPublicationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        base = Path(cls.tmp.name)
        cls.exe = base / 'probe'
        cc = [os.environ.get('CC', 'clang'), '-std=c11', '-D_DEFAULT_SOURCE', '-D_POSIX_C_SOURCE=200809L', '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror', '-pthread', '-I'+str(ROOT/'src')]
        subprocess.run(cc + ['-Dfsync=probe_fsync', '-c', str(ROOT/'src/maintenance.c'), '-o', str(base/'maintenance.o')], check=True)
        subprocess.run(cc + [str(ROOT/'tests/snapshot_publish_check.c'), str(ROOT/'src/saves.c'), str(ROOT/'src/sha256.c'), str(base/'maintenance.o'), '-o', str(cls.exe)], check=True)
    def check_mode(self, mode):
        with tempfile.TemporaryDirectory() as root:
            subprocess.run([str(self.exe), str(Path(root).resolve()), mode], check=True)
    def test_transient_failure_completes_durability(self):
        self.check_mode('transient')
    def test_persistent_failure_never_certifies_protection(self):
        self.check_mode('persistent')

    def test_retry_enforces_five_auto_retention_and_keeps_manual(self):
        self.check_mode('retention')
