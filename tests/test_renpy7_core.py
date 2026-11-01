#!/usr/bin/env python3
# Dulcamara - Ren'Py 7's core, built for this computer and run headless on the test game of tests/renpy-game.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""tests/test_renpy_core.py's tests on Ren'Py 7's core (tools/build-renpy7.sh host: Ren'Py 7.8.7 with CPython 2.7 and
pygame_sdl2, in the same core C): the test game must play, save and load, in a folder named in Japanese, at a size
larger than the picture and at 44.1 kHz, take the pointer and fast-forward, refuse a second game and report a broken
one. The core's own checks are Ren'Py 8's build's: its C is the same."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

try:
    from tests import test_renpy_core as renpy8
except ImportError:  # unittest discover -s tests
    import test_renpy_core as renpy8


class Renpy7CoreTest(renpy8.RenpyCoreTest):
    SCRIPT = "tools/build-renpy7.sh"
    BUILD = renpy8.ROOT / "build/renpy7-host"
    test_the_core_checks = None


class PS5StdioTest(unittest.TestCase):
    """The stdio.h CPython 2.7 is built with for the PS5 (tools/renpy7/include/stdio.h): its character macros are the
    libc's functions, not FreeBSD's inline FILE reads and writes and their __srget and __swbuf, which the console's libc
    does not take (0.2.9's Ren'Py 7 ended at the first .py file Python read). This computer's stdio.h stands in for the
    SDK's: macOS's is FreeBSD's too, and glibc's inlines its own __uflow and __overflow."""

    INLINED = {"__srget", "__swbuf", "__uflow", "__overflow"}
    SOURCE = r"""
#include <stdio.h>
int reads(FILE *f)
{
    flockfile(f);
    int c = getc_unlocked(f) + getchar_unlocked() + getc(f) + getchar();
    funlockfile(f);
    return c + feof(f) + ferror(f) + fileno(f) + feof_unlocked(f) + ferror_unlocked(f) + fileno_unlocked(f);
}
int writes(FILE *f)
{
    clearerr(f);
    clearerr_unlocked(f);
    return putc_unlocked('a', f) + fputc_unlocked('b', f) + putchar_unlocked('c') + putc('d', f) + putchar('e');
}
"""

    def undefined(self, *flags):
        cc = os.environ.get("CC", "clang")
        if not shutil.which(cc) or not shutil.which("nm"):
            self.skipTest(f"needs {cc} and nm")
        with tempfile.TemporaryDirectory() as directory:
            source, obj = Path(directory) / "stdio.c", Path(directory) / "stdio.o"
            source.write_text(self.SOURCE)
            subprocess.run([cc, "-std=gnu99", "-O2", "-D_DEFAULT_SOURCE", *flags, "-c", str(source), "-o", str(obj)],
                           check=True)
            lines = subprocess.run(["nm", "-u", str(obj)], capture_output=True, text=True, check=True).stdout
        names = [line.split()[-1] for line in lines.splitlines() if line.strip()]
        # Mach-O's C names start with an underscore of their own
        return {name[1:] if sys.platform == "darwin" else name for name in names}

    def test_the_character_macros_are_the_libcs_functions(self):
        names = self.undefined("-I" + str(renpy8.ROOT / "tools/renpy7/include"))
        self.assertFalse(names & self.INLINED, sorted(names))
        # (glibc's getchar and putchar are inline getc and putc of stdin and stdout)
        for function in ("getc", "putc", "fputc", "feof", "ferror", "clearerr", "fileno"):
            self.assertIn(function, names)


if __name__ == "__main__":
    unittest.main()
