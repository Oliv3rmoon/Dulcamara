#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rebuild the pinned SDK's Gallium driver with Dulcamara's cache-coherency fix.

The released SDK includes its exact sources. Keep its Mesa/PSBC libraries and
runtime configuration, generate the matching Mesa headers, and replace only
ps5_screen.o. Both Ren'Py cores link the resulting SDK. No remote source HEAD
or binary rewriting is used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile


def run(args, **kwargs):
    print("+", shlex.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def extract(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as stream:
        for member in stream.getmembers():
            target = (destination / member.name).resolve()
            target.relative_to(destination.resolve())
            if member.issym() or member.islnk():
                linked = (target.parent / member.linkname).resolve() if member.issym() else (
                    destination / member.linkname).resolve()
                linked.relative_to(destination.resolve())
        stream.extractall(destination)


def rebuild(package, payload_sdk, work, cc, ar, meson):
    root = Path(__file__).resolve().parents[2]
    patch = root / "tools/patches/ps5-opengl-cache-coherency.diff"
    sources = package / "sources"
    pins = json.loads((package / "dependencies.json").read_text())
    provenance = json.loads((package / "provenance.json").read_text())
    for name in ("ps5-opengl.tar", "opengnm-psbc.tar"):
        if hashlib.sha256((sources / name).read_bytes()).hexdigest() != provenance["source_archives"][name]:
            raise RuntimeError("the SDK's source archive does not match its provenance: " + name)
    mesa_archive = sources / "mesa-26.2.0.tar.xz"
    if hashlib.sha256(mesa_archive.read_bytes()).hexdigest() != pins["mesa"]["sha256"]:
        raise RuntimeError("the SDK's Mesa source does not match its pinned digest")
    work.mkdir(parents=True, exist_ok=True)
    # These are private build intermediates, never an existing source checkout.
    for name in ("ps5-opengl", "opengnm-psbc"):
        if (work / name).exists():
            shutil.rmtree(work / name)
    for name in ("ps5-opengl.tar", "opengnm-psbc.tar"):
        extract(sources / name, work)
    driver_root, mesa_src = work / "ps5-opengl", work / "mesa-26.2.0"
    psbc_patch = driver_root / "toolchain/opengnm-psbc-ps5.patch"
    if hashlib.sha256(psbc_patch.read_bytes()).hexdigest() != pins["psbc_patch"]["sha256"]:
        raise RuntimeError("the PSBC integration patch does not match the SDK pin")
    with psbc_patch.open("rb") as changes:
        run(["patch", "--batch", "-p1", "-d", work / "opengnm-psbc"], stdin=changes)
    with patch.open("rb") as changes:
        run(["patch", "--batch", "-p1", "-d", driver_root], stdin=changes)
    mesa_patch = driver_root / "toolchain/mesa-ps5.patch"
    mesa_key = pins["mesa"]["sha256"] + hashlib.sha256(mesa_patch.read_bytes()).hexdigest()
    mesa_stamp = work / "mesa-source.sha256"
    if not mesa_stamp.exists() or mesa_stamp.read_text() != mesa_key or not mesa_src.exists():
        for path in (mesa_src, work / "mesa-build"):
            if path.exists():
                shutil.rmtree(path)
        extract(mesa_archive, work)
        with mesa_patch.open("rb") as changes:
            run(["patch", "--batch", "-p1", "-d", mesa_src], stdin=changes)
        mesa_stamp.write_text(mesa_key)
    mesa_build = work / "mesa-build"
    # Mesa is the library being built for the console (Meson's host machine).
    # The SDK file calls this target_machine, which only happens to agree when
    # the build computer is also x86-64. Relocate its @DIRNAME@ entries as well.
    cross = work / "prospero.ini"
    cross.write_text((payload_sdk / "toolchain/prospero.ini").read_text().replace(
        "@DIRNAME@", str(payload_sdk / "toolchain")).replace("[target_machine]", "[host_machine]"))
    # Same Mesa feature configuration as toolchain/build-mesa-ps5.sh in SDK1.0.2.
    options = ["--buildtype=release", "-Db_ndebug=true", "-Dplatforms=",
               "-Dc_args=-Wno-thread-safety-analysis", "-Dcpp_args=-Wno-thread-safety-analysis",
               "-Dgallium-drivers=softpipe", "-Dvulkan-drivers=", "-Dopengl=true",
               "-Dllvm=disabled", "-Dshader-cache=disabled", "-Dvideo-codecs=",
               "-Dbuild-tests=false", "-Dtools=", "-Dgallium-rusticl=false"]
    options += ["-D" + name + "=disabled" for name in (
        "egl", "gbm", "gles1", "gles2", "glx", "zlib", "zstd", "xmlconfig", "expat",
        "libunwind", "valgrind", "gallium-va")]
    run([meson, "setup", *(["--reconfigure"] if (mesa_build / "build.ninja").exists() else []), mesa_build, mesa_src,
         "--cross-file=" + str(cross), *options])
    # No Mesa libraries are replaced: only their generated declarations are needed.
    targets = subprocess.check_output(["ninja", "-C", str(mesa_build), "-t", "targets", "all"], text=True)
    headers = [line.rsplit(": ", 1)[0] for line in targets.splitlines()
               if line.rsplit(": ", 1)[0].endswith(".h")
               and "parse" not in line.rsplit(": ", 1)[0]]
    for offset in range(0, len(headers), 100):
        run(["ninja", "-C", mesa_build, *headers[offset:offset + 100]])
    # The SDK runtime also uses PSBC's generated AMD register header (including
    # its additional export constants); softpipe-only Mesa does not generate it.
    run(["make", "-C", work / "opengnm-psbc", "CONFIG=/dev/null",
         "PYTHON=" + sys.executable, "src/amd/common/amdgfxregs.h"])
    config = (package / "runtime-config.txt").read_text()
    # The distribution records its actual feature flags, including dynamic4K.
    # Relocate include paths instead of depending on the publisher's build paths.
    flags = [token for line in config.splitlines()[1:] for token in shlex.split(line)
             if not token.startswith(("-I", "-iquote"))]
    driver = driver_root / "src/gallium/ps5/ps5_screen.c"
    cache_header = work / "shader-cache-build-id.h"
    runtime = package / "sdk/lib/libps5_opengl_core33.a"
    original_runtime = package / "upstream-runtime.a"
    if not original_runtime.exists():
        shutil.copy2(runtime, original_runtime)
    if hashlib.sha256(original_runtime.read_bytes()).hexdigest() != provenance["runtime_archive_sha256"]:
        raise RuntimeError("the saved upstream runtime does not match the SDK provenance")
    libs = sorted((package / "sdk/lib").glob("*.a"))
    libs = [original_runtime if path == runtime else path for path in libs]
    # The upstream helper hashes source and archives; linker scripts are inputs too.
    run(["python3", driver_root / "tools/shader-cache-build-id.py", cache_header,
         package / "runtime-config.txt", driver,
         driver_root / "src/gallium/ps5/ps5_shader_cache.h", *libs])
    includes = [mesa_build / "src", mesa_src / "src", mesa_src / "include",
                mesa_src / "src/gallium/include", mesa_src / "src/gallium/auxiliary",
                driver.parent, driver_root / "src/platform", work / "opengnm-psbc/libpsbc",
                work / "opengnm-psbc/src"]
    # Mesa's public internal headers include generated siblings by basename.
    includes += sorted({(mesa_build / header).parent for header in headers})
    obj = work / "ps5_screen.o"
    run([cc, *flags, *("-I" + str(path) for path in includes),
         "-include", cache_header, "-c", driver, "-o", obj])
    members = subprocess.check_output([str(ar), "t", str(runtime)], text=True).splitlines()
    if members.count("ps5_screen.o") != 1:
        raise RuntimeError("expected exactly one ps5_screen.o in the pinned runtime archive")
    run([ar, "r", runtime, obj])
    run([ar, "s", runtime])
    shutil.copy2(patch, package / "dulcamara-cache-fix.diff")
    manifest = package / "sdk/manifest.sha256"
    original_manifest = package / "upstream-sdk-manifest.sha256"
    if not original_manifest.exists():
        shutil.copy2(manifest, original_manifest)
    manifest.write_text("".join(hashlib.sha256(path.read_bytes()).hexdigest() + "  " +
                                path.relative_to(package / "sdk").as_posix() + "\n"
                                for path in sorted((package / "sdk").rglob("*"))
                                if path.is_file() and path != manifest))
    record = {"sdk_version": "1.0.2", "patch_sha256": hashlib.sha256(patch.read_bytes()).hexdigest(),
              "driver_sha256": hashlib.sha256(driver.read_bytes()).hexdigest(),
              "object_sha256": hashlib.sha256(obj.read_bytes()).hexdigest(),
              "runtime_sha256": hashlib.sha256(runtime.read_bytes()).hexdigest(),
              "scope": "compiled, not console-validated"}
    (package / "dulcamara-cache-fix.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("payload_sdk", type=Path)
    parser.add_argument("work", type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--ar", required=True)
    parser.add_argument("--meson", default=os.environ.get("MESON", "meson"))
    args = parser.parse_args()
    rebuild(args.package.resolve(), args.payload_sdk.resolve(), args.work.resolve(), args.cc, args.ar, args.meson)
