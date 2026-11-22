#!/usr/bin/env python3
# SwanStationPS5 - fails the build if eboot.bin imports a system module that stops
# a native title from launching ("Can't start the game or app").
# SPDX-License-Identifier: GPL-3.0-or-later
import re
import sys

# Found on hardware:
# - libSceKeyboard, libSceImeDialog (PacBrew SDL2): the title wouldn't launch
# - libScePosixForWebKit (only exporter of strcasestr): crashed when a game started
# - libkernel_web (candidates ecl200-n1 to n5, 2.0.0): every kernel import bound to it instead of libkernel, the
#   binding of each build proven on a console. n5 stopped at launch (CE-108255-1) and left no log; this was the
#   one difference from 0.2.11 in the modules its imports are bound to. The SDK's libkernel_stub_weak.so and libkernel_web.so both name it;
#   libkernel.so must come first (tools/sdk-stubs.sh).
FORBIDDEN = {"libSceKeyboard", "libSceImeDialog", "libScePosixForWebKit", "libkernel_web"}
# Without it nothing the app calls in the kernel resolves.
REQUIRED = {"libkernel"}

data = open(sys.argv[1], "rb").read()
modules = {m.decode() for m in re.findall(rb"(libSce[A-Za-z0-9_]+|libkernel[A-Za-z0-9_]*)(?=\x00)", data)}
print("imported modules:", ", ".join(sorted(modules)))
bad = sorted(modules & FORBIDDEN)
if bad:
    sys.exit(f"error: eboot.bin imports {', '.join(bad)}; it is in no console-proven build of this app. "
             "Provide the functions in src/platform/ps5_shims.c; for libkernel_web, link the SDK's stubs in byte "
             "order (tools/sdk-stubs.sh).")
missing = sorted(REQUIRED - modules)
if missing:
    sys.exit(f"error: eboot.bin does not import {', '.join(missing)}; every build proven on a console does.")
