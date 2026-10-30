#!/usr/bin/env bash
# Dulcamara - builds Ren'Py 7's libretro core (Ren'Py 7.8.7, MIT, with CPython 2.7.18, PSF, and pygame_sdl2, zlib), for
# games made with Ren'Py 7, as tools/build-renpy.sh builds Ren'Py 8's: the same core (src/core/renpy), with Python 2.7
# (python2.c, boot2.py, errors2.py) in place of Python 3.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/build-renpy7.sh       -> build/renpy7-ps5/librenpy7.a, the libretro API renamed renpy7_retro_*, and
#                                  build/renpy7-ps5/assets/renpy7
#   tools/build-renpy7.sh host  -> build/renpy7-host/: the same core for this computer, linked into its headless
#                                  check (run by tests/test_renpy7_core.py), and build/renpy7-host/assets/renpy7
#
# Sources: third_party/renpy7 (Ren'Py 7.8.7, a pinned submodule), third_party/pygame_sdl2 (Ren'Py 7's, pinned),
# CPython 2.7.18 (pinned by SHA-256; with renpy-build's two UTF-8 patches and tools/patches/python-2.7.18-configure.diff
# for arm64 Macs and the PS5), Cython 0.29.32 and setuptools 44.1.1 to write the C, and the pure-Python packages of
# tools/renpy7/wheels.txt. The libraries are Ren'Py 8's build's: FFmpeg, fribidi and SDL2_image from
# build/renpy-host for the host, and FFmpeg, ps5-opengl, the SDK and PacBrew from build/renpy-ps5 for the PS5 (each
# built by tools/build-renpy.sh first).
#
# Each stage is redone only when what it is built from changed (build/renpy7-<target>/.<stage>).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-ps5}
case $target in ps5|host) ;; *)
    echo "usage: tools/build-renpy7.sh [ps5|host]" >&2
    exit 2
esac
# Ren'Py 8's build first: its libraries are this one's. The PS5 build starts from Ren'Py 7's host build: its Python
# runs the build, its generated C and its engine (with Ren'Py's common scripts compiled by the host core) are the
# console's too.
bash "$root/tools/build-renpy.sh" "$target"
[[ $target == ps5 ]] && bash "$root/tools/build-renpy7.sh" host
renpy="$root/third_party/renpy7"
pygame="$root/third_party/pygame_sdl2"
source "$root/tools/renpy/common.sh"
out=$(build_folder "renpy7-$target")
renpy8=$(cd "$root/build/renpy-$target" && pwd -P)
prefix="$renpy8/prefix" # Ren'Py 8's build's libraries
log="$out/logs"
cc=${CC:-cc}
cxx=${CXX:-c++}

renpy_version=7.8.7.25031702 # third_party/renpy7's release
renpy_version_name="Straight on Till Morning"
python_version=2.7.18
python_hash=da3080e3b488f648a3d7a4560ddee895284c3380b11d6de75edb986526b9a814

[[ -f $renpy/renpy/__init__.py && -f $renpy/module/Setup ]] || {
    echo "Ren'Py 7's source is missing: run 'git submodule update --init third_party/renpy7'" >&2
    exit 2
}
[[ -f $pygame/Setup ]] || {
    echo "pygame_sdl2's source is missing: run 'git submodule update --init third_party/pygame_sdl2'" >&2
    exit 2
}
mkdir -p "$out" "$log" "$out/bin"

# the include folders of Ren'Py's and pygame_sdl2's C, from the folder that holds the two sources
# (the host build's: the PS5 build compiles its generated C)
module_includes() {
    printf -- '-I%s ' "$1/renpy-src/module" "$1/renpy-src/module/gen-static" "$1/pygame_sdl2-src" \
        "$1/pygame_sdl2-src/src" "$1/pygame_sdl2-src/gen-static" "$1/deps/include"
}

if [[ $target == ps5 ]]; then
    source "$root/tools/renpy7/ps5.sh"
    exit 0
fi

# The build machine's libraries, as Ren'Py 8's build finds them.
pc_path="$prefix/lib/pkgconfig"
if [[ $(uname -s) == Darwin ]] && command -v brew >/dev/null; then
    for keg in jpeg-turbo xz; do
        keg_dir=$(brew --prefix "$keg" 2>/dev/null) || continue
        [[ -d $keg_dir/lib/pkgconfig ]] && pc_path="$pc_path:$keg_dir/lib/pkgconfig"
    done
fi
export PKG_CONFIG_PATH="$pc_path${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
command -v sdl2-config >/dev/null || { echo "sdl2-config is required (pygame_sdl2's setup.py runs it)" >&2; exit 2; }

# cflags_known <flag>...: the ones this compiler takes (gcc refuses the -Wno-error= of warnings it lacks)
cflags_known() {
    local f
    for f in "$@"; do
        echo 'int x;' | "$cc" -Werror "$f" -x c -c - -o /dev/null 2>/dev/null && printf '%s ' "$f"
    done
}

# ---- CPython 2.7 (PSF): static, with the modules of tools/renpy7/Setup.local built in. On the build machine it is
# also the Python that runs Cython and the setup.py of Ren'Py and pygame_sdl2. Today's compilers make errors of what
# 2.7's C was written with (implicit declarations, pointer conversions): they stay warnings.
python_cflags() {
    cflags_known -Wno-error=implicit-function-declaration -Wno-error=int-conversion \
        -Wno-error=incompatible-function-pointer-types -Wno-error=incompatible-pointer-types
}
build_python() {
    echo "==> [renpy7] CPython $python_version"
    fetch "https://www.python.org/ftp/python/$python_version/Python-$python_version.tgz" \
        "Python-$python_version.tgz" "$python_hash"
    unpack "Python-$python_version.tgz" "$out/work/python"
    rm -rf "$out/python"
    local src="$out/work/python/Python-$python_version" p
    for p in "$root"/tools/patches/python-2.7.18-*.diff; do
        patch -d "$src" -p1 <"$p" >>"$log/python.log" 2>&1 || fail python
    done
    cp "$root/tools/renpy7/Setup.local" "$src/Modules/Setup.local"
    # only here, as in renpy-build's host Python: setuptools imports ctypes (the engine's Python has none)
    printf '\n_ctypes _ctypes/_ctypes.c _ctypes/callbacks.c _ctypes/callproc.c _ctypes/stgdict.c _ctypes/cfield.c %s\n' \
        "$(pkg-config --cflags --libs libffi)" >>"$src/Modules/Setup.local"
    # urllib imports it on macOS (future's aliases, as Ren'Py starts)
    [[ $(uname -s) == Darwin ]] && printf '_scproxy ../Mac/Modules/_scproxy.c %s\n' \
        "-framework SystemConfiguration -framework CoreFoundation" >>"$src/Modules/Setup.local"
    (cd "$src" && ./configure --prefix="$out/python" --disable-shared --enable-unicode=ucs4 --without-ensurepip \
        CC="$cc" CFLAGS="$(python_cflags)") >>"$log/python.log" 2>&1 || fail python
    { make -C "$src" -j"$jobs" && make -C "$src" install; } >>"$log/python.log" 2>&1 || fail python
    # The engine has no lib-dynload: every C module Ren'Py imports must be built in.
    "$out/python/bin/python2.7" - <<'PY'
import sys
need = {"_bisect", "_codecs_jp", "_collections", "_csv", "_ctypes", "_functools", "_heapq", "_io", "_json", "_locale",
        "_md5", "_multibytecodec", "_random", "_sha", "_sha256", "_sha512", "_socket", "_struct", "array", "binascii",
        "bz2", "cPickle", "cStringIO", "cmath", "datetime", "fcntl", "itertools", "math", "mmap", "operator",
        "pyexpat", "select", "strop", "time", "unicodedata", "zlib"}
missing = need - set(sys.builtin_module_names)
if missing:
    sys.exit("CPython was built without " + " ".join(sorted(missing)))
PY
}

# ---- the packages of tools/renpy7/wheels.txt of one kind, unpacked into a folder (no pip: they are pure Python). A
# source archive (future's) gives the packages of its src folder, and its licence goes into their egg-info. For the
# engine, the "license" lines then give the wheels without a licence file theirs.
unpack_wheels() {
    local kind=$1 into=$2 k hash url member info name top
    rm -rf "$into" "$out/work/sdist"
    mkdir -p "$into" "$out/work/sdist"
    while read -r k hash url member info; do
        [[ -z $k || $k == \#* ]] && continue
        [[ $k == "$kind" || ($kind == engine && $k == license) ]] || continue
        name=${url##*/}
        fetch "$url" "$name" "$hash"
        if [[ $k == license ]]; then
            [[ -d $into/$info ]] || { echo "no $info for ${member##*/}" >&2; exit 1; }
            tar -xzf "$cache/$name" -C "$out/work/sdist" "$member"
            cp "$out/work/sdist/$member" "$into/$info/"
        elif [[ $name == *.tar.gz ]]; then
            tar -xzf "$cache/$name" -C "$out/work/sdist"
            top="$out/work/sdist/${name%.tar.gz}"
            cp -R "$top/src/." "$into/"
            rm -f "$into/__init__.py" # the src folder's own
            for info in "$top"/src/*.egg-info; do cp "$top"/LICENSE* "$into/${info##*/}/"; done
        else
            "$py" -m zipfile -e "$cache/$name" "$into"
        fi
    done <"$root/tools/renpy7/wheels.txt"
    rm -rf "$out/work/sdist"
}

python_key="python-3 $python_version $(digest "$root/tools/renpy7/Setup.local" "$root/tools/renpy7/wheels.txt" \
    "$root"/tools/patches/python-2.7.18-*.diff)"
py="$out/python/bin/python2.7"
if ! fresh python "$python_key"; then
    build_python
    unpack_wheels build "$out/tools"
    printf '#!/bin/sh\nPYTHONPATH="%s" exec "%s" -m cython "$@"\n' "$out/tools" "$py" >"$out/bin/cython"
    chmod +x "$out/bin/cython"
    stamp python "$python_key"
fi

# ---- pygame_sdl2's and Ren'Py's own C: their setup.py turn their .pyx into C for a static build (PYGAME_SDL2_STATIC,
# RENPY_STATIC: each module's init function named after its whole dotted name). Ren'Py's needs pygame_sdl2's .pxd
# (found on PYTHONPATH) and its setup.py only checks that the libraries it would link are there: deps/lib holds a
# stand-in of each, as this build links them itself. Left out, as from Ren'Py 8's build: renpy.gl2.live2dmodel
# (Live2D's Cubism SDK) and _renpytfd (desktop file dialogs, in Setup.tfd).
build_generate() {
    echo "==> [renpy7] pygame_sdl2's and Ren'Py's modules as C (Cython $("$out/bin/cython" --version 2>&1 | awk '{print $NF}'))"
    local psrc="$out/pygame_sdl2-src" rsrc="$out/renpy-src" deps="$out/deps" lib
    rm -rf "$psrc" "$rsrc" "$deps" "$out/gen"
    mkdir -p "$psrc" "$rsrc" "$deps/lib" "$out/gen"
    git -C "$pygame" archive HEAD | tar -x -C "$psrc"
    git -C "$renpy" archive HEAD | tar -x -C "$rsrc"
    # what Ren'Py's own distribution writes (Ren'Py's setup.py imports Ren'Py, which reads it)
    printf "branch = 'fix'\nnightly = False\nofficial = False\nversion = '%s'\nversion_name = '%s'\n" \
        "$renpy_version" "$renpy_version_name" >"$rsrc/renpy/vc_version.py"
    (cd "$psrc" && PYGAME_SDL2_STATIC=1 PATH="$out/bin:$PATH" PYTHONPATH="$out/tools" CC="$cc" \
        "$py" setup.py generate) >"$log/generate.log" 2>&1 || fail generate
    for lib in SDL2 png freetype z avformat avcodec avutil swresample swscale fribidi; do
        : >"$deps/lib/lib$lib.a"
    done
    (cd "$rsrc/module" && RENPY_STATIC=1 RENPY_CYTHON="$out/bin/cython" RENPY_DEPS_INSTALL="$deps" \
        PYTHONPATH="$psrc/src:$out/tools" CC="$cc" "$py" setup.py generate) >>"$log/generate.log" 2>&1 ||
        fail generate
    [[ -d $psrc/gen-static && -d $rsrc/module/gen-static ]] || {
        echo "the setup.py of pygame_sdl2 or Ren'Py did not write its gen-static" >&2
        exit 1
    }
    # pygame_sdl2's headers, which Ren'Py's C includes as <pygame_sdl2/...>
    mkdir -p "$deps/include/pygame_sdl2"
    cp "$psrc/src/pygame_sdl2/pygame_sdl2.h" "$psrc"/gen-static/pygame_sdl2.{rwobject,surface,display}_api.h \
        "$deps/include/pygame_sdl2/"
    local modules=() sources=() line parts s dir
    for dir in "$psrc" "$rsrc/module"; do
        while read -r line; do
            line=${line%%#*}
            read -r -a parts <<<"$line"
            [[ ${#parts[@]} -eq 0 || ${parts[0]} == renpy.gl2.live2dmodel ]] && continue
            modules+=("${parts[0]}")
            for s in "${parts[@]:1}"; do
                sources+=("$dir/${s/#gen\//gen-static/}")
            done
        done <"$dir/Setup"
    done
    printf '%s\n' "${modules[@]}" >"$out/modules.txt"
    printf '%s\n' "${sources[@]}" | sort -u >"$out/sources.txt" # the PS5 build compiles the same files
    {
        printf '/* Dulcamara - written by tools/build-renpy7.sh: the engine'"'"'s compiled modules, built in. */\n'
        printf '#include <Python.h>\n\n'
        for s in "${modules[@]}"; do printf 'PyMODINIT_FUNC init%s(void);\n' "${s//./_}"; done
        printf '\nstatic struct _inittab renpy_modules[] = {\n'
        for s in "${modules[@]}"; do printf '    {"%s", init%s},\n' "$s" "${s//./_}"; done
        printf '    {NULL, NULL},\n};\n\nvoid renpy_extend_inittab(void)\n{\n    PyImport_ExtendInittab(renpy_modules);\n}\n'
    } >"$out/gen/inittab.c"
}

sdl_cflags() { pkg-config --cflags sdl2 SDL2_image freetype2 harfbuzz fribidi libpng libjpeg libwebp zlib; }
build_modules() {
    echo "==> [renpy7] Ren'Py's and pygame_sdl2's modules"
    local objects="$out/modules"
    rm -rf "$objects"
    mkdir -p "$objects"
    : >"$log/modules.log"
    # this build's own headers first: the build machine may have other versions (Homebrew's FFmpeg)
    local cflags=(-O2 -g -fPIC -w -fno-strict-aliasing -include "$root/src/core/renpy/sdl_renames.h"
        $(module_includes "$out") -I"$prefix/include" -I"$out/python/include/python2.7" $(sdl_cflags))
    local pids=() objs=() f o
    while read -r f; do
        o="$objects/$(basename "$(dirname "$f")")-$(basename "${f%.*}").o"
        objs+=("$o")
        "$cc" -std=gnu99 "${cflags[@]}" -c "$f" -o "$o" >>"$log/modules.log" 2>&1 &
        pids+=($!)
        if (( ${#pids[@]} >= jobs )); then wait "${pids[0]}" || fail modules; pids=("${pids[@]:1}"); fi
    done <"$out/sources.txt"
    for f in "${pids[@]}"; do wait "$f" || fail modules; done
    rm -f "$out/librenpy7-modules.a"
    ar rcs "$out/librenpy7-modules.a" "${objs[@]}"
}

# ---- the engine folder (renpy7/): Ren'Py's and pygame_sdl2's Python, Ren'Py's common scripts and fonts, renpy.py,
# the boot script, the standard library (in an uncompressed zip) and the pure-Python packages of
# tools/renpy7/wheels.txt. The Python is compiled and its source left out (but Ren'Py's renpy.py and
# renpy/__init__.py, which the core looks for): Python 2 checks a compiled file against its source's time, which
# copying the app may change, and the app folder may be read-only. renpy/common stays as it is: its _ren.py files
# are scripts, which Ren'Py compiles with its others.
build_engine() {
    echo "==> [renpy7] the engine folder"
    local rsrc="$out/renpy-src" psrc="$out/pygame_sdl2-src" engine="$out/assets/renpy7"
    rm -rf "$engine"
    mkdir -p "$engine/lib"
    cp -R "$rsrc/renpy" "$engine/renpy"
    find "$engine/renpy" \( -name '*.pyx' -o -name '*.pxd' -o -name '*.pxi' -o -name '*.c' -o -name '*.h' \) -delete
    cp "$rsrc/renpy.py" "$engine/renpy.py"
    mkdir -p "$engine/pygame_sdl2"
    (cd "$psrc/src/pygame_sdl2" && find . -name '*.py' | while read -r f; do
        mkdir -p "$engine/pygame_sdl2/$(dirname "$f")"
        cp "$f" "$engine/pygame_sdl2/$f"
    done)
    cp "$root/src/core/renpy/boot2.py" "$engine/dulcamara_boot.py"
    cp "$root/src/core/renpy/errors2.py" "$engine/dulcamara_errors.py"
    unpack_wheels engine "$engine/lib/site-packages"
    find "$engine/lib/site-packages" -name '*.pyc' -delete
    "$py" -m compileall -q -x '/renpy/common/' "$engine/renpy" "$engine/pygame_sdl2" "$engine/lib/site-packages" \
        "$engine/dulcamara_errors.py" >"$log/engine.log" 2>&1 || fail engine
    find "$engine/renpy" "$engine/pygame_sdl2" "$engine/lib/site-packages" "$engine/dulcamara_errors.py" -name '*.py' \
        ! -path "$engine/renpy/__init__.py" ! -path "$engine/renpy/common/*" | while read -r f; do
        [[ -f ${f}c ]] && rm "$f"
    done
    "$py" - "$out/python/lib/python2.7" "$engine/lib/python27.zip" <<'PY' >>"$log/engine.log" 2>&1 || fail engine
import os, py_compile, sys, tempfile, zipfile
stdlib, zip_path = sys.argv[1], sys.argv[2]
skip = {"test", "idlelib", "lib-tk", "lib2to3", "ensurepip", "pydoc_data", "site-packages", "lib-dynload", "bsddb",
        "curses", "msilib", "sqlite3", "hotshot", "distutils"}
compiled = os.path.join(tempfile.mkdtemp(), "m.pyc")
z = zipfile.ZipFile(zip_path, "w", zipfile.ZIP_STORED)
for folder, folders, files in os.walk(stdlib):
    relative_folder = os.path.relpath(folder, stdlib)
    top = relative_folder.split(os.sep)[0]
    if top in skip or top.startswith("config") or top.startswith("plat-"):
        folders[:] = []
        continue
    folders.sort()
    for name in sorted(files):
        if not name.endswith(".py"):
            continue
        relative = os.path.normpath(os.path.join(relative_folder, name))
        py_compile.compile(os.path.join(folder, name), cfile=compiled, dfile=relative, doraise=True)
        z.write(compiled, relative[:-3] + ".pyc")
z.close()
PY
}

generate_key="generate-2 $(git -C "$renpy" rev-parse HEAD) $(git -C "$pygame" rev-parse HEAD) $python_key"
if ! fresh generate "$generate_key"; then
    build_generate
    stamp generate "$generate_key"
fi
engine_key="engine-2 $renpy_version $generate_key $(digest "$root"/src/core/renpy/{boot2,errors2}.py "$root/tools/renpy7/wheels.txt")"
if ! fresh engine "$engine_key"; then
    build_engine
    stamp engine "$engine_key"
fi
libs_key=$(cat "$renpy8/.libs")
modules_key="modules-1 $generate_key $libs_key $(digest "$root/src/core/renpy/sdl_renames.h")"
if ! fresh modules "$modules_key"; then
    build_modules
    stamp modules "$modules_key"
fi
echo "==> [renpy7] $(wc -l <"$out/modules.txt" | tr -d ' ') Ren'Py 7 and pygame_sdl2 modules built in, the engine in $out/assets/renpy7"

# ---- the core and its headless frontend: always rebuilt (a few seconds). The core's own C is warning-free.
libretro="$root/third_party/easyrpg-player/builds/libretro/libretro-common/include"
[[ -f $libretro/libretro.h ]] || { echo "libretro.h is missing: run 'git submodule update --init third_party/easyrpg-player'" >&2; exit 2; }
check="$out/core-check"
mkdir -p "$check"
own_flags=(-std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -O1 -g -Wall -Wextra -Werror -pthread)
core_flags=(-DDR_ENGINE_FOLDER='"renpy7"' -DDR_RENPY_VERSION='"7.8.7"')
if [[ $(uname -s) == Darwin ]]; then
    gl_backend="$root/src/core/renpy/gl_cgl.c"
    gl_libs=(-framework OpenGL)
else
    gl_backend="$root/src/core/renpy/gl_egl.c"
    gl_libs=($(pkg-config --libs egl))
fi
echo "==> [renpy7] the core and its headless frontend"
: >"$log/core.log"
objects=()
for f in core python2 hooks input log handoff frames audio gl; do
    # Python 2's headers are not -Wextra clean: they are system headers here
    "$cc" "${own_flags[@]}" "${core_flags[@]}" -I"$libretro" -isystem "$out/python/include/python2.7" $(sdl_cflags) \
        -c "$root/src/core/renpy/$f.c" -o "$check/$f.o" >>"$log/core.log" 2>&1 || fail core
    objects+=("$check/$f.o")
done
"$cc" "${own_flags[@]}" -c "$gl_backend" -o "$check/gl_backend.o" >>"$log/core.log" 2>&1 || fail core
"$cc" -O1 -g -w -I"$out/python/include/python2.7" -c "$out/gen/inittab.c" -o "$check/inittab.o" \
    >>"$log/core.log" 2>&1 || fail core
"$cc" "${own_flags[@]}" -I"$libretro" -c "$root/tests/core/renpy_headless.c" -o "$check/renpy_headless.o" \
    >>"$log/core.log" 2>&1 || fail core
# What Python's built-in modules link (LOCALMODLIBS: zlib, bzip2), then the libraries Ren'Py's modules use.
python_libs=$("$py" -c "import sysconfig; v = sysconfig.get_config_var; print(' '.join([v('LOCALMODLIBS') or '', v('LIBS') or '', v('SYSLIBS') or '']))")
"$cxx" -o "$check/renpy-headless" "$check/renpy_headless.o" "${objects[@]}" "$check/gl_backend.o" \
    "$check/inittab.o" "$out/librenpy7-modules.a" \
    -L"$prefix/lib" -lavformat -lavcodec -lswscale -lswresample -lavutil \
    "$out/python/lib/libpython2.7.a" $python_libs \
    $(pkg-config --libs SDL2_image sdl2 freetype2 harfbuzz fribidi libpng libjpeg libwebp zlib) \
    "${gl_libs[@]}" -pthread -lm \
    >>"$log/core.log" 2>&1 || fail core
# ---- Ren'Py's common scripts, compiled into the engine by Ren'Py's own compile command, run by the core.
compile_key="compile-1 $engine_key"
if ! fresh compile "$compile_key"; then
    echo "==> [renpy7] Ren'Py's common scripts, compiled"
    find "$out/assets/renpy7/renpy/common" -name '*.rpyc' -delete
    rm -rf "$out/work/compile"
    mkdir -p "$out/work/compile/game/game"
    printf 'label start:\n    return\n' >"$out/work/compile/game/game/script.rpy"
    DULCAMARA_RENPY_COMMAND=compile "$check/renpy-headless" "$out/assets" compile "$out/work/compile/game" \
        "$out/work/compile/data" >"$log/compile.log" 2>&1 || fail compile
    missing=$(find "$out/assets/renpy7/renpy/common" -name '*.rpy' | while read -r f; do [[ -f ${f}c ]] || echo "$f"; done)
    [[ -z $missing ]] || { echo "Ren'Py did not compile: $missing" >&2; exit 1; }
    stamp compile "$compile_key"
fi
echo "==> [renpy7] $check/renpy-headless"
