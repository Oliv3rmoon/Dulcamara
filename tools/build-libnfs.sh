#!/usr/bin/env bash
# SwanStationPS5 - builds libnfs (the NFS client library, LGPL-2.1-or-later with BSD-2-Clause protocol files) as a
# static library for the PS5 app's games on NFS servers (src/nfs).
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/build-libnfs.sh   -> build/libnfs-ps5/libnfs.a
#
# The sources and the console's configuration (config.h picks config_ps5.h under __PROSPERO__) are in
# third_party/libnfs; only the five source folders libnfs needs are built.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src="$root/third_party/libnfs"
out="$root/build/libnfs-ps5"
[[ -f $src/include/nfsc/libnfs.h ]] || {
    echo "libnfs missing at $src" >&2
    exit 2
}

sdk="${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}"
[[ -d $sdk ]] || bash "$root/tools/setup-native-dependencies.sh" >/dev/null
export PS5_PAYLOAD_SDK=$sdk USE_CCACHE=0
cc=(sh "$root/tooling/prospero-clang18")
ar=$(command -v llvm-ar-18 || command -v llvm-ar)

flags=(-O2 -fPIC -ffunction-sections -fdata-sections -DNDEBUG -w -std=gnu99
    -DHAVE_CONFIG_H -DHAVE_NFS4_2 "-D_U_=__attribute__((unused))"
    -I"$src" -I"$src/include" -I"$src/include/nfsc"
    -I"$src/mount" -I"$src/nfs" -I"$src/nfs4" -I"$src/portmap")

mapfile -t sources < <(cd "$src" && find lib mount nfs nfs4 portmap -name '*.c' | sort)
mkdir -p "$out/obj"
objects=()
for s in "${sources[@]}"; do
    o="$out/obj/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
    if [[ ! -f $o || $src/$s -nt $o ]]; then
        "${cc[@]}" "${flags[@]}" -c "$src/$s" -o "$o"
    fi
    objects+=("$o")
done
rm -f "$out/libnfs.a"
"$ar" rcs "$out/libnfs.a" "${objects[@]}"
echo "==> [libnfs] $out/libnfs.a (${#objects[@]} objects)"
