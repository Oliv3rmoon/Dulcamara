"""Executable production updater, ordinary prompt, and insider consent (no NP_PREVIEW)."""
import json,os,subprocess,sys,tempfile,unittest,zipfile
from pathlib import Path
from test_update_storage import REQUIRED as STORAGE_REQUIRED,manifested
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import release_metadata
# The release offered: the next patch after the app's own NP_VERSION (2.0.0 -> 2.0.1), so it is always newer.
META=release_metadata.read(ROOT)
_major,_minor,_patch=(int(p) for p in META.version.split('.'))
OFFERED=f'{_major}.{_minor}.{_patch+1}'
REQUIRED=dict(STORAGE_REQUIRED)
REQUIRED['sce_sys/param.json']=json.dumps({'titleId':META.title_id,'contentVersion':release_metadata.content_version(OFFERED)},separators=(',',':')).encode()
class UpdateProductionTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
   cls.tmp=tempfile.TemporaryDirectory();base=Path(cls.tmp.name).resolve();cls.base=base
   cc=[os.environ.get('CC','clang'),'-std=c11','-D_DEFAULT_SOURCE','-g','-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'tests/host-sdl'),'-I'+str(ROOT/'src'),'-I'+str(ROOT/'src/ui')]
   objs=[]
   for name in ['miniz','miniz_tdef','miniz_tinfl','miniz_zip']:
    obj=base/(name+'.o');subprocess.run(cc+['-w','-DMINIZ_NO_ZLIB_COMPATIBLE_NAMES','-c',str(ROOT/f'third_party/miniz/{name}.c'),'-o',str(obj)],check=True);objs.append(str(obj))
   obj=base/'maintenance.o';subprocess.run(cc+['-Dunlink=update_test_unlink','-Drename=update_test_rename','-c',str(ROOT/'src/maintenance.c'),'-o',str(obj)],check=True);objs.append(str(obj))
   sources=['tests/update_production_check.c','src/update.c','src/update_storage.c','src/update_release.c','src/insider.c','src/insider_check.c','src/keyboard.c','src/i18n.c','src/util.c','src/ui/update_popup.c','src/ui/insider_prompt.c','examples/self-update/self_update_sha256.c']
   exe=base/'production';cls.exe=exe;subprocess.run(cc+['-Wno-unused-function',f'-DOFFERED_VERSION="{OFFERED}"',*[os.environ.get('TEST_UPDATE_POPUP',str(ROOT/p)) if p=='src/ui/update_popup.c' else str(ROOT/p) for p in sources],*objs,'-lm','-o',str(exe)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def run_scenario(self,mode):
  with tempfile.TemporaryDirectory(dir=self.base) as d:
   base=Path(d);app=base/'installed';app.mkdir();(app/'eboot.bin').write_bytes(b'old executable');zip_path=base/'offer.zip'
   (app/'untouched').write_bytes(b'user data')
   with zipfile.ZipFile(zip_path,'w',zipfile.ZIP_DEFLATED) as z:
    for p,b in manifested(REQUIRED).items():z.writestr('PPSA99010/'+p,b)
   r=subprocess.run([str(self.exe),str(base),str(zip_path),str(app),mode],capture_output=True,text=True,timeout=30)
   print(mode+': '+r.stdout.strip(),flush=True)
   self.assertEqual(r.returncode,0,r.stdout+r.stderr)
   self.assertEqual((app/'untouched').read_bytes(),b'user data')
   if mode=='apply-write':
    self.assertEqual((app/'eboot.bin').read_bytes(),b'old executable')
    self.assertEqual({p.name for p in app.iterdir() if p.is_file()},{'eboot.bin','untouched'})
   else:
    for p,b in REQUIRED.items():self.assertEqual((app/p).read_bytes(),b)
   self.assertFalse((app/'.eclipse-update').exists())
 def test_jobs_prompt_consent_and_first_still_frame(self):self.run_scenario('ok')
 def test_committed_cleanup_retry_reports_installed(self):self.run_scenario('cleanup-once')
 def test_committed_cleanup_pending_then_recovered(self):self.run_scenario('cleanup-always')
 def test_precommit_rollback_reports_retryable_failure(self):self.run_scenario('apply-write')
