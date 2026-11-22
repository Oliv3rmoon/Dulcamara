#!/usr/bin/env python3
"""Normalize local checkout prefixes in the linker's read-only C strings before packaging.
Cached game engines can retain an earlier checkout's __FILE__ and configure paths.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

source, destination = map(Path, sys.argv[1:3])
objcopy = next((p for p in (shutil.which("llvm-objcopy-18"), shutil.which("llvm-objcopy")) if p), None)
if objcopy is None:
    sys.exit("llvm-objcopy is required")
home = os.fsencode(Path.home())
# Restrict this to the current builder's home directory, followed by a known project path.
# Keep offsets intact for ELF merged strings whose references can start in a shared suffix.
prefix = re.compile(re.escape(home) + rb"/[^\0\r\n]*?/(?=(?:build/(?:renpy|easyrpg)|src/core/renpy|\.deps/))")
with tempfile.TemporaryDirectory() as directory:
    pool = Path(directory) / "rodata"
    subprocess.run([objcopy, "--dump-section", ".rodata=" + str(pool), str(source),
                    str(Path(directory) / "scratch.elf")], check=True)
    def canonical(match):
        length = len(match.group())
        label = b"/build/dulcamara/"
        return label + b"/" * (length - len(label)) if length >= len(label) else match.group()
    pool.write_bytes(prefix.sub(canonical, pool.read_bytes()))
    subprocess.run([objcopy, "--update-section", ".rodata=" + str(pool), str(source), str(destination)], check=True)
