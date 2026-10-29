#!/usr/bin/env python3
"""Compiles Nightshade Player and packs the release asset, PPSA99010.zip, in the project root.

    python3 tools/make_release.py                 # make, then PPSA99010.zip
    python3 tools/make_release.py --skip-build    # only pack what dist/ already holds
    python3 tools/make_release.py --jobs 12 --no-nice
    python3 tools/make_release.py --ip 192.168.1.90 [--port 2121]   # also deploys to the PS5 over FTP

The zip holds just the app folder, PPSA99010/: what the in-app updater installs, and what you copy to
/data/homebrew. The release tag is the version from NP_VERSION in src/nightshade.h, without a "v" (0.1.0).
(From Nightshade Station's, which is SwanStationPS5's.)

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = "PPSA99010"


def read_version():
    text = (ROOT / "src/nightshade.h").read_text()
    m = re.search(r'#define\s+NP_VERSION\s+"([^"]+)"', text)
    if not m:
        sys.exit("error: NP_VERSION not found in src/nightshade.h")
    return m.group(1)


def sync_content_version(version):
    """Writes NP_VERSION into sce_sys/param.json as contentVersion: 1.0.2 -> 01.000.002."""
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", version)
    if not m:
        sys.exit(f'error: NP_VERSION "{version}" is not X.Y.Z')
    major, minor, patch = (int(g) for g in m.groups())
    if major > 99 or minor > 999 or patch > 999:
        sys.exit(f'error: NP_VERSION "{version}" does not fit contentVersion (00-99.000-999.000-999)')
    content = f"{major:02d}.{minor:03d}.{patch:03d}"
    path = ROOT / "sce_sys/param.json"
    text = path.read_text()
    new, n = re.subn(r'("contentVersion"\s*:\s*)"[^"]*"', lambda mm: f'{mm.group(1)}"{content}"', text, count=1)
    if n != 1:
        sys.exit("error: contentVersion not found in sce_sys/param.json")
    if new != text:
        path.write_text(new)
    print(f"==> contentVersion {content} (sce_sys/param.json)")


def build(jobs, use_nice):
    env = dict(os.environ, BUILD_JOBS=str(jobs))
    cmd = ["make"]
    if use_nice:
        cmd = ["nice", "-n", "19"] + cmd
    print("==> " + " ".join(cmd) + f"   (BUILD_JOBS={jobs})", flush=True)
    result = subprocess.run(cmd, cwd=ROOT, env=env)
    if result.returncode != 0:
        sys.exit(f"error: the build failed (make exit code {result.returncode}); no zip was made")


def deploy(ip, port):
    env = dict(os.environ, PS5_HOST=ip, FTP_PORT=str(port))
    print(f"==> deploy to ftp://{ip}:{port}/data/homebrew/{APP}/", flush=True)
    result = subprocess.run(["bash", "tools/deploy.sh"], cwd=ROOT, env=env)
    if result.returncode != 0:
        sys.exit(f"error: the deploy failed (deploy.sh exit code {result.returncode})")


def pack():
    app_dir = ROOT / "dist" / APP
    if not (app_dir / "eboot.bin").is_file():
        sys.exit(f"error: {app_dir / 'eboot.bin'} is missing: build first (drop --skip-build)")
    out = ROOT / f"{APP}.zip"
    if out.exists():
        out.unlink()
    count = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in sorted(app_dir.rglob("*")):
            if path.is_file():
                zf.write(path, f"{APP}/{path.relative_to(app_dir).as_posix()}")
                count += 1
    return out, count


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sync-version", action="store_true", help="only write the version into sce_sys/param.json (make deploy does it)")
    ap.add_argument("--skip-build", action="store_true", help="do not run make; pack dist/ as it is")
    ap.add_argument("--jobs", type=int, default=10, help="parallel compile jobs (default 10)")
    ap.add_argument("--no-nice", action="store_true", help="run make at normal priority (faster, hogs the PC)")
    ap.add_argument("--ip", help="PS5 address: after packing, deploy the app folder to it over FTP")
    ap.add_argument("--port", type=int, default=2121, help="FTP port of the PS5 (default 2121)")
    args = ap.parse_args()

    version = read_version()
    print(f"Nightshade Player {version}")
    if args.sync_version:
        sync_content_version(version)
        return
    if not args.skip_build:
        sync_content_version(version)
        build(args.jobs, not args.no_nice)
    out, count = pack()
    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    print(f"==> {out}  ({count} files, {out.stat().st_size / 1e6:.1f} MB)")
    print(f"    sha256 {digest}")
    if args.ip:
        deploy(args.ip, args.port)
    # A release is published by the Build workflow from a version tag, with SHA256SUMS
    # (.github/workflows/build.yml); a ZIP made here is for testing.
    print(f"    publish: push a signed tag {version} to Oliv3rmoon/Nightshade-Player; the Build workflow "
          f"publishes {out.name} and SHA256SUMS as \"Nightshade Player {version}\"")
    if args.ip:
        print(f"    live log (open the app first): nc {args.ip} 3232 | grep --line-buffered NightshadePlayer")


main()
