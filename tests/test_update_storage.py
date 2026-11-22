"""Actual staged installer, faulted libc boundaries, and process termination in owned trees."""
import os, signal, subprocess, tempfile, unittest, zipfile, time, hashlib, re, struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ANCHORS=re.findall(r'^    "([^"]+)",$',(ROOT/'src/update_payload.h').read_text(),re.M)
REQUIRED={p:b'fixture '+p.encode() for p in ANCHORS}
REQUIRED['eboot.bin']=b'new executable'
REQUIRED['sce_sys/param.json']=b'{"titleId":"PPSA99010","contentVersion":"02.000.000"}'
def manifested(entries):
 entries=dict(entries)
 entries['eclipse-payload.sha256']=('DULCAMARA-ECLIPSE-PAYLOAD-1\n'+''.join(f'{hashlib.sha256(b).hexdigest()} {len(b)} {p}\n' for p,b in entries.items())).encode()
 return entries

class UpdateStorageTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory(); cls.build=Path(cls.tmp.name); cls.exe=cls.build/'installer'
  cc=[os.environ.get('CC','clang'),'-std=c11','-D_DEFAULT_SOURCE','-g','-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'src')]
  objs=[]
  for src,defs in [('src/maintenance.c',['-Dstatvfs=update_test_statvfs','-Drename=update_test_rename','-Dfsync=update_test_fsync','-Dunlink=update_test_unlink']),('src/update_storage.c',['-Dwrite=update_test_write','-Dreaddir=update_test_readdir'])]:
   obj=cls.build/(Path(src).stem+'.o');subprocess.run(cc+defs+['-c',str(ROOT/src),'-o',str(obj)],check=True);objs.append(str(obj))
  for name in ['miniz','miniz_tdef','miniz_tinfl','miniz_zip']:
   obj=cls.build/(name+'.o');subprocess.run(cc+['-w','-DMINIZ_NO_ZLIB_COMPATIBLE_NAMES','-c',str(ROOT/f'third_party/miniz/{name}.c'),'-o',str(obj)],check=True);objs.append(str(obj))
  subprocess.run(cc+[str(ROOT/'tests/update_storage_check.c'),str(ROOT/'examples/self-update/self_update_sha256.c'),str(ROOT/'src/update_release.c'),*objs,'-o',str(cls.exe)],check=True)
 @classmethod
 def tearDownClass(cls): cls.tmp.cleanup()
 def setUp(self):
  self.tmpcase=tempfile.TemporaryDirectory(dir=self.build);self.base=Path(self.tmpcase.name);self.app=self.base/'installed';self.app.mkdir();self.zip=self.base/'update.zip'
  (self.app/'eboot.bin').write_bytes(b'old executable');(self.app/'untouched').write_bytes(b'user data')
 def tearDown(self): self.tmpcase.cleanup()
 def payload(self,entries=None):
  with zipfile.ZipFile(self.zip,'w',zipfile.ZIP_DEFLATED) as z:
   for p,b in manifested(entries or REQUIRED).items(): z.writestr('PPSA99010/'+p,b)
 def run_install(self,mode='ok',success=True):
  r=subprocess.run([str(self.exe),str(self.app),str(self.zip),mode],text=True,capture_output=True,timeout=30)
  self.assertEqual(r.returncode,0 if success else 1,r.stdout+r.stderr);self.assertEqual((self.app/'untouched').read_bytes(),b'user data');return r.stdout
 def test_success_and_executable_last(self):
  self.payload();out=self.run_install();self.assertIn('ready 0 restart_ready 1',out)
  for p,b in REQUIRED.items():self.assertEqual((self.app/p).read_bytes(),b)
  self.assertFalse((self.app/'.eclipse-update').exists())
 def test_invalid_incomplete_and_alias_payloads(self):
  cases=[{'eboot.bin':b'only executable'},dict(REQUIRED,**{'../escape':b'x'}),dict(REQUIRED,**{'eboot.bin.eclipse-stage':b'x'}),dict(REQUIRED,**{'EBOOT.BIN':b'x'}),dict(REQUIRED,**{'eboot.bin/child':b'x'})]
  for entries in cases:
   self.payload(entries);self.run_install(success=False);self.assertEqual((self.app/'eboot.bin').read_bytes(),b'old executable')
 def test_identity_must_match_numeric_offer_and_title(self):
  for param in [b'{"titleId":"PPSA99999","contentVersion":"02.000.000"}',b'{"titleId":"PPSA99010","contentVersion":"00.002.013"}',b'{"titleId":"PPSA99010","contentVersion":"02.000.000","titleId":"PPSA99010"}']:
   self.payload(dict(REQUIRED,**{'sce_sys/param.json':param}));self.run_install(success=False)
   self.assertEqual((self.app/'eboot.bin').read_bytes(),b'old executable')
 def test_manifest_completeness_digests_and_records(self):
  valid=manifested(REQUIRED)
  cases=[]
  absent=dict(valid);del absent['assets/renpy8/renpy/common/00start.rpy'];cases.append(absent)
  absent=dict(valid);del absent['eclipse-payload.sha256'];cases.append(absent)
  extra=dict(valid);extra['extra.bin']=b'extra';cases.append(extra)
  changed=dict(valid);changed['assets/renpy7/dulcamara_boot.py']=b'X'*len(changed['assets/renpy7/dulcamara_boot.py']);cases.append(changed)
  duplicate=dict(valid);duplicate['eclipse-payload.sha256']+=duplicate['eclipse-payload.sha256'].splitlines(keepends=True)[1];cases.append(duplicate)
  unknown=dict(valid);unknown['eclipse-payload.sha256']+=b'0'*64+b' 1 unknown\n';cases.append(unknown)
  for entries in cases:
   with zipfile.ZipFile(self.zip,'w',zipfile.ZIP_DEFLATED) as z:
    for p,b in entries.items():z.writestr('PPSA99010/'+p,b)
   self.run_install(success=False);self.assertEqual((self.app/'eboot.bin').read_bytes(),b'old executable')
 def test_source_sized_expansion_admitted_only_with_rollback_space(self):
  # A small fixture claims a 910 MiB source member. We constrain actual statvfs
  # availability to 1 GiB, so the production preflight must reject before extraction.
  name='source/CorrespondingSource.tar.gz';data=dict(REQUIRED,**{name:b'source'})
  entries=manifested(data);claimed=910*1024*1024
  entries['eclipse-payload.sha256']=entries['eclipse-payload.sha256'].replace(b' 6 '+name.encode()+b'\n',f' {claimed} {name}\n'.encode())
  with zipfile.ZipFile(self.zip,'w') as z:
   for p,b in entries.items():z.writestr('PPSA99010/'+p,b)
  raw=bytearray(self.zip.read_bytes());full=('PPSA99010/'+name).encode()
  for sig,name_offset,size_offset in [(b'PK\x03\x04',30,22),(b'PK\x01\x02',46,24)]:
   at=0
   while True:
    at=raw.find(sig,at)
    if at<0:self.fail('missing archive header')
    if raw[at+name_offset:at+name_offset+len(full)]==full:struct.pack_into('<I',raw,at+size_offset,claimed);break
    at+=4
  self.zip.write_bytes(raw)
  out=self.run_install('space-large',False);self.assertIn('Not enough space for update staging and rollback',out)
  self.assertEqual((self.app/'eboot.bin').read_bytes(),b'old executable')
 def test_truncation_crc_and_duplicate_entries(self):
  self.payload();raw=self.zip.read_bytes();self.zip.write_bytes(raw[:-20]);self.run_install(success=False)
  self.payload()
  with self.assertWarnsRegex(UserWarning, r"Duplicate name: 'PPSA99010/eboot.bin'"):
   with zipfile.ZipFile(self.zip,'a') as z:z.writestr('PPSA99010/eboot.bin',b'duplicate')
  self.run_install(success=False)
  self.payload();raw=bytearray(self.zip.read_bytes());at=raw.find(b'PK\x03\x04');length,extra=struct.unpack_from('<HH',raw,at+26);raw[at+30+length+extra]^=0x7f;self.zip.write_bytes(raw);self.run_install(success=False)
 def test_generator_rejects_interrupted_old_temporary(self):
  import importlib.util
  spec=importlib.util.spec_from_file_location('payload',ROOT/'tools/update_payload.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
  tree=self.base/'package';tree.mkdir()
  for p,b in REQUIRED.items():
   target=tree/p;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(b)
  stale=tree/(module.NAME+'.tmp');stale.write_bytes(b'interrupted')
  with self.assertRaises(ValueError):module.generate(tree)
  self.assertEqual(stale.read_bytes(),b'interrupted');self.assertFalse((tree/module.NAME).exists())
  stale.unlink();module.generate(tree);module.generate(tree)
  self.assertEqual(set(p.relative_to(tree).as_posix() for p in tree.rglob('*') if p.is_file()),set(REQUIRED)|{module.NAME})
 def test_generator_binds_source_archive_and_every_member(self):
  import importlib.util
  spec=importlib.util.spec_from_file_location('payload',ROOT/'tools/update_payload.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
  tree=self.base/'package';tree.mkdir()
  for p,b in dict(REQUIRED,**{'source/CorrespondingSource.tar.gz':b'source archive'}).items():
   target=tree/p;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(b)
  manifest=module.generate(tree).read_bytes()
  self.assertIn(b'source/CorrespondingSource.tar.gz',manifest)
  with zipfile.ZipFile(self.zip,'w') as z:
   for p in tree.rglob('*'):
    if p.is_file():z.write(p,'PPSA99010/'+p.relative_to(tree).as_posix())
  self.run_install()
 def test_space_write_and_writer_exclusion(self):
  for mode in ['space','write','apply-write','runtime','restore']:
   self.payload();self.run_install(mode,False);self.assertEqual((self.app/'eboot.bin').read_bytes(),b'old executable')
 def test_recovery_read_and_final_directory_sync_failure(self):
  work=self.app/'.eclipse-update';(work/'payload').mkdir(parents=True);(work/'payload/0').write_bytes(b'partial stage')
  out=self.run_install('recover-readdir',False);self.assertIn('ready 0 restart_ready 0',out);self.assertTrue(work.exists())
  out=self.run_install('recover-sync',False);self.assertIn('ready 0 restart_ready 0',out);self.assertFalse(work.exists())
  # Missing workspace must still sync its parent on retry; continued EIO cannot unlock.
  out=self.run_install('recover-sync-always',False);self.assertIn('ready 0 restart_ready 0',out)
  out=self.run_install('recover');self.assertIn('ready 1 restart_ready 1',out)
 def test_committed_cleanup_retry_preserves_installed_outcome(self):
  self.payload();out=self.run_install('cleanup-once')
  self.assertIn('cleanup failures 1 launch locked',out)
  self.assertIn('result 1 ready 0 restart_ready 1',out)
  for p,b in REQUIRED.items():self.assertEqual((self.app/p).read_bytes(),b)
  self.assertFalse((self.app/'.eclipse-update').exists())
 def test_committed_cleanup_pending_is_not_rollback(self):
  self.payload();out=self.run_install('cleanup-always',False)
  self.assertIn('cleanup failures 2 launch locked',out)
  self.assertIn('result 0 ready 0 restart_ready 0',out)
  self.assertIn('Update recovery pending',out);self.assertNotIn('original installation restored',out)
  for p,b in REQUIRED.items():self.assertEqual((self.app/p).read_bytes(),b)
  self.assertTrue((self.app/'.eclipse-update/transaction/journal').exists())
  out=self.run_install('recover');self.assertIn('ready 0 restart_ready 1 restart 1',out)
  self.assertFalse((self.app/'.eclipse-update').exists())
 def test_precommit_rollback_remains_retryable(self):
  self.payload();out=self.run_install('apply-write',False)
  self.assertIn('result 0 ready 1 restart_ready 1',out)
  self.assertIn('original installation restored. Try again',out)
  self.assertEqual((self.app/'eboot.bin').read_bytes(),b'old executable')
  self.assertFalse((self.app/'.eclipse-update').exists())
 def test_interrupted_staging_apply_and_committed_cleanup(self):
  for mode in ['stage','apply','commit']:
   (self.app/'eboot.bin').write_bytes(b'old executable');self.payload()
   with (self.base/'child.log').open('w') as log:
    p=subprocess.Popen([str(self.exe),str(self.app),str(self.zip),mode],stdout=log,stderr=log)
    deadline=time.time()+15
    while time.time()<deadline:
     pid,status=os.waitpid(p.pid,os.WNOHANG|os.WUNTRACED)
     if pid: break
     time.sleep(.01)
    else:p.kill();p.wait();self.fail('installer did not reach boundary')
    self.assertTrue(os.WIFSTOPPED(status),(self.base/'child.log').read_text())
    if mode=='apply':self.assertEqual((self.app/'eboot.bin').read_bytes(),b'old executable')
    p.kill();p.wait()
   out=self.run_install('recover');self.assertIn('ready 0 restart_ready 1 restart 1',out)
   self.assertEqual((self.app/'eboot.bin').read_bytes(),b'new executable' if mode=='commit' else b'old executable')
   out=self.run_install('recover');self.assertIn('ready 1 restart_ready 1 restart 0',out)
if __name__=='__main__':unittest.main()
