#!/usr/bin/env python3
"""Generate the complete Eclipse update manifest AFTER all runtime/source files are assembled.
The manifest lists every regular file except itself: SHA256, decimal size, relative UTF-8 path.
It is authenticated by the outer release ZIP digest; it detects omitted/extra package members.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import hashlib, os, re, sys, tempfile
from pathlib import Path
HEADER=Path(__file__).resolve().parents[1]/'src/update_payload.h'
NAME='eclipse-payload.sha256'
MAGIC='DULCAMARA-ECLIPSE-PAYLOAD-1\n'
REQUIRED=re.findall(r'^    "([^"]+)",$',HEADER.read_text(),re.M)
def valid(name):
 return (len(name.encode())<900 and not name.startswith('.') and all(p not in ('','.','..') for p in name.split('/'))
         and not any(ord(c)<32 or ord(c)==127 or c in '\\:' for c in name) and '.eclipse-stage' not in name)
def generate(root):
 root=Path(root); records=[]; seen=set()
 for path in sorted(root.rglob('*')):
  name=path.relative_to(root).as_posix()
  if path.is_symlink(): raise ValueError('symlink in payload: '+name)
  if path.is_dir(): continue
  if not path.is_file(): raise ValueError('special file in payload: '+name)
  if name==NAME+'.tmp':raise ValueError('unfinished payload manifest temporary file: '+name)
  if name==NAME:continue
  if not valid(name) or name.lower() in seen:raise ValueError('invalid/aliased payload path: '+name)
  seen.add(name.lower());h=hashlib.sha256();size=0
  with path.open('rb') as f:
   for b in iter(lambda:f.read(1024*1024),b''): h.update(b);size+=len(b)
  records.append((name,size,h.hexdigest()))
 if len(records)>=65536:raise ValueError('too many payload files')
 for name in REQUIRED:
  if not any(n==name and size>0 for n,size,_ in records):raise ValueError('missing required payload member: '+name)
 text=MAGIC+''.join(f'{digest} {size} {name}\n' for name,size,digest in records)
 if len(text.encode())>16*1024*1024:raise ValueError('payload manifest exceeds limit')
 output=root/NAME
 fd,temporary=tempfile.mkstemp(prefix='.eclipse-payload-',dir=root.parent)
 temp=Path(temporary)
 try:
  with os.fdopen(fd,'wb') as f:f.write(text.encode());f.flush();os.fsync(f.fileno())
  temp.replace(output)
  parent=os.open(root,os.O_RDONLY)
  try:os.fsync(parent)
  finally:os.close(parent)
 finally:
  if temp.exists():temp.unlink()

 return output
if __name__=='__main__':
 if len(sys.argv)!=2:raise SystemExit('usage: update_payload.py <assembled PPSA99010 directory>')
 print(generate(sys.argv[1]))
