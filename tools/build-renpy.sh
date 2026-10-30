#!/usr/bin/env bash
# Dulcamara - builds Ren'Py 8's libretro core (Ren'Py, MIT, with CPython 3.12, PSF) as one static archive that
# lives in the same app as the frontend, as tools/build-easyrpg.sh builds EasyRPG Player's, and the engine folder
# the app ships beside eboot.bin.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/build-renpy.sh        -> build/renpy-ps5/librenpy.a, the libretro API renamed renpy_retro_*, and
#                                  build/renpy-ps5/assets/renpy8 (milestone 2)
#   tools/build-renpy.sh host   -> build/renpy-host/: the same core for this computer, linked into its checks
#                                  (tests/core/renpy_headless.c and renpy_unit.c, run by tests/test_renpy_core.py),
#                                  and build/renpy-host/assets/renpy8
#
# Sources: third_party/renpy (Ren'Py 8.5.3, a pinned submodule; its pygame_sdl2 lives in it as renpy/pygame) and
# release tarballs pinned by SHA-256: CPython 3.12.8 (the Python Ren'Py 8.5.3 is built with), FFmpeg 4.3.1 (Ren'Py's,
# with the two fixes Ren'Py applies, LGPL options only and the decoders Ren'Py's own build enables) and Assimp 5.4.3
# (glTF and STL only, as Ren'Py's build). Cython 3.1.4 turns Ren'Py's .pyx into C on the build machine, and the
# pure-Python packages Ren'Py imports go into the engine folder (tools/renpy/wheels.txt). SDL2, SDL2_image, FreeType,
# HarfBuzz, fribidi, libpng, libjpeg, libwebp, zlib, bzip2, xz and OpenSSL come from the build machine for the host
# build (fribidi and SDL2_image are built from pinned tarballs where it lacks them) and from PacBrew for the PS5.
#
# Each stage is redone only when what it is built from changed (build/renpy-<target>/.<stage>).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-ps5}
case $target in ps5|host) ;; *)
    echo "usage: tools/build-renpy.sh [ps5|host]" >&2
    exit 2
esac
if [[ $target == ps5 ]]; then
    echo "tools/build-renpy.sh: the PS5 build is milestone 2 of docs/superpowers/plans/2026-10-29-renpy-core.md" >&2
    exit 2
fi
renpy="$root/third_party/renpy"
out="$root/build/renpy-$target"
sha256() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
# Autotools (fribidi's libtool, CPython's and FFmpeg's configure) cannot build under a path with spaces in it, as a
# checkout's may have. Then build/renpy-<target> is a link to a folder in TMPDIR whose path has none.
if [[ $out == *[[:space:]]* ]]; then
    real="${TMPDIR:-/tmp}"
    real="${real%/}/dulcamara-renpy-$target-$(printf '%s' "$out" | sha256 | cut -c1-12)"
    [[ $real == *[[:space:]]* ]] && { echo "no folder without spaces for the build (TMPDIR has them)" >&2; exit 2; }
    mkdir -p "$real" "$(dirname "$out")"
    if [[ ! -L $out ]]; then
        rm -rf "$out"
        ln -s "$real" "$out"
    fi
    out=$(cd "$out" && pwd -P)
fi
prefix="$out/prefix"
cache="$root/.deps/renpy"
log="$out/logs"
jobs=${BUILD_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
cc=${CC:-cc}
cxx=${CXX:-c++}

python_version=3.12.8
python_hash=c909157bb25ec114e5869124cc2a9c4a4d4c1e957ca4ff553f1edc692101154e
ffmpeg_version=4.3.1
ffmpeg_hash=45035f15d6f192772de2309c846e1d60472694f479679354a39c699719e53772
assimp_version=5.4.3
assimp_hash=66dfbaee288f2bc43172440a55d0235dfc7bf885dda6435c038e8000e79582cb
fribidi_version=1.0.7
fribidi_hash=5ab5f21e9f2fc57b4b40f8ea8f14dba78a5cc46d9cf94bc5e00a58e6886a935d
sdl2_image_version=2.6.2
sdl2_image_hash=48355fb4d8d00bac639cd1c4f4a7661c4afef2c212af60b340e06b7059814777

[[ -f $renpy/renpy/__init__.py && -f $renpy/src/Setup ]] || {
    echo "Ren'Py's source is missing: run 'git submodule update --init third_party/renpy'" >&2
    exit 2
}
for need in cmake curl make pkg-config tar patch; do
    command -v "$need" >/dev/null || { echo "$need is required to build Ren'Py's core" >&2; exit 2; }
done
digest() { cat "$@" | sha256 | cut -c1-16; }
fail() { echo "the $1 build failed; the end of $log/$1.log:" >&2; tail -60 "$log/$1.log" >&2; exit 1; }
# fresh <stage> <key>: the stage was last built from <key>. stamp <stage> <key> records it.
fresh() { [[ -f $out/.$1 && $(cat "$out/.$1") == "$2" ]]; }
stamp() { echo "$2" >"$out/.$1"; }

# fetch <url> <name in .deps/renpy> <sha256>
fetch() {
    local file="$cache/$2"
    mkdir -p "$cache"
    if [[ -f $file ]] && printf '%s  %s\n' "$3" "$file" | sha256 -c - >/dev/null 2>&1; then
        return
    fi
    echo "==> [renpy] downloading $2"
    curl -fsSL "$1" -o "$file.download"
    printf '%s  %s\n' "$3" "$file.download" | sha256 -c - >/dev/null || {
        echo "$2 does not match its pinned SHA-256" >&2
        exit 1
    }
    mv "$file.download" "$file"
}

# unpack <archive in .deps/renpy> <work folder>: a clean copy of the sources
unpack() {
    rm -rf "$2"
    mkdir -p "$2"
    tar -xf "$cache/$1" -C "$2"
}

mkdir -p "$out" "$prefix/include" "$prefix/lib/pkgconfig" "$log" "$out/bin"

# The build machine's libraries, found by pkg-config; on macOS, Homebrew's keg-only ones too.
pc_path="$prefix/lib/pkgconfig"
if [[ $(uname -s) == Darwin ]] && command -v brew >/dev/null; then
    for keg in openssl@3 jpeg-turbo xz; do
        keg_dir=$(brew --prefix "$keg" 2>/dev/null) || continue
        [[ -d $keg_dir/lib/pkgconfig ]] && pc_path="$pc_path:$keg_dir/lib/pkgconfig"
    done
fi
export PKG_CONFIG_PATH="$pc_path${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# ---- fribidi (LGPL-2.1), only where the build machine has none: its library, without the documentation.
build_fribidi() {
    pkg-config --exists fribidi && return
    echo "==> [renpy] fribidi $fribidi_version"
    fetch "https://github.com/fribidi/fribidi/releases/download/v$fribidi_version/fribidi-$fribidi_version.tar.bz2" \
        "fribidi-$fribidi_version.tar.bz2" "$fribidi_hash"
    unpack "fribidi-$fribidi_version.tar.bz2" "$out/work/fribidi"
    local src="$out/work/fribidi/fribidi-$fribidi_version"
    (cd "$src" && ./configure --prefix="$prefix" --disable-shared --enable-static --with-pic CC="$cc") \
        >"$log/fribidi.log" 2>&1 || fail fribidi
    { make -C "$src/gen.tab" && make -C "$src/lib" -j"$jobs" && make -C "$src/lib" install &&
        make -C "$src" install-pkgconfigDATA; } >>"$log/fribidi.log" 2>&1 || fail fribidi
}

# ---- SDL2_image (zlib), only where the build machine has none: PNG, JPEG and WebP, as Ren'Py uses them.
build_sdl2_image() {
    pkg-config --exists SDL2_image && return
    echo "==> [renpy] SDL2_image $sdl2_image_version"
    fetch "https://github.com/libsdl-org/SDL_image/releases/download/release-$sdl2_image_version/SDL2_image-$sdl2_image_version.tar.gz" \
        "SDL2_image-$sdl2_image_version.tar.gz" "$sdl2_image_hash"
    unpack "SDL2_image-$sdl2_image_version.tar.gz" "$out/work/sdl2_image"
    local src="$out/work/sdl2_image/SDL2_image-$sdl2_image_version"
    (cd "$src" && ./configure --prefix="$prefix" --disable-shared --enable-static --with-pic CC="$cc" \
        --disable-imageio --disable-stb-image --disable-avif --disable-jxl --disable-tif --disable-lbm --disable-pcx \
        --disable-xcf --disable-qoi --disable-png-shared --disable-jpg-shared --disable-webp-shared) \
        >"$log/sdl2_image.log" 2>&1 || fail sdl2_image
    { make -C "$src" -j"$jobs" && make -C "$src" install; } >>"$log/sdl2_image.log" 2>&1 || fail sdl2_image
}

# ---- CPython (PSF): static, with the modules of tools/renpy/Setup.local built in. On the build machine it is also
# the Python that runs Cython and Ren'Py's setup.py.
build_python() {
    echo "==> [renpy] CPython $python_version"
    fetch "https://www.python.org/ftp/python/$python_version/Python-$python_version.tar.xz" \
        "Python-$python_version.tar.xz" "$python_hash"
    unpack "Python-$python_version.tar.xz" "$out/work/python"
    rm -rf "$out/python"
    local src="$out/work/python/Python-$python_version" ssl=()
    cp "$root/tools/renpy/Setup.local" "$src/Modules/Setup.local"
    if [[ $(uname -s) == Darwin ]] && command -v brew >/dev/null; then
        ssl=(--with-openssl="$(brew --prefix openssl@3)")
    fi
    (cd "$src" && ./configure --prefix="$out/python" --disable-shared --with-ensurepip=no --disable-test-modules \
        "${ssl[@]}" CC="$cc") >"$log/python.log" 2>&1 || fail python
    { make -C "$src" -j"$jobs" && make -C "$src" install; } >>"$log/python.log" 2>&1 || fail python
    # the two libraries of Setup.local's modules that libpython does not hold (python3.12-config omits them)
    mkdir -p "$out/python/lib/modules"
    cp "$src/Modules/expat/libexpat.a" "$src/Modules/_hacl/libHacl_Hash_SHA2.a" "$out/python/lib/modules/"
    # The engine has no lib-dynload: every C module Ren'Py imports must be built in.
    "$out/python/bin/python3.12" - <<'PY'
import sys
need = {"_asyncio", "_bisect", "_blake2", "_bz2", "_csv", "_datetime", "_elementtree", "_hashlib", "_heapq", "_json",
        "_lzma", "_md5", "_multibytecodec", "_opcode", "_pickle", "_posixsubprocess", "_queue", "_random", "_sha1",
        "_sha2", "_sha3", "_socket", "_ssl", "_struct", "array", "binascii", "fcntl", "math", "mmap", "pyexpat",
        "select", "unicodedata", "zlib"}
missing = need - set(sys.builtin_module_names)
if missing:
    sys.exit("CPython was built without " + " ".join(sorted(missing)))
PY
}

# ---- FFmpeg (LGPL-2.1): Ren'Py's version and fixes, its decoders and demuxers (renpy-build's tasks/ffmpeg.py),
# without AV1 (Ren'Py's needs libaom), encoders, programs or the network.
build_ffmpeg() {
    echo "==> [renpy] FFmpeg $ffmpeg_version"
    fetch "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.gz" "ffmpeg-$ffmpeg_version.tar.gz" "$ffmpeg_hash"
    unpack "ffmpeg-$ffmpeg_version.tar.gz" "$out/work/ffmpeg"
    local src="$out/work/ffmpeg/ffmpeg-$ffmpeg_version" p
    for p in "$root"/tools/patches/ffmpeg-4.3.1-*.diff; do
        patch -d "$src" -p1 <"$p" >>"$log/ffmpeg.log" 2>&1 || fail ffmpeg
    done
    local decoders=flac,mp2,mp3,mp3on4,mpeg1video,mpeg2video,mpegvideo,msmpeg4v1,msmpeg4v2,msmpeg4v3,mpeg4,pcm_dvd
    decoders+=,pcm_s16be,pcm_s16le,pcm_s8,pcm_u16be,pcm_u16le,pcm_u8,theora,vorbis,opus,vp3,vp8,vp9
    local demuxers=au,avi,flac,m4v,matroska,mov,mp3,mpegps,mpegts,mpegtsraw,mpegvideo,ogg,wav
    local parsers=mpegaudio,mpegvideo,mpeg4video,vp3,vp8,vp9
    (cd "$src" && ./configure --prefix="$prefix" --cc="$cc" --enable-static --disable-shared --enable-pic \
        --disable-programs --disable-doc --disable-everything --disable-autodetect --disable-network \
        --disable-avdevice --disable-avfilter --disable-postproc --disable-x86asm --enable-zlib \
        --enable-avcodec --enable-avformat --enable-swresample --enable-swscale \
        --enable-decoder="$decoders" --enable-demuxer="$demuxers" --enable-parser="$parsers") \
        >>"$log/ffmpeg.log" 2>&1 || fail ffmpeg
    grep -q '^#define CONFIG_GPL 0' "$src/config.h" && grep -q '^#define CONFIG_NONFREE 0' "$src/config.h" || {
        echo "FFmpeg was configured with GPL or non-free parts" >&2
        exit 1
    }
    { make -C "$src" -j"$jobs" && make -C "$src" install; } >>"$log/ffmpeg.log" 2>&1 || fail ffmpeg
}

# ---- Assimp (BSD-3-Clause): glTF and STL only, as Ren'Py's build; renpy.gl2.assimp is always imported.
build_assimp() {
    echo "==> [renpy] Assimp $assimp_version"
    fetch "https://github.com/assimp/assimp/archive/refs/tags/v$assimp_version.tar.gz" \
        "assimp-$assimp_version.tar.gz" "$assimp_hash"
    unpack "assimp-$assimp_version.tar.gz" "$out/work/assimp"
    local src="$out/work/assimp/assimp-$assimp_version" generator=()
    command -v ninja >/dev/null && generator=(-G Ninja)
    cmake -S "$src" -B "$out/work/assimp/build" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DCMAKE_CXX_STANDARD=17 \
        -DASSIMP_NO_EXPORT=ON -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DASSIMP_BUILD_ZLIB=OFF \
        -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF -DASSIMP_BUILD_GLTF_IMPORTER=ON -DASSIMP_BUILD_STL_IMPORTER=ON \
        -DASSIMP_WARNINGS_AS_ERRORS=OFF -DASSIMP_INSTALL_PDB=OFF >"$log/assimp.log" 2>&1 || fail assimp
    { cmake --build "$out/work/assimp/build" --parallel "$jobs" && cmake --install "$out/work/assimp/build"; } \
        >>"$log/assimp.log" 2>&1 || fail assimp
}

# ---- the wheels of tools/renpy/wheels.txt of one kind, unpacked into a folder (no pip: they are pure Python)
unpack_wheels() {
    local kind=$1 into=$2 k hash url name
    rm -rf "$into"
    mkdir -p "$into"
    while read -r k hash url; do
        [[ -z $k || $k == \#* || $k != "$kind" ]] && continue
        name=${url##*/}
        fetch "$url" "$name" "$hash"
        "$py" -m zipfile -e "$cache/$name" "$into"
    done <"$root/tools/renpy/wheels.txt"
}

deps_key="deps-1 $python_version $ffmpeg_version $assimp_version $fribidi_version $sdl2_image_version $(
    digest "$root/tools/renpy/Setup.local" "$root/tools/renpy/wheels.txt" "$root"/tools/patches/ffmpeg-4.3.1-*.diff)"
py="$out/python/bin/python3.12"
if ! fresh deps "$deps_key"; then
    build_fribidi
    build_sdl2_image
    build_python
    build_ffmpeg
    build_assimp
    unpack_wheels build "$out/tools"
    printf '#!/bin/sh\nPYTHONPATH="%s" exec "%s" -m cython "$@"\n' "$out/tools" "$py" >"$out/bin/cython"
    chmod +x "$out/bin/cython"
    stamp deps "$deps_key"
fi
"$out/bin/cython" --version
echo "==> [renpy] $py"
