import importlib.util, os, subprocess, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('mark',ROOT/'tools/gen-watermark.py');mark=importlib.util.module_from_spec(spec);spec.loader.exec_module(mark)
class WatermarkTest(unittest.TestCase):
    def test_validation_and_identity(self):
        self.assertEqual(mark.configuration({}),'#define NP_WATERMARK_TEXT ""\n')
        self.assertIn('PRIVATE TEST / Tester / ui-v4',mark.configuration({'NP_PRIVATE_TEST':'1','NP_TEST_RECIPIENT':'Tester','NP_TEST_BUILD':'ui-v4'}))
        for bad in ('a"; system(x);','../x','x\n#define evil', 'x'*33,'日本語','$(whoami)'):
            with self.assertRaises(ValueError):mark.configuration({'NP_PRIVATE_TEST':'1','NP_TEST_RECIPIENT':bad,'NP_TEST_BUILD':'ui-v4'})
        with self.assertRaises(ValueError):mark.configuration({'NP_PRIVATE_TEST':'1'})
        with self.assertRaises(ValueError):mark.configuration({'NP_TEST_RECIPIENT':'Tester'})
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'watermark_config.h';cmd=['python3',str(ROOT/'tools/gen-watermark.py'),str(p)]
            env={k:v for k,v in os.environ.items() if not k.startswith('NP_TEST_') and k!='NP_PRIVATE_TEST'}
            subprocess.run(cmd,env=env,check=True);first=p.stat().st_mtime_ns
            subprocess.run(cmd,env=env,check=True);self.assertEqual(p.stat().st_mtime_ns,first)
            env.update(NP_PRIVATE_TEST='1',NP_TEST_BUILD='build-a');subprocess.run(cmd,env=env,check=True)
            self.assertIn('build-a',p.read_text());a=p.stat().st_mtime_ns
            env['NP_TEST_BUILD']='build-b';subprocess.run(cmd,env=env,check=True)
            self.assertNotEqual(p.stat().st_mtime_ns,a);self.assertIn('build-b',p.read_text())
