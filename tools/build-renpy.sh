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
#   tools/build-renpy.sh engine -> build/renpy-host/: only what the PS5 build needs from the host (the Python that
#                                  runs the build, Cython's C for Ren'Py's modules, the engine folder)
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
case $target in ps5|host|engine) ;; *)
    echo "usage: tools/build-renpy.sh [ps5|host|engine]" >&2
    exit 2
esac
# The PS5 build starts from the host's: its Python runs the build, its generated C and engine (with Ren'Py's common
# scripts compiled by the host core) are the console's too.
[[ $target == ps5 ]] && bash "$root/tools/build-renpy.sh" host
renpy="$root/third_party/renpy"
folder=$target
[[ $target == engine ]] && folder=host
source "$root/tools/renpy/common.sh"
out=$(build_folder "renpy-$folder")
prefix="$out/prefix"
log="$out/logs"
cc=${CC:-cc}
cxx=${CXX:-c++}

renpy_version=8.5.3.26051504 # third_party/renpy's tag
renpy_version_name="We Can Go to the Moon"
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
mkdir -p "$out" "$prefix/include" "$prefix/lib/pkgconfig" "$log" "$out/bin"

if [[ $target == ps5 ]]; then
    source "$root/tools/renpy/ps5.sh"
    exit 0
fi

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
    if [[ $(uname -s) == Darwin ]]; then
        # urllib.request imports it on macOS (setuptools here; requests in the engine)
        printf '\n*static*\n_scproxy -DPy_BUILD_CORE_BUILTIN _scproxy.c $(MODULE__SCPROXY_LDFLAGS)\n' \
            >>"$src/Modules/Setup.local"
        command -v brew >/dev/null && ssl=(--with-openssl="$(brew --prefix openssl@3)")
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

# The Python that runs the build (and, on the host, the engine's), with Cython and setuptools beside it.
python_key="python-2 $python_version $(digest "$root/tools/renpy/Setup.local" "$root/tools/renpy/wheels.txt") $(
    recipe build_python unpack_wheels)"
py="$out/python/bin/python3.12"
if ! fresh python "$python_key"; then
    build_python
    unpack_wheels build "$out/tools"
    printf '#!/bin/sh\nPYTHONPATH="%s" exec "%s" -m cython "$@"\n' "$out/tools" "$py" >"$out/bin/cython"
    chmod +x "$out/bin/cython"
    stamp python "$python_key"
fi
# The host core's libraries (the PS5 build makes its own).
libs_key="libs-2 $ffmpeg_version $assimp_version $fribidi_version $sdl2_image_version $(
    recipe build_fribidi build_sdl2_image build_ffmpeg build_assimp) $(
    digest "$root"/tools/patches/ffmpeg-4.3.1-*.diff)"
if [[ $target == host ]] && ! fresh libs "$libs_key"; then
    build_fribidi
    build_sdl2_image
    build_ffmpeg
    build_assimp
    stamp libs "$libs_key"
fi
# ---- Ren'Py's own C: setup.py turns its .pyx into C for a static build (RENPY_STATIC: each module's init function
# named after its whole dotted name), then each module listed in src/Setup is compiled with the core's renamed SDL
# calls (src/core/renpy/sdl_renames.h) and registered in an inittab. Two optional modules are left out:
# tinyfiledialogs (renpy.tfd, desktop file dialogs) and renpy.gl2.live2dmodel, which needs Live2D's Cubism SDK
# (not free to give away; Ren'Py's own builds add it only where it is installed).
sdl_cflags() { pkg-config --cflags sdl2 SDL2_image freetype2 harfbuzz fribidi libpng libjpeg libwebp zlib; }
build_generate() {
    echo "==> [renpy] Ren'Py's modules as C (Cython $("$out/bin/cython" --version 2>&1 | awk '{print $NF}'))"
    local src="$out/renpy-src" generated
    rm -rf "$src" "$out/gen"
    mkdir -p "$src" "$out/gen"
    git -C "$renpy" archive HEAD | tar -x -C "$src"
    (cd "$src" && RENPY_STATIC=1 RENPY_REGENERATE_CYTHON=1 RENPY_CYTHON="$out/bin/cython" PYTHONPATH="$out/tools" \
        CC="$cc" CXX="$cxx" CFLAGS="-O2" "$py" setup.py generate) >"$log/generate.log" 2>&1 || fail generate
    generated="$src/tmp/gen3-static"
    [[ -d $generated ]] || { echo "Ren'Py's setup.py did not write $generated" >&2; exit 1; }
    local modules=() sources=() line parts s
    while read -r line; do
        line=${line%%#*}
        read -r -a parts <<<"$line"
        [[ ${#parts[@]} -eq 0 || ${parts[0]} == renpy.tfd || ${parts[0]} == renpy.gl2.live2dmodel ]] && continue
        modules+=("${parts[0]}")
        for s in "${parts[@]:1}"; do
            sources+=("$src/src/${s/..\/tmp\/gen\//../tmp/gen3-static/}")
        done
    done <"$src/src/Setup"
    printf '%s\n' "${modules[@]}" >"$out/modules.txt"
    printf '%s\n' "${sources[@]}" | sort -u >"$out/sources.txt" # the PS5 build compiles the same files
    {
        printf '/* Dulcamara - written by tools/build-renpy.sh: the engine'"'"'s compiled modules, built in. */\n'
        printf '#include <Python.h>\n\n'
        for s in "${modules[@]}"; do printf 'PyMODINIT_FUNC PyInit_%s(void);\n' "${s//./_}"; done
        printf '\nstatic struct _inittab renpy_modules[] = {\n'
        for s in "${modules[@]}"; do printf '    {"%s", PyInit_%s},\n' "$s" "${s//./_}"; done
        printf '    {NULL, NULL},\n};\n\nvoid renpy_extend_inittab(void)\n{\n    PyImport_ExtendInittab(renpy_modules);\n}\n'
    } >"$out/gen/inittab.c"
}

build_modules() {
    echo "==> [renpy] Ren'Py's modules"
    local src="$out/renpy-src" objects="$out/modules" generated="$out/renpy-src/tmp/gen3-static"
    rm -rf "$objects"
    mkdir -p "$objects"
    : >"$log/modules.log"
    # this build's own headers first: the build machine may have other versions (Homebrew's FFmpeg)
    local cflags=(-O2 -g -fPIC -w -fno-strict-aliasing -include "$root/src/core/renpy/sdl_renames.h"
        -I"$src/src" -I"$generated" -I"$prefix/include" $("$out/python/bin/python3.12-config" --includes)
        $(sdl_cflags))
    # compile every source once, $jobs at a time
    local pids=() objs=() f o
    while read -r f; do
        o="$objects/$(basename "${f%.*}").o"
        objs+=("$o")
        if [[ $f == *.cc ]]; then
            "$cxx" -std=c++17 "${cflags[@]}" -c "$f" -o "$o" >>"$log/modules.log" 2>&1 &
        else
            "$cc" -std=gnu99 "${cflags[@]}" -c "$f" -o "$o" >>"$log/modules.log" 2>&1 &
        fi
        pids+=($!)
        if (( ${#pids[@]} >= jobs )); then wait "${pids[0]}" || fail modules; pids=("${pids[@]:1}"); fi
    done <"$out/sources.txt"
    for f in "${pids[@]}"; do wait "$f" || fail modules; done
    rm -f "$out/librenpy-modules.a"
    ar rcs "$out/librenpy-modules.a" "${objs[@]}"
}

# ---- the engine folder (renpy8/): Ren'Py's Python (compiled; the app folder may be read-only), Ren'Py's common
# scripts and fonts, renpy.py, the boot script, the standard library (compiled, in an uncompressed zip, so that
# zipimport needs nothing) and the pure-Python packages of tools/renpy/wheels.txt.
build_engine() {
    echo "==> [renpy] the engine folder"
    local src="$out/renpy-src" engine="$out/assets/renpy8"
    rm -rf "$engine"
    mkdir -p "$engine/lib"
    cp -R "$src/renpy" "$engine/renpy"
    find "$engine/renpy" \( -name '*.pyx' -o -name '*.pxd' -o -name '*.pxi' -o -name '*.c' -o -name '*.c.dynamic' \
        -o -name '*.cc' -o -name '*.h' \) -delete
    cp "$src/renpy.py" "$engine/renpy.py"
    # what Ren'Py's own distribution writes: without it Ren'Py looks for its version in a git checkout
    printf "branch = 'fix'\nnightly = False\nofficial = False\nversion = '%s'\nversion_name = '%s'\n" \
        "$renpy_version" "$renpy_version_name" >"$engine/renpy/vc_version.py"
    cp "$root/src/core/renpy/boot.py" "$engine/dulcamara_boot.py"
    cp "$root/src/core/renpy/errors.py" "$engine/dulcamara_errors.py"
    unpack_wheels engine "$engine/lib/site-packages"
    "$py" -m compileall -q -j 1 --invalidation-mode unchecked-hash "$engine/renpy" "$engine/lib/site-packages" \
        >"$log/engine.log" 2>&1 || fail engine
    "$py" - "$out/python/lib/python3.12" "$engine/lib/python312.zip" <<'PY' >>"$log/engine.log" 2>&1 || fail engine
import importlib.util, pathlib, py_compile, sys, tempfile, zipfile
stdlib, zip_path = pathlib.Path(sys.argv[1]), sys.argv[2]
skip = {"test", "idlelib", "tkinter", "turtledemo", "ensurepip", "lib2to3", "pydoc_data", "site-packages",
        "lib-dynload", "__phello__", "venv"}
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_STORED) as z, tempfile.TemporaryDirectory() as tmp:
    for source in sorted(stdlib.rglob("*.py")):
        relative = source.relative_to(stdlib)
        if relative.parts[0] in skip or relative.parts[0].startswith("config-"):
            continue
        compiled = pathlib.Path(tmp) / "m.pyc"
        py_compile.compile(str(source), cfile=str(compiled), dfile=str(relative), doraise=True,
                           invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
        z.write(compiled, str(relative.with_suffix(".pyc")))
PY
}

generate_key="generate-2 $(git -C "$renpy" rev-parse HEAD) $python_key $(recipe build_generate)"
if ! fresh generate "$generate_key"; then
    build_generate
    stamp generate "$generate_key"
fi
engine_key="engine-3 $renpy_version $generate_key $(recipe build_engine) $(
    digest "$root"/src/core/renpy/{boot,errors}.py "$root/tools/renpy/wheels.txt")"
if ! fresh engine "$engine_key"; then
    build_engine
    stamp engine "$engine_key"
fi
if [[ $target == engine ]]; then
    echo "==> [renpy] the engine in $out/assets/renpy8, and Ren'Py's modules as C"
    exit 0
fi
modules_key="modules-5 $generate_key $libs_key $(recipe build_modules sdl_cflags) $(
    digest "$root/src/core/renpy/sdl_renames.h")"
if ! fresh modules "$modules_key"; then
    build_modules
    stamp modules "$modules_key"
fi
echo "==> [renpy] $(wc -l <"$out/modules.txt" | tr -d ' ') Ren'Py modules built in, the engine in $out/assets/renpy8"

# ---- the core and its checks: always rebuilt (a few seconds). The core's own C is warning-free.
libretro="$root/third_party/easyrpg-player/builds/libretro/libretro-common/include"
[[ -f $libretro/libretro.h ]] || { echo "libretro.h is missing: run 'git submodule update --init third_party/easyrpg-player'" >&2; exit 2; }
check="$out/core-check"
mkdir -p "$check"
own_flags=(-std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -O1 -g -Wall -Wextra -Werror -pthread)
if [[ $(uname -s) == Darwin ]]; then
    gl_backend="$root/src/core/renpy/gl_cgl.c"
    gl_libs=(-framework OpenGL)
else
    gl_backend="$root/src/core/renpy/gl_egl.c"
    gl_libs=($(pkg-config --libs egl))
fi
echo "==> [renpy] the core's checks"
"$cc" "${own_flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=all \
    "$root/tests/core/renpy_unit.c" "$root"/src/core/renpy/{handoff,frames,audio,gl,log}.c "$gl_backend" \
    "${gl_libs[@]}" -lm -o "$check/renpy-unit" >"$log/core.log" 2>&1 || fail core

echo "==> [renpy] the core and its headless frontend"
objects=()
for f in core python hooks input log handoff frames audio gl; do
    "$cc" "${own_flags[@]}" -I"$libretro" $("$out/python/bin/python3.12-config" --includes) $(sdl_cflags) \
        -c "$root/src/core/renpy/$f.c" -o "$check/$f.o" >>"$log/core.log" 2>&1 || fail core
    objects+=("$check/$f.o")
done
"$cc" "${own_flags[@]}" -c "$gl_backend" -o "$check/gl_backend.o" >>"$log/core.log" 2>&1 || fail core
"$cc" -O1 -g -w $("$out/python/bin/python3.12-config" --includes) -c "$out/gen/inittab.c" -o "$check/inittab.o" \
    >>"$log/core.log" 2>&1 || fail core
"$cc" "${own_flags[@]}" -I"$libretro" -c "$root/tests/core/renpy_headless.c" -o "$check/renpy_headless.o" \
    >>"$log/core.log" 2>&1 || fail core
# What Python's built-in modules link (python3.12-config leaves them out): its LOCALMODLIBS, but for the two
# archives of its own build, which python/lib/modules holds. Before the other libraries, so that their folders
# (Homebrew's lib, which may hold another OpenSSL) come after the ones Python was built with.
python_modlibs=$("$py" -c "import sysconfig; print(' '.join(w for w in sysconfig.get_config_var('LOCALMODLIBS').split() if not w.endswith('.a')))")
# Ren'Py's modules, then what they use: FFmpeg, Assimp, SDL2_image and SDL2, the text libraries, Python.
"$cxx" -o "$check/renpy-headless" "$check/renpy_headless.o" "${objects[@]}" "$check/gl_backend.o" \
    "$check/inittab.o" "$out/librenpy-modules.a" \
    -L"$prefix/lib" -lavformat -lavcodec -lswscale -lswresample -lavutil -lassimp \
    $("$out/python/bin/python3.12-config" --embed --ldflags) "$out"/python/lib/modules/*.a $python_modlibs \
    $(pkg-config --libs SDL2_image sdl2 freetype2 harfbuzz fribidi libpng libjpeg libwebp zlib) \
    "${gl_libs[@]}" -pthread -lm \
    >>"$log/core.log" 2>&1 || fail core
# ---- Ren'Py's common scripts, compiled into the engine by Ren'Py's own compile command, run by the core: the console's
# app folder is read-only, and Ren'Py would otherwise compile them at every start.
compile_key="compile-1 $engine_key"
if ! fresh compile "$compile_key"; then
    echo "==> [renpy] Ren'Py's common scripts, compiled"
    find "$out/assets/renpy8/renpy/common" -name '*.rpyc' -delete
    rm -rf "$out/work/compile"
    mkdir -p "$out/work/compile/game/game"
    printf 'label start:\n    return\n' >"$out/work/compile/game/game/script.rpy"
    DULCAMARA_RENPY_COMMAND=compile "$check/renpy-headless" "$out/assets" compile "$out/work/compile/game" \
        "$out/work/compile/data" >"$log/compile.log" 2>&1 || fail compile
    missing=$(find "$out/assets/renpy8/renpy/common" -name '*.rpy' | while read -r f; do [[ -f ${f}c ]] || echo "$f"; done)
    [[ -z $missing ]] || { echo "Ren'Py did not compile: $missing" >&2; exit 1; }
    stamp compile "$compile_key"
fi
echo "==> [renpy] $check/renpy-headless"
