"""Production text entry and controller model; sanitizer regression."""
import os, subprocess, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
class ControllerTest(unittest.TestCase):
    def test_controller(self):
        with tempfile.TemporaryDirectory() as d:
            exe = str(Path(d) / 'controller')
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(ROOT/'src'),str(ROOT/'tests/ui_controller_check.c'),str(ROOT/'src/ui/controller.c'),str(ROOT/'src/keyboard.c'),'-o',exe],check=True)
            r = subprocess.run([exe],capture_output=True,text=True,env={**os.environ,"TZ":"UTC"})
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            print(r.stdout)
