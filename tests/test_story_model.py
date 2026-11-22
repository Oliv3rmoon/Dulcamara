"""Story mode's production model (typing, honest lines, wrap, tint); sanitizer regression."""
import os, subprocess, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
class StoryModelTest(unittest.TestCase):
    def test_story_model(self):
        with tempfile.TemporaryDirectory() as d:
            exe = str(Path(d) / 'story_model')
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(ROOT/'src'),
                            str(ROOT/'tests/story_model_check.c'),str(ROOT/'src/ui/story_model.c'),'-o',exe],check=True)
            r = subprocess.run([exe],capture_output=True,text=True,env={**os.environ,"TZ":"UTC"})
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            print(r.stdout)
