"""Deterministic first-poll terminal service results through the real backup panel."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class BackupPanelTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='backup-panel-')
        cls.exe = str(Path(cls.temp.name) / 'check')
        cmd = ['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-I' + str(ROOT / 'src'),
               '-I' + str(ROOT / 'third_party/easyrpg-player/builds/libretro/libretro-common/include')]
        cmd += [str(ROOT / p) for p in ['tests/backup_panel_check.c',
                'src/ui/backup_panel.c', 'src/ui/controller.c', 'src/keyboard.c', 'src/ui/story_model.c']]
        subprocess.run([*cmd, '-o', cls.exe], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_first_poll_terminal_results(self):
        # the same production state machine drawn as the shelf's list and as Story's save slots
        for scenario in ['details', 'create', 'restore', 'delete', 'list-error', 'details-error', 'restore-error', 'second-page']:
          for view in ('shelf', 'story'):
            with self.subTest(scenario=scenario, view=view):
                run = subprocess.run([self.exe, scenario, view], capture_output=True, text=True,
                                     env={**os.environ, 'TZ': 'UTC'})
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                print(run.stdout, end='')
