#!/usr/bin/env python3
# Dulcamara - Ren'Py 7's core, built for this computer and run headless on the test game of tests/renpy-game.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""tests/test_renpy_core.py's tests on Ren'Py 7's core (tools/build-renpy7.sh host: Ren'Py 7.8.7 with CPython 2.7 and
pygame_sdl2, in the same core C): the test game must play, save and load, in a folder named in Japanese, at a size
larger than the picture and at 44.1 kHz, take the pointer and fast-forward, refuse a second game and report a broken
one. The core's own checks are Ren'Py 8's build's: its C is the same."""

import unittest

try:
    from tests import test_renpy_core as renpy8
except ImportError:  # unittest discover -s tests
    import test_renpy_core as renpy8


class Renpy7CoreTest(renpy8.RenpyCoreTest):
    SCRIPT = "tools/build-renpy7.sh"
    BUILD = renpy8.ROOT / "build/renpy7-host"
    test_the_core_checks = None


if __name__ == "__main__":
    unittest.main()
