#!/usr/bin/env bash
# Dulcamara - the SDK's stub libraries, in the one order the app is linked against.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# sdk_stub_libraries <sdk>/target/lib prints every *.so file there, one a line, in byte order whatever the
# builder's locale. The linker binds each console function to the first stub on its command line that defines it,
# and the SDK has the kernel's functions in four: libkernel.so, libkernel_stub_weak.so and libkernel_web.so (both
# of whose module is libkernel_web) and libkernel_sys.so. In byte order libkernel.so is first: that is the binding
# of every build proven on a console (the CI's, up to 0.2.11). A shell's own *.so glob follows the locale: in a
# macOS user locale it ignores case and puts "_" before ".", and candidates ecl200-n1 to n5 had all 233 kernel
# imports bound to libkernel_web. n5, the first of them to reach a console, stopped at launch there (CE-108255-1)
# and left no log; that binding was the one difference from 0.2.11 in the modules its imports are bound to.
# tools/check-imports.py now refuses such an eboot.bin.
sdk_stub_libraries()
{
    local directory=$1
    [[ -d $directory ]] || {
        echo "sdk_stub_libraries: no such directory: $directory" >&2
        return 1
    }
    find "$directory" -maxdepth 1 -type f -name '*.so' ! -name '.*' -print | LC_ALL=C sort
}
