#!/usr/bin/env bash
# Dulcamara - fetches EasyRPG's TestGame (github.com/EasyRPG/TestGame) at a pinned commit into the ignored
# cache, for the host checks only: tests/test_core.py runs the core on it, tests/test_library.py finds it.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/fetch-testgame.sh   -> .deps/testgame/TestGame-2000, TestGame-2003, ...
#
# TestGame is GPL-3.0-or-later (its COPYING), its graphics and sounds CC-BY-4.0, CC0 or public domain as its
# AUTHORS.md lists them. It is never committed here and never goes into the app or its release ZIP: players copy
# their own games.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
commit=4bbedb73492c80b79d8290e0b06ea86875b0bdc8
dir="$root/.deps/testgame"

if [[ -f $dir/.commit && $(cat "$dir/.commit") == "$commit" && -f $dir/TestGame-2000/RPG_RT.ldb ]]; then
    echo "$dir"
    exit 0
fi
rm -rf "$dir"
mkdir -p "$dir"
git -C "$dir" init -q
git -C "$dir" fetch -q --depth 1 https://github.com/EasyRPG/TestGame.git "$commit"
git -C "$dir" checkout -q FETCH_HEAD
[[ $(git -C "$dir" rev-parse HEAD) == "$commit" ]] || { echo "TestGame is not at $commit" >&2; exit 1; }
echo "$commit" >"$dir/.commit"
echo "$dir"
