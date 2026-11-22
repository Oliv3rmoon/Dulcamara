import subprocess, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class EclipseTest(unittest.TestCase):
    def test_bounded_time_based_motion(self):
        with tempfile.TemporaryDirectory() as d:
            obj=Path(d)/'eclipse.o';exe=Path(d)/'eclipse'
            flags=['-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(ROOT/'src'),'-I'+str(ROOT/'third_party/stb')]
            subprocess.run(['clang',*flags,'-Dmalloc=tracked_malloc','-Dcalloc=tracked_calloc','-Dfree=tracked_free','-c',str(ROOT/'src/ui/eclipse.c'),'-o',str(obj)],check=True)
            subprocess.run(['clang',*flags,str(ROOT/'tests/eclipse_check.c'),str(obj),'-lm','-o',str(exe)],check=True)
            r=subprocess.run([str(exe)],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stdout+r.stderr);print(r.stdout)
