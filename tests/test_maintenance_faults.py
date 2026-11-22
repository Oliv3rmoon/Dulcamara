"""Confirmed shared transaction faults and composed real save-recovery worker contract."""
from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class MaintenanceFaultsTest(unittest.TestCase):
 def test_faults_and_barrier(self):
  with tempfile.TemporaryDirectory() as d:
   base=Path(d).resolve();exe=base/'probe'
   cc=[os.environ.get('CC','clang'),'-std=c11','-D_DEFAULT_SOURCE','-g','-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'src')]
   obj=base/'maintenance.o'
   subprocess.run(cc+['-Dmkdir=probe_test_mkdir','-Dopen=probe_test_open','-Dunlink=probe_test_unlink','-c',str(ROOT/'src/maintenance.c'),'-o',str(obj)],check=True)
   subprocess.run(cc+[*[str(ROOT/p) for p in ['tests/maintenance_faults_check.c','src/saves.c','examples/self-update/self_update_sha256.c']],str(obj),'-o',str(exe)],check=True)
   for case in ['collision','collision-reverse','collision-unicode','collision-unicode-reverse','collision-long-s','collision-long-s-reverse','missing','barrier','probe-empty','probe-filled','probe-cleanup','probe-recovery','collision-long-s-plan-removed','collision-long-s-journal-removed']:
    r=subprocess.run([str(exe),str(base/case),case],capture_output=True,text=True,timeout=30)
    self.assertEqual(r.returncode,0,r.stdout+r.stderr);print(r.stdout.strip())
