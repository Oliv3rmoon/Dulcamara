#!/usr/bin/env python3
"""Dulcamara's release identity, read from its one source, src/nightshade.h.

    python3 tools/release_metadata.py                      # print the identity as JSON
    python3 tools/release_metadata.py --check PARAM.json   # exit 1 unless PARAM agrees with it (tools/build.sh)
    python3 tools/release_metadata.py --sync PARAM.json    # write titleName and contentVersion into PARAM

The display title is NP_NAME + " " + NP_CODENAME ("Dulcamara Eclipse"); the PS5 contentVersion is NP_VERSION as
NN.NNN.NNN (2.0.0 -> 02.000.000). NP_VERSION is numeric X.Y.Z; a private test build's identifier is separate
metadata (tools/gen-watermark.py). Every 2.x release keeps the codename Eclipse.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# The codename of each major version: every 2.x is Eclipse.
CODENAMES = {2: "Eclipse"}


@dataclass(frozen=True)
class Metadata:
    name: str
    codename: str
    version: str
    title_id: str
    data: str
    display_title: str
    content_version: str


def content_version(version):
    m = re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", version)
    if not m:
        raise ValueError(f'version "{version}" is not numeric X.Y.Z')
    major, minor, patch = (int(g) for g in m.groups())
    if major > 99 or minor > 999 or patch > 999:
        raise ValueError(f'version "{version}" does not fit contentVersion (00-99.000-999.000-999)')
    return f"{major:02d}.{minor:03d}.{patch:03d}"


def check_codename(version, codename):
    major = int(version.split(".", 1)[0])
    expected = CODENAMES.get(major)
    if expected is not None and codename != expected:
        raise ValueError(f"every {major}.x release is {expected}, not {codename}")


def _define(text, name):
    m = re.search(r'^#define\s+' + name + r'\s+"([^"]*)"', text, re.M)
    if not m:
        raise ValueError(f"{name} not found in src/nightshade.h")
    return m.group(1)


def read(root=ROOT):
    text = (Path(root) / "src/nightshade.h").read_text(encoding="utf-8")
    if not re.search(r'^#define\s+NP_TITLE\s+NP_NAME\s+" "\s+NP_CODENAME\b', text, re.M):
        raise ValueError('src/nightshade.h must define NP_TITLE as NP_NAME " " NP_CODENAME')
    name, codename, version = _define(text, "NP_NAME"), _define(text, "NP_CODENAME"), _define(text, "NP_VERSION")
    check_codename(version, codename)
    return Metadata(name=name, codename=codename, version=version, title_id=_define(text, "NP_TITLE_ID"),
                    data=_define(text, "NP_DATA"), display_title=f"{name} {codename}",
                    content_version=content_version(version))


def _title(param):
    language = param.get("localizedParameters", {}).get("defaultLanguage", "")
    return param.get("localizedParameters", {}).get(language, {}).get("titleName")


def check_param(path, meta):
    param = json.loads(Path(path).read_text(encoding="utf-8"))
    problems = []
    if param.get("titleId") != meta.title_id:
        problems.append(f'titleId is "{param.get("titleId")}", expected "{meta.title_id}"')
    if param.get("contentVersion") != meta.content_version:
        problems.append(f'contentVersion is "{param.get("contentVersion")}", expected "{meta.content_version}" '
                        f"(NP_VERSION {meta.version})")
    if _title(param) != meta.display_title:
        problems.append(f'titleName is "{_title(param)}", expected "{meta.display_title}"')
    if problems:
        raise ValueError(f"{path}: " + "; ".join(problems) + " (python3 tools/make_release.py --sync-version)")


def sync_param(path, meta):
    """Writes contentVersion and the default language's titleName, keeping every other byte of the file."""
    path = Path(path)
    text = path.read_text(encoding="utf-8")
    language = json.loads(text)["localizedParameters"]["defaultLanguage"]
    new, n = re.subn(r'("contentVersion"\s*:\s*)"[^"]*"', lambda m: f'{m.group(1)}"{meta.content_version}"', text,
                     count=1)
    if n != 1:
        raise ValueError(f"contentVersion not found in {path}")
    block = re.search(r'"' + re.escape(language) + r'"\s*:\s*\{[^{}]*\}', new)
    if not block:
        raise ValueError(f"the {language} parameters not found in {path}")
    replaced, n = re.subn(r'("titleName"\s*:\s*)"(?:[^"\\]|\\.)*"',
                          lambda m: m.group(1) + json.dumps(meta.display_title), block.group(0), count=1)
    if n != 1:
        raise ValueError(f"the {language} titleName not found in {path}")
    new = new[:block.start()] + replaced + new[block.end():]
    if new != text:
        path.write_text(new, encoding="utf-8")
    check_param(path, meta)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--check", metavar="PARAM")
    group.add_argument("--sync", metavar="PARAM")
    args = ap.parse_args()
    try:
        meta = read()
        if args.check:
            check_param(args.check, meta)
            print(f"==> {meta.display_title} {meta.version} ({meta.title_id}, contentVersion {meta.content_version})")
        elif args.sync:
            sync_param(args.sync, meta)
            print(f"==> {args.sync}: {meta.display_title}, contentVersion {meta.content_version}")
        else:
            print(json.dumps(asdict(meta), indent=2))
    except (OSError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
