#!/usr/bin/env bash
# Dulcamara - fails the build if the linked app imports a console function weakly.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   bash tools/check-weak-imports.sh <llvm-nm> <build/llvm-pie.elf>
#
# Every import of each build proven on a console (the CI's, up to 0.2.11) is a strong one. The SDK's platform layer
# declares __real_fclose and __real_fflush weak (its open_memstream), the link wraps both, and with a newer linker
# (LLD 23 on a Mac, against the CI's LLD 18) the app's only reference to the console's fclose and fflush was that
# weak one: candidates ecl200-n1 to n7 imported both weakly. Whether the console binds a weak import is not known,
# and fclose is called in the first lines of main. src/platform/ps5_shims.c now refers to both strongly.
set -euo pipefail
nm=$1 elf=$2
weak=$("$nm" -D --undefined-only "$elf" | awk 'NF >= 2 && ($(NF-1) == "w" || $(NF-1) == "v") { print $NF }' | LC_ALL=C sort)
if [[ -n $weak ]]; then
    echo "error: $elf imports weakly: $(tr '\n' ' ' <<<"$weak")" >&2
    echo "Refer to each strongly from an app object (src/platform/ps5_shims.c); no console-proven build has a weak import." >&2
    exit 1
fi
echo "weak imports: none"
