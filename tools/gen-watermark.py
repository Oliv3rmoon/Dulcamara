#!/usr/bin/env python3
"""Build-only label; header content changes invalidate dependent objects."""
import argparse, json, os, re
from pathlib import Path

def configuration(env):
    marked=env.get('NP_PRIVATE_TEST','0')
    if marked not in ('0','1'): raise ValueError('NP_PRIVATE_TEST must be 0 or 1')
    recipient=env.get('NP_TEST_RECIPIENT','')
    build=env.get('NP_TEST_BUILD','')
    if not re.fullmatch(r'[A-Za-z0-9 ._-]{0,32}',recipient): raise ValueError('recipient: at most 32 ASCII letters, digits, spaces, ._-')
    if not re.fullmatch(r'[A-Za-z0-9._-]{0,20}',build): raise ValueError('build ID: at most 20 ASCII letters, digits, ._-')
    if marked=='1' and not build: raise ValueError('marked builds require NP_TEST_BUILD')
    if marked=='0' and (recipient or build): raise ValueError('recipient/build requires NP_PRIVATE_TEST=1')
    text='PRIVATE TEST'+(' / '+recipient if recipient else '')+(' / '+build if build else '') if marked=='1' else ''
    return '#define NP_WATERMARK_TEXT '+json.dumps(text)+'\n'
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args()
    try: text=configuration(os.environ)
    except ValueError as e: p.error(str(e))
    path=Path(a.output);path.parent.mkdir(parents=True,exist_ok=True)
    if not path.exists() or path.read_text()!=text:
        tmp=path.with_suffix('.tmp');tmp.write_text(text);tmp.replace(path)
