#!/usr/bin/env bash
# Dulcamara - builds the app for this computer (tools/desktop): SDL2 in a window, EasyRPG Player's core
# built as tools/build-easyrpg.sh host builds it. For the interface; the PS5 build is `make`.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/build-desktop.sh            -> build/desktop/dulcamara
#   tools/build-desktop.sh preview    -> build/desktop-preview/dulcamara (off screen, scripted: NP_SCRIPT)
#
# Run it with NP_ROOT=<a data folder> (default ./dulcamara-data) and NP_ASSETS=<repo>/assets.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-window}
bash "$root/tools/build-easyrpg.sh" host >/dev/null
prefix="$root/build/easyrpg-host/prefix"
out="$root/build/desktop"
preview=OFF
[[ $mode == preview ]] && { out="$root/build/desktop-preview"; preview=ON; }
paths="$prefix"
[[ $(uname -s) == Darwin ]] && command -v brew >/dev/null && paths="$prefix;$(brew --prefix)"
generator=()
command -v ninja >/dev/null && generator=(-G Ninja)
cmake -S "$root/tools/desktop" -B "$out" "${generator[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$paths" -DNP_PREVIEW=$preview -DBUILD_SHARED_LIBS=OFF \
    -DPLAYER_TARGET_PLATFORM=libretro -DPLAYER_ENABLE_TESTS=OFF -DPLAYER_WITH_OPUS=OFF -DPLAYER_WITH_WILDMIDI=OFF \
    -DPLAYER_WITH_FLUIDSYNTH=OFF -DPLAYER_WITH_FLUIDLITE=OFF -DPLAYER_WITH_XMP=OFF -DPLAYER_WITH_LHASA=OFF \
    -DPLAYER_WITH_NLOHMANN_JSON=OFF -DPLAYER_WITH_SPEEXDSP=OFF \
    -DINIH_INCLUDE_DIR="$prefix/include" -DINIH_LIBRARY="$prefix/lib/libinih.a" >"$out.log" 2>&1 ||
    { tail -40 "$out.log"; exit 1; }
cmake --build "$out" --target dulcamara --parallel "${BUILD_JOBS:-8}"
echo "==> [desktop] $out/dulcamara"
