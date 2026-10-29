#!/usr/bin/env bash
# SwanStationPS5 v2 - fetches the RADV Vulkan driver build (made by the "Build RADV"
# workflow, .github/workflows/radv.yml) and installs it for tools/build.sh:
#   .deps/native/radv-release/                 the driver archive and Vulkan headers
#   .deps/native/ps5-payload-sdk/target/...    + the SDK fork's platform layer
# Needs gh (GH_TOKEN in CI) because the repository may be private.
# Nightshade Player (as Nightshade Station): the release is looked for in this repository first and then in
# darkxex/SwanStationPS5 (RADV_RELEASE_REPOSITORY), whose deps-radv-* pre-release is the same build, so
# Oliv3rmoon/Nightshade-Player builds without a copy of its own. A copy made here must stay a pre-release: the app's update check reads
# releases/latest.
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tag=${RADV_RELEASE_TAG:-deps-radv-7b59ef27}
native="$root/.deps/native"
sdk="$native/ps5-payload-sdk"

if [[ -f $native/radv-release/.tag && $(cat "$native/radv-release/.tag") == "$tag" &&
      -f $sdk/target/lib/libps5platform.a ]]; then
    exit 0
fi
[[ -d $sdk/target/lib ]] || bash "$root/tools/setup-native-dependencies.sh" >/dev/null

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
echo "==> [vulkan] downloading $tag"
fetched=
for repo in ${GITHUB_REPOSITORY:+"$GITHUB_REPOSITORY"} "${RADV_RELEASE_REPOSITORY:-darkxex/SwanStationPS5}"; do
    rm -f "$work"/*.tar.zst
    if gh release download "$tag" -R "$repo" -p '*.tar.zst' -D "$work" 2>/dev/null; then
        fetched=$repo
        break
    fi
    echo "==> [vulkan] no $tag in $repo"
done
[[ -n $fetched ]] || { echo "error: no $tag release with the RADV archives was found" >&2; exit 1; }
echo "==> [vulkan] $tag from $fetched"

rm -rf "$native/radv-release"
tar --zstd -xf "$work/radv-release.tar.zst" -C "$native"
echo "$tag" > "$native/radv-release/.tag"

# Only the platform layer is taken from the fork's SDK: the rest is the same
# v0.42 release tools/setup-native-dependencies.sh installed.
tar --zstd -xf "$work/ps5-payload-sdk.tar.zst" -C "$work" \
    ps5-payload-sdk/target/lib/libps5platform.a ps5-payload-sdk/target/include/ps5platform
cp "$work/ps5-payload-sdk/target/lib/libps5platform.a" "$sdk/target/lib/"
rm -rf "$sdk/target/include/ps5platform"
cp -r "$work/ps5-payload-sdk/target/include/ps5platform" "$sdk/target/include/"
echo "==> [vulkan] RADV and the platform layer are in $native"
