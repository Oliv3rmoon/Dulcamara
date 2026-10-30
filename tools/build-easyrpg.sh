#!/usr/bin/env bash
# Dulcamara - builds EasyRPG Player's libretro core (EasyRPG/Player, GPL-3.0-or-later, and EasyRPG/liblcf,
# MIT) for the PS5 as one static archive that lives in the same app as the frontend, the way Mandrake
# builds SwanStation.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/build-easyrpg.sh        -> build/easyrpg-ps5/libeasyrpg.a, the libretro API renamed easyrpg_retro_*
#   tools/build-easyrpg.sh host   -> build/easyrpg-host/: the same core for this computer, linked into the
#                                    headless check of tests/core (tests/test_core.py runs it)
#
# Sources: third_party/easyrpg-player and third_party/liblcf (pinned submodules), third_party/inih (liblcf reads
# RPG_RT.ini with it) and pixman, which PacBrew lacks: a release tarball, pinned by its SHA-256. liblcf gets
# tools/patches/liblcf-iconv.patch: PacBrew has no ICU either, so ICU is off and the game's text encodings
# (Shift-JIS, the Windows code pages...) go through iconv, with a guess at the encoding from the bytes instead of
# ICU's detector.
#
# The other libraries come from the PacBrew prefix the frontend already uses (tools/setup-pacbrew-dependencies.sh):
# fmt, libpng, zlib, FreeType, HarfBuzz, Ogg Vorbis, mpg123, libsndfile and FLAC, libsamplerate and libiconv.
# WildMidi and FluidSynth are off: EasyRPG's own FmMidi synthesiser plays the MIDI music. Opus (PacBrew has opus
# but not opusfile), lhasa (.lzh games), libxmp and nlohmann-json are off too.
#
# Like the cores of Mandrake, the archive is self-contained: the core and every library it uses are
# linked into one relocatable object, every symbol but the libretro API is made local, and the API is renamed
# easyrpg_retro_*. A second core (ScummVM's) can sit beside it the same way.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-ps5}
case $target in ps5|host) ;; *)
    echo "usage: tools/build-easyrpg.sh [ps5|host]" >&2
    exit 2
esac
player="$root/third_party/easyrpg-player"
lcf="$root/third_party/liblcf"
inih="$root/third_party/inih"
out="$root/build/easyrpg-$target"
prefix="$out/prefix"
jobs=${BUILD_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}

pixman_version=0.46.4
pixman_url="https://www.cairographics.org/releases/pixman-$pixman_version.tar.gz"
pixman_hash=d09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c

for need in "$player/CMakeLists.txt" "$player/builds/libretro/libretro-common/include/libretro.h" \
    "$lcf/CMakeLists.txt" "$inih/ini.c"; do
    [[ -f $need ]] || {
        echo "EasyRPG sources missing: run 'git submodule update --init --recursive third_party/easyrpg-player third_party/liblcf third_party/inih'" >&2
        exit 2
    }
done
command -v cmake >/dev/null || { echo "cmake is required to build EasyRPG Player" >&2; exit 2; }
sha256() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
generator=()
# The PS5 toolchain keeps CMake from searching PATH: the build program is named outright.
command -v ninja >/dev/null && generator=(-G Ninja -DCMAKE_MAKE_PROGRAM="$(command -v ninja)")

# liblcf's change lives as a patch (the submodule stays pristine); applied once.
patch="$root/tools/patches/liblcf-iconv.patch"
if ! git -C "$lcf" apply --reverse --check "$patch" 2>/dev/null; then
    echo "==> [easyrpg] applying $(basename "$patch") to liblcf"
    git -C "$lcf" apply "$patch"
fi

# Rebuild only when a source revision, this script or the patch changed.
stamp="$out/.stamp"
want="$(git -C "$player" rev-parse HEAD) $(git -C "$lcf" rev-parse HEAD) $(git -C "$inih" rev-parse HEAD) $pixman_version $(
    cat "$0" "$patch" "$root"/tests/core/* | sha256 | cut -c1-16)"
result="$out/libeasyrpg.a"
[[ $target == host ]] && result="$out/core-check/easyrpg-headless"
if [[ -f $result && -f $stamp && $(cat "$stamp") == "$want" ]]; then
    echo "==> [easyrpg] $result"
    exit 0
fi
rm -rf "$out"
mkdir -p "$out/bin" "$prefix/include" "$prefix/lib"

cmake_common=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

if [[ $target == ps5 ]]; then
    sdk="${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}"
    [[ -x $sdk/bin/prospero-lld ]] || bash "$root/tools/setup-native-dependencies.sh" >/dev/null
    export PS5_PAYLOAD_SDK=$sdk USE_CCACHE=0
    sysroot=$(bash "$root/tools/setup-pacbrew-dependencies.sh" --all)
    hb="$sysroot/user/homebrew"
    llvm=$(dirname "$(command -v llvm-ar-18 || command -v llvm-ar)")
    ar="$llvm/llvm-ar-18"; [[ -x $ar ]] || ar="$llvm/llvm-ar"
    ranlib="$llvm/llvm-ranlib-18"; [[ -x $ranlib ]] || ranlib="$llvm/llvm-ranlib"
    objcopy="$llvm/llvm-objcopy-18"; [[ -x $objcopy ]] || objcopy="$llvm/llvm-objcopy"
    nm="$llvm/llvm-nm-18"; [[ -x $nm ]] || nm="$llvm/llvm-nm"
    lld=$(command -v ld.lld-18 || command -v ld.lld)

    # The compiler: the app's own PS5 target (tooling/prospero-clang18), as programs CMake can call.
    printf '#!/bin/sh\nexec sh "%s" "$@"\n' "$root/tooling/prospero-clang18" > "$out/bin/ps5-cc"
    printf '#!/bin/sh\nexec sh "%s" --driver-mode=g++ "$@"\n' "$root/tooling/prospero-clang18" > "$out/bin/ps5-c++"
    chmod +x "$out/bin/ps5-cc" "$out/bin/ps5-c++"
    # -DNDEBUG: the PS5 libc has no __assert. EasyRPG needs exceptions and RTTI, which the prospero target leaves
    # off. Nothing is linked here, so CMake's checks only compile.
    flags="-O2 -fPIC -w -ffunction-sections -fdata-sections -march=znver2 -DNDEBUG"
    cat > "$out/ps5.cmake" <<EOF
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CROSSCOMPILING ON)
set(CMAKE_C_COMPILER "$out/bin/ps5-cc")
set(CMAKE_CXX_COMPILER "$out/bin/ps5-c++")
set(CMAKE_AR "$ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "$ranlib" CACHE FILEPATH "")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "$flags")
set(CMAKE_CXX_FLAGS_INIT "$flags -frtti -fexceptions")
set(CMAKE_C_FLAGS_RELEASE "-DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG" CACHE STRING "")
# Only the PS5's libraries: this prefix and PacBrew's, never the build machine's.
set(CMAKE_PREFIX_PATH "$prefix;$hb")
set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH OFF)
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
EOF
    export PKG_CONFIG_DIR= PKG_CONFIG_SYSROOT_DIR="$sysroot" PKG_CONFIG_PATH=
    export PKG_CONFIG_LIBDIR="$hb/lib/pkgconfig:$hb/libdata/pkgconfig"
    cmake_target=(-DCMAKE_TOOLCHAIN_FILE="$out/ps5.cmake"
        -DIconv_IS_BUILT_IN=OFF -DIconv_INCLUDE_DIR="$hb/include" -DIconv_LIBRARY="$hb/lib/libiconv.a")
    cc=("$out/bin/ps5-cc")
    cflags=(-O2 -fPIC -w -ffunction-sections -fdata-sections -march=znver2 -DNDEBUG)
else
    ar=$(command -v ar)
    cmake_target=()
    if [[ $(uname -s) == Darwin ]] && command -v brew >/dev/null; then
        cmake_target=(-DCMAKE_PREFIX_PATH="$prefix;$(brew --prefix)")
    else
        cmake_target=(-DCMAKE_PREFIX_PATH="$prefix")
    fi
    cc=("${CC:-cc}")
    cflags=(-O2 -fPIC -w)
fi

# ---- pixman (MIT): its library sources, compiled straight from the release (no meson needed); SSE2 and SSSE3 as
# its meson build turns them on for x86-64, chosen at run time by its CPU check. The build machine's pixman is used
# for the host check when it has one.
build_pixman() {
    local work="$out/pixman" archive="$root/.deps/pixman/pixman-$pixman_version.tar.gz"
    mkdir -p "$work" "$(dirname "$archive")"
    if [[ ! -f $archive ]] || ! printf '%s  %s\n' "$pixman_hash" "$archive" | sha256 -c - >/dev/null 2>&1; then
        echo "==> [easyrpg] downloading pixman $pixman_version"
        curl -fsSL "$pixman_url" -o "$archive.download"
        printf '%s  %s\n' "$pixman_hash" "$archive.download" | sha256 -c - >/dev/null || {
            echo "pixman-$pixman_version.tar.gz does not match its pinned SHA-256" >&2
            exit 1
        }
        mv "$archive.download" "$archive"
    fi
    tar -xzf "$archive" -C "$work"
    local src="$work/pixman-$pixman_version/pixman"
    local major=${pixman_version%%.*} rest=${pixman_version#*.}
    sed -e "s/@PIXMAN_VERSION_MAJOR@/$major/g" -e "s/@PIXMAN_VERSION_MINOR@/${rest%%.*}/g" \
        -e "s/@PIXMAN_VERSION_MICRO@/${rest#*.}/g" "$src/pixman-version.h.in" > "$src/pixman-version.h"
    local simd=()
    [[ $target == ps5 || $(uname -m) == x86_64 ]] && simd=(USE_SSE2 USE_SSSE3)
    {
        printf '#define PACKAGE "pixman"\n#define HAVE_PTHREADS 1\n#define TLS __thread\n#define SIZEOF_LONG 8\n'
        printf '#define HAVE_BUILTIN_CLZ 1\n#define HAVE_GCC_VECTOR_EXTENSIONS 1\n#define HAVE_POSIX_MEMALIGN 1\n'
        printf '#define TOOLCHAIN_SUPPORTS_ATTRIBUTE_CONSTRUCTOR 1\n#define TOOLCHAIN_SUPPORTS_ATTRIBUTE_DESTRUCTOR 1\n'
        for s in "${simd[@]}"; do printf '#define %s 1\n' "$s"; done
    } > "$src/pixman-config.h"
    local files=(pixman pixman-access pixman-access-accessors pixman-arm pixman-bits-image pixman-combine32
        pixman-combine-float pixman-conical-gradient pixman-edge pixman-edge-accessors pixman-fast-path pixman-filter
        pixman-glyph pixman-general pixman-gradient-walker pixman-image pixman-implementation pixman-linear-gradient
        pixman-matrix pixman-mips pixman-noop pixman-ppc pixman-radial-gradient pixman-region16 pixman-region32
        pixman-region64f pixman-riscv pixman-solid-fill pixman-timer pixman-trap pixman-utils pixman-x86)
    [[ ${#simd[@]} -gt 0 ]] && files+=(pixman-sse2 pixman-ssse3)
    local objects=() f extra
    for f in "${files[@]}"; do
        extra=()
        [[ $f == pixman-sse2 ]] && extra=(-msse2)
        [[ $f == pixman-ssse3 ]] && extra=(-mssse3)
        "${cc[@]}" "${cflags[@]}" "${extra[@]}" -DHAVE_CONFIG_H -include "$src/pixman-config.h" -I"$src" \
            -c "$src/$f.c" -o "$work/$f.o"
        objects+=("$work/$f.o")
    done
    mkdir -p "$prefix/include/pixman-1"
    cp "$src/pixman.h" "$src/pixman-version.h" "$prefix/include/pixman-1/"
    rm -f "$prefix/lib/libpixman-1.a"
    "$ar" rcs "$prefix/lib/libpixman-1.a" "${objects[@]}"
}

# ---- inih (BSD-3-Clause): ini.c, as liblcf's INIReader expects it.
build_inih() {
    mkdir -p "$out/inih"
    "${cc[@]}" "${cflags[@]}" -c "$inih/ini.c" -o "$out/inih/ini.o"
    cp "$inih/ini.h" "$prefix/include/"
    rm -f "$prefix/lib/libinih.a"
    "$ar" rcs "$prefix/lib/libinih.a" "$out/inih/ini.o"
}

# ---- liblcf (MIT): RPG Maker's file formats; ICU off, iconv on (the patch), no XML, no tools or tests.
build_liblcf() {
    echo "==> [easyrpg] liblcf"
    cmake -S "$lcf" -B "$out/liblcf" "${generator[@]}" "${cmake_common[@]}" "${cmake_target[@]}" \
        -DLIBLCF_WITH_ICU=OFF -DLIBLCF_WITH_ICONV=ON -DLIBLCF_WITH_XML=OFF -DLIBLCF_WITH_INI=ON \
        -DLIBLCF_ENABLE_TOOLS=OFF -DLIBLCF_ENABLE_TESTS=OFF -DLIBLCF_ENABLE_BENCHMARKS=OFF \
        -DLIBLCF_ENABLE_INSTALL=ON -DLIBLCF_UPDATE_MIMEDB=OFF \
        -DINIH_INCLUDE_DIR="$prefix/include" -DINIH_LIBRARY="$prefix/lib/libinih.a" >"$out/liblcf.log" ||
        { cat "$out/liblcf.log"; exit 1; }
    cmake --build "$out/liblcf" --parallel "$jobs" >>"$out/liblcf.log" || { tail -60 "$out/liblcf.log"; exit 1; }
    cmake --install "$out/liblcf" >>"$out/liblcf.log"
}

# The core's options, the same for the PS5 and the host check.
player_options=(-DPLAYER_TARGET_PLATFORM=libretro -DPLAYER_ENABLE_TESTS=OFF -DPLAYER_WITH_OPUS=OFF
    -DPLAYER_WITH_WILDMIDI=OFF -DPLAYER_WITH_FLUIDSYNTH=OFF -DPLAYER_WITH_FLUIDLITE=OFF -DPLAYER_WITH_XMP=OFF
    -DPLAYER_WITH_LHASA=OFF -DPLAYER_WITH_NLOHMANN_JSON=OFF -DPLAYER_WITH_SPEEXDSP=OFF -DPLAYER_ENABLE_FMMIDI=ON
    -DINIH_INCLUDE_DIR="$prefix/include" -DINIH_LIBRARY="$prefix/lib/libinih.a")

if [[ $target == host ]]; then
    build_inih
    build_liblcf
    pixman=()
    if ! pkg-config --exists pixman-1 2>/dev/null; then
        build_pixman
        pixman=(-DPIXMAN_INCLUDE_DIR="$prefix/include/pixman-1" -DPIXMAN_LIBRARY="$prefix/lib/libpixman-1.a")
    fi
    # No libsndfile or libsamplerate needed on the build machine: dr_wav reads WAV and EasyRPG resamples itself.
    echo "==> [easyrpg] the core and its headless check (tests/core)"
    cmake -S "$root/tests/core" -B "$out/core-check" "${generator[@]}" "${cmake_common[@]}" "${cmake_target[@]}" \
        "${player_options[@]}" "${pixman[@]}" -DEASYRPG_PLAYER_DIR="$player" >"$out/core.log" ||
        { cat "$out/core.log"; exit 1; }
    cmake --build "$out/core-check" --parallel "$jobs" >>"$out/core.log" || { tail -80 "$out/core.log"; exit 1; }
    echo "$want" >"$stamp"
    echo "==> [easyrpg] $result"
    exit 0
fi

echo "==> [easyrpg] pixman $pixman_version, inih, liblcf ($jobs jobs)"
build_pixman
build_inih
build_liblcf

echo "==> [easyrpg] EasyRPG Player (libretro)"
# rthreads.c asks for _POSIX_C_SOURCE 199309 when it is not set, and the SDK's headers then hide CLOCK_REALTIME
# (it needs 200112): setting it first makes its own #ifndef skip it. The core's only C file is rthreads.c.
cmake -S "$player" -B "$out/player" "${generator[@]}" "${cmake_common[@]}" "${cmake_target[@]}" "${player_options[@]}" \
    -DCMAKE_C_FLAGS="$flags -D_POSIX_C_SOURCE=200112L" \
    -DPIXMAN_INCLUDE_DIR="$prefix/include/pixman-1" -DPIXMAN_LIBRARY="$prefix/lib/libpixman-1.a" \
    >"$out/player.log" || { cat "$out/player.log"; exit 1; }
grep -E "^-- (Found|Could NOT)|Audio|audio" "$out/player.log" | sed 's/^/    /' || true
cmake --build "$out/player" --target easyrpg_libretro --parallel "$jobs" >>"$out/player.log" ||
    { grep -E "error|Error" -A3 "$out/player.log" | head -80; exit 1; }

echo "==> [easyrpg] isolating its symbols"
work="$out/isolate"
mkdir -p "$work"
core_archive=$(find "$out/player" -name 'easyrpg_libretro.a' -print -quit)
common_archive=$(find "$out/player" -name 'libretro_common.a' -print -quit)
[[ -f $core_archive && -f $common_archive ]] || { echo "the core's archives were not built" >&2; exit 1; }
# Every library the core uses goes into the object: only what it references is taken from each.
libraries=("$common_archive" "$prefix/lib/liblcf.a" "$prefix/lib/libinih.a" "$prefix/lib/libpixman-1.a")
for name in fmt png16 freetype bz2 harfbuzz vorbisfile vorbisenc vorbis ogg mpg123 sndfile FLAC opus mp3lame \
    samplerate iconv z; do
    [[ -f $hb/lib/lib$name.a ]] || { echo "PacBrew has no lib$name.a" >&2; exit 1; }
    libraries+=("$hb/lib/lib$name.a")
done
"$lld" -r -o "$work/easyrpg.o" --whole-archive "$core_archive" --no-whole-archive \
    --start-group "${libraries[@]}" --end-group
# keep only the libretro API global, then rename it
"$nm" --defined-only --extern-only "$work/easyrpg.o" | awk '{print $NF}' | grep -E '^retro_' | sort -u >"$work/api.txt"
[[ -s $work/api.txt ]] || { echo "no retro_* symbols in EasyRPG Player" >&2; exit 1; }
sed 's/.*/& easyrpg_&/' "$work/api.txt" >"$work/rename.txt"
"$objcopy" --keep-global-symbols="$work/api.txt" "$work/easyrpg.o" "$work/easyrpg-local.o"
"$objcopy" --redefine-syms="$work/rename.txt" "$work/easyrpg-local.o" "$work/easyrpg-ps5.o"
# What the object still needs from outside: the C and C++ runtimes and the system, never another library.
"$nm" --undefined-only "$work/easyrpg-ps5.o" | awk '{print $NF}' | sort -u >"$work/undefined.txt"
rm -f "$result"
"$ar" rcs "$result" "$work/easyrpg-ps5.o"
echo "$want" >"$stamp"
echo "==> [easyrpg] $(wc -l <"$work/api.txt" | tr -d ' ') libretro functions as easyrpg_retro_*," \
    "$(wc -l <"$work/undefined.txt" | tr -d ' ') runtime symbols left for the app's link"
echo "==> [easyrpg] $result"
