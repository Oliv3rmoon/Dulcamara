# Dulcamara - the PS5 half of tools/build-renpy.sh, which sources it with its variables and helpers (root, out,
# prefix, cache, log, jobs, fetch, unpack, fresh, stamp, digest, fail, the versions). It cross-builds the core of
# build/renpy-host for the PS5: the host build gives it the Python that runs the build, Cython's C for Ren'Py's
# modules and the engine folder, which are the same for the console.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   build/renpy-ps5/librenpy.a      the core: one relocatable object, every symbol local but the libretro API, which
#                                   is renamed renpy_retro_* (as tools/build-easyrpg.sh does for EasyRPG Player)
#   build/renpy-ps5/assets/renpy8   the engine, shipped beside eboot.bin
#
# The compiler is the payload SDK's prospero-clang (it links test programs, which autoconf's checks need), from a copy
# of the SDK under the build folder, whose path has no spaces. PacBrew gives SDL2, SDL2_image, FreeType, HarfBuzz,
# fribidi, libpng, libjpeg, libwebp, zlib, bzip2, xz and OpenSSL; ps5-opengl 1.0.2 (the SDK Nightshade pins) gives
# OpenGL. Both are inside the core's object, so their symbols cannot meet the app's (its RADV is Mesa too).

host=$(cd "$root/build/renpy-host" && pwd -P)
hostpy="$host/python/bin/python3.12"
[[ -x $hostpy && -f $host/gen/inittab.c && -d $host/assets/renpy8 ]] || {
    echo "the host build is incomplete: run tools/build-renpy.sh host" >&2
    exit 1
}

opengl_version=1.0.2
opengl_hash=30d8e51dd062739b629346102be5932728ba714bc50b1bdcdf08306c8843d9a4

# ---- the tools: LLVM (CI's clang-18, or Homebrew's), the payload SDK, PacBrew
if command -v llvm-config-18 >/dev/null; then
    llvm_bin=$(llvm-config-18 --bindir)
elif [[ -x /opt/homebrew/opt/llvm/bin/llvm-config ]]; then
    llvm_bin=/opt/homebrew/opt/llvm/bin
elif command -v llvm-config >/dev/null; then
    llvm_bin=$(llvm-config --bindir)
else
    echo "LLVM is required for the PS5 build (clang-18 and its tools)" >&2
    exit 2
fi
export PATH="$llvm_bin:$PATH"
lld=$(command -v ld.lld) || { echo "ld.lld is required for the PS5 build" >&2; exit 2; }
ar="$llvm_bin/llvm-ar"
ranlib="$llvm_bin/llvm-ranlib"
nm="$llvm_bin/llvm-nm"
objcopy="$llvm_bin/llvm-objcopy"

sdk_source="${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}"
[[ -x $sdk_source/bin/prospero-clang ]] || bash "$root/tools/setup-native-dependencies.sh" >/dev/null
sdk="$out/sdk"
sdk_key="sdk-1 $(cat "$sdk_source"/bin/prospero-clang "$sdk_source"/target/include/stdio.h | sha256 | cut -c1-16)"
if ! fresh sdk "$sdk_key"; then
    rm -rf "$sdk"
    cp -R "$sdk_source" "$sdk"
    stamp sdk "$sdk_key"
fi
ln -sfn "$(bash "$root/tools/setup-pacbrew-dependencies.sh" --all)" "$out/pacbrew"
hb="$out/pacbrew/user/homebrew"
export PKG_CONFIG_LIBDIR="$hb/lib/pkgconfig:$hb/libdata/pkgconfig" PKG_CONFIG_PATH="" PKG_CONFIG_SYSROOT_DIR="$out/pacbrew"
cc="$sdk/bin/prospero-clang"
cxx="$sdk/bin/prospero-clang++"
ps5_flags="-O2 -fPIC -march=znver2 -ffunction-sections -fdata-sections"
unset CC CXX CFLAGS CXXFLAGS LDFLAGS
# what every stage is built with: the SDK, PacBrew's release (its folder's name) and the flags
toolchain_key="$sdk_key $(basename "$(dirname "$(readlink "$out/pacbrew")")") $ps5_flags"
echo "$toolchain_key" >"$out/.toolchain" # for Ren'Py 7's build

# ---- CPython for the PS5: a FreeBSD host (its headers are FreeBSD's) with the console's libc, the modules of
# tools/renpy/Setup.local. Only the library is built: Python itself runs on the build machine (the host build's).
build_python_ps5() {
    echo "==> [renpy ps5] CPython $python_version"
    fetch "https://www.python.org/ftp/python/$python_version/Python-$python_version.tar.xz" \
        "Python-$python_version.tar.xz" "$python_hash"
    unpack "Python-$python_version.tar.xz" "$out/work/python"
    rm -rf "$out/python"
    local src="$out/work/python/Python-$python_version"
    patch -d "$src" -p1 <"$root/tools/patches/python-3.12.8-cross-freebsd.diff" >"$log/python.log" 2>&1 ||
        fail python
    cp "$root/tools/renpy/Setup.local" "$src/Modules/Setup.local"
    # cross answers: no pseudo-terminals; getaddrinfo is the console's (the core stands in for it, ps5_libc.c)
    printf 'ac_cv_file__dev_ptmx=no\nac_cv_file__dev_ptc=no\nac_cv_buggy_getaddrinfo=no\n' >"$src/config.site"
    (cd "$src" && CONFIG_SITE=config.site ./configure --host=x86_64-unknown-freebsd11 --build="$(./config.guess)" \
        --prefix="$out/python" --disable-shared --with-ensurepip=no --disable-test-modules \
        --with-build-python="$hostpy" --with-openssl="$hb" CC="$cc" CFLAGS="$ps5_flags" CPPFLAGS="-I$hb/include" \
        LDFLAGS="-L$hb/lib" AR="$ar" RANLIB="$ranlib" READELF="$llvm_bin/llvm-readelf") \
        >>"$log/python.log" 2>&1 || fail python
    make -C "$src" -j"$jobs" libpython3.12.a Modules/expat/libexpat.a Modules/_hacl/libHacl_Hash_SHA2.a \
        >>"$log/python.log" 2>&1 || fail python
    mkdir -p "$out/python/include/python3.12" "$out/python/lib"
    cp -R "$src/Include/." "$out/python/include/python3.12/"
    cp "$src/pyconfig.h" "$out/python/include/python3.12/"
    cp "$src/libpython3.12.a" "$src/Modules/expat/libexpat.a" "$src/Modules/_hacl/libHacl_Hash_SHA2.a" \
        "$out/python/lib/"
}

# ---- FFmpeg for the PS5: milestone 1's options, cross-compiled; x86 assembly when nasm is there to build it.
build_ffmpeg_ps5() {
    echo "==> [renpy ps5] FFmpeg $ffmpeg_version"
    fetch "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.gz" "ffmpeg-$ffmpeg_version.tar.gz" "$ffmpeg_hash"
    unpack "ffmpeg-$ffmpeg_version.tar.gz" "$out/work/ffmpeg"
    local src="$out/work/ffmpeg/ffmpeg-$ffmpeg_version" p asm=(--disable-x86asm)
    : >"$log/ffmpeg.log"
    for p in "$root"/tools/patches/ffmpeg-4.3.1-*.diff; do
        patch -d "$src" -p1 <"$p" >>"$log/ffmpeg.log" 2>&1 || fail ffmpeg
    done
    command -v nasm >/dev/null && asm=()
    local decoders=flac,mp2,mp3,mp3on4,mpeg1video,mpeg2video,mpegvideo,msmpeg4v1,msmpeg4v2,msmpeg4v3,mpeg4,pcm_dvd
    decoders+=,pcm_s16be,pcm_s16le,pcm_s8,pcm_u16be,pcm_u16le,pcm_u8,theora,vorbis,opus,vp3,vp8,vp9
    local demuxers=au,avi,flac,m4v,matroska,mov,mp3,mpegps,mpegts,mpegtsraw,mpegvideo,ogg,wav
    local parsers=mpegaudio,mpegvideo,mpeg4video,vp3,vp8,vp9
    (cd "$src" && ./configure --prefix="$prefix" --enable-cross-compile --target-os=freebsd --arch=x86_64 \
        --cc="$cc" --cxx="$cxx" --ar="$ar" --ranlib="$ranlib" --nm="$nm" \
        --extra-cflags="$ps5_flags -I$hb/include" --extra-ldflags="-L$hb/lib" \
        --enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-everything \
        --disable-autodetect --disable-network --disable-avdevice --disable-avfilter --disable-postproc \
        "${asm[@]}" --enable-zlib --enable-avcodec --enable-avformat --enable-swresample --enable-swscale \
        --enable-decoder="$decoders" --enable-demuxer="$demuxers" --enable-parser="$parsers") \
        >>"$log/ffmpeg.log" 2>&1 || fail ffmpeg
    grep -q '^#define CONFIG_GPL 0' "$src/config.h" && grep -q '^#define CONFIG_NONFREE 0' "$src/config.h" || {
        echo "FFmpeg was configured with GPL or non-free parts" >&2
        exit 1
    }
    { make -C "$src" -j"$jobs" && make -C "$src" install; } >>"$log/ffmpeg.log" 2>&1 || fail ffmpeg
}

# ---- Assimp for the PS5, through a CMake toolchain like tools/build-easyrpg.sh's (CMake's checks only compile).
build_assimp_ps5() {
    echo "==> [renpy ps5] Assimp $assimp_version"
    fetch "https://github.com/assimp/assimp/archive/refs/tags/v$assimp_version.tar.gz" \
        "assimp-$assimp_version.tar.gz" "$assimp_hash"
    unpack "assimp-$assimp_version.tar.gz" "$out/work/assimp"
    local src="$out/work/assimp/assimp-$assimp_version" generator=()
    command -v ninja >/dev/null && generator=(-G Ninja -DCMAKE_MAKE_PROGRAM="$(command -v ninja)")
    cat >"$out/ps5.cmake" <<EOF
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CROSSCOMPILING ON)
set(CMAKE_C_COMPILER "$cc")
set(CMAKE_CXX_COMPILER "$cxx")
set(CMAKE_AR "$ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "$ranlib" CACHE FILEPATH "")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "$ps5_flags -DNDEBUG")
set(CMAKE_CXX_FLAGS_INIT "$ps5_flags -DNDEBUG -frtti -fexceptions")
set(CMAKE_PREFIX_PATH "$prefix;$hb")
set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH OFF)
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
EOF
    cmake -S "$src" -B "$out/work/assimp/build" "${generator[@]}" -DCMAKE_TOOLCHAIN_FILE="$out/ps5.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_CXX_STANDARD=17 -DASSIMP_NO_EXPORT=ON -DASSIMP_BUILD_TESTS=OFF \
        -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DASSIMP_BUILD_ZLIB=OFF -DZLIB_INCLUDE_DIR="$hb/include" \
        -DZLIB_LIBRARY="$hb/lib/libz.a" -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF \
        -DASSIMP_BUILD_GLTF_IMPORTER=ON -DASSIMP_BUILD_STL_IMPORTER=ON -DASSIMP_WARNINGS_AS_ERRORS=OFF \
        -DASSIMP_INSTALL_PDB=OFF >"$log/assimp.log" 2>&1 || fail assimp
    { cmake --build "$out/work/assimp/build" --parallel "$jobs" && cmake --install "$out/work/assimp/build"; } \
        >>"$log/assimp.log" 2>&1 || fail assimp
}

# ---- ps5-opengl (MIT; Mesa's licences): the SDK, as released.
fetch_opengl() {
    fetch "https://github.com/darkxex/ps5-opengl/releases/download/v$opengl_version/ps5-opengl-sdk-$opengl_version.tar.gz" \
        "ps5-opengl-sdk-$opengl_version.tar.gz" "$opengl_hash"
    rm -rf "$out/ps5-opengl"
    mkdir -p "$out/ps5-opengl"
    tar -xzf "$cache/ps5-opengl-sdk-$opengl_version.tar.gz" -C "$out/ps5-opengl" --strip-components=1
    [[ -f $out/ps5-opengl/sdk/lib/libPS5OpenGL.a ]] || { echo "ps5-opengl's SDK has no libPS5OpenGL.a" >&2; exit 1; }
}

python_key="python-ps5-2 $python_version $toolchain_key $(recipe build_python_ps5) $(
    digest "$root/tools/renpy/Setup.local" "$root/tools/patches/python-3.12.8-cross-freebsd.diff")"
fresh python "$python_key" || { build_python_ps5; stamp python "$python_key"; }
ffmpeg_key="ffmpeg-ps5-2 $ffmpeg_version $toolchain_key $(recipe build_ffmpeg_ps5) $(command -v nasm >/dev/null && echo asm) $(
    digest "$root"/tools/patches/ffmpeg-4.3.1-*.diff)"
fresh ffmpeg "$ffmpeg_key" || { build_ffmpeg_ps5; stamp ffmpeg "$ffmpeg_key"; }
assimp_key="assimp-ps5-2 $assimp_version $toolchain_key $(recipe build_assimp_ps5)"
fresh assimp "$assimp_key" || { build_assimp_ps5; stamp assimp "$assimp_key"; }
opengl_key="opengl-1 $opengl_version"
fresh opengl "$opengl_key" || { fetch_opengl; stamp opengl "$opengl_key"; }

# ---- Ren'Py's modules: the host build's generated C, compiled for the PS5 with the renamed SDL calls.
pc_cflags() { pkg-config --cflags sdl2 SDL2_image freetype2 harfbuzz fribidi libpng libjpeg libwebp zlib; }
build_modules_ps5() {
    echo "==> [renpy ps5] Ren'Py's modules"
    local src="$host/renpy-src" objects="$out/modules"
    rm -rf "$objects"
    mkdir -p "$objects"
    local cflags=($ps5_flags -g0 -w -fno-strict-aliasing -include "$root/src/core/renpy/sdl_renames.h"
        -I"$src/src" -I"$src/tmp/gen3-static" -I"$prefix/include" -I"$out/python/include/python3.12" $(pc_cflags)
        -I"$hb/include") # renpygl.h includes <SDL2/SDL.h>
    local pids=() objs=() f o
    : >"$log/modules.log"
    while read -r f; do
        o="$objects/$(basename "${f%.*}").o"
        objs+=("$o")
        if [[ $f == *.cc ]]; then
            "$cxx" -std=c++17 -frtti -fexceptions "${cflags[@]}" -c "$f" -o "$o" >>"$log/modules.log" 2>&1 &
        else
            "$cc" -std=gnu99 "${cflags[@]}" -c "$f" -o "$o" >>"$log/modules.log" 2>&1 &
        fi
        pids+=($!)
        if (( ${#pids[@]} >= jobs )); then wait "${pids[0]}" || fail modules; pids=("${pids[@]:1}"); fi
    done <"$host/sources.txt"
    for f in "${pids[@]}"; do wait "$f" || fail modules; done
    rm -f "$out/librenpy-modules.a"
    "$ar" rcs "$out/librenpy-modules.a" "${objs[@]}"
}
modules_key="modules-ps5-3 $python_key $(cat "$host/.generate") $(recipe build_modules_ps5 pc_cflags) $(
    digest "$host/sources.txt" "$root/src/core/renpy/sdl_renames.h")"
fresh modules "$modules_key" || { build_modules_ps5; stamp modules "$modules_key"; }

# ---- the core's own C, and everything in one object
echo "==> [renpy ps5] the core"
libretro="$root/third_party/easyrpg-player/builds/libretro/libretro-common/include"
core="$out/core"
rm -rf "$core"
mkdir -p "$core"
core_flags=($ps5_flags -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I"$libretro"
    -I"$out/python/include/python3.12" -I"$out/ps5-opengl/sdk/include" $(pc_cflags))
objects=()
for f in core python hooks input log handoff frames audio gl gl_egl ps5_libc ps5_gl_output ps5_console ps5_threads ps5_trace; do
    "$cc" "${core_flags[@]}" -c "$root/src/core/renpy/$f.c" -o "$core/$f.o" >"$log/core.log" 2>&1 ||
        { cat "$log/core.log" >&2; exit 1; }
    objects+=("$core/$f.o")
done
"$cc" $ps5_flags -w -I"$out/python/include/python3.12" -c "$host/gen/inittab.c" -o "$core/inittab.o" \
    >>"$log/core.log" 2>&1 || fail core
objects+=("$core/inittab.o")

# Every library the core uses goes into the object: only what it references is taken from each. The C and C++
# runtimes, the console's modules (the AGC driver ps5-opengl uses, video out, the pad and sound SDL2 knows) stay
# undefined, for the app's link.
libraries=("$out/python/lib/libpython3.12.a" "$out/python/lib/libexpat.a" "$out/python/lib/libHacl_Hash_SHA2.a"
    "$prefix/lib/libavformat.a" "$prefix/lib/libavcodec.a" "$prefix/lib/libswscale.a" "$prefix/lib/libswresample.a"
    "$prefix/lib/libavutil.a" "$prefix/lib/libassimp.a" "$out/ps5-opengl/sdk/lib/libPS5OpenGL.a")
for name in SDL2_image SDL2 freetype harfbuzz fribidi png16 jpeg webpdemux webp sharpyuv bz2 lzma ssl crypto iconv z; do
    [[ -f $hb/lib/lib$name.a ]] || { echo "PacBrew has no lib$name.a" >&2; exit 1; }
    libraries+=("$hb/lib/lib$name.a")
done
work="$out/isolate"
rm -rf "$work"
mkdir -p "$work"
# chdir goes through the core's own (ps5_libc.c), which keeps the working folder that its getcwd answers with;
# ps5-opengl's TV output is the core's stand-in (ps5_gl_output.c), so the app keeps the screen; what the core's
# libraries print goes to dulcamara.log too (ps5_console.c); their threads get room and a signal stack
# (ps5_threads.c); what ends the app and FFmpeg's steps are traced (ps5_trace.c)
wraps=()
for f in Open Close SetFlipRate RegisterBuffers2 UnregisterBuffers SubmitFlip IsFlipPending WaitVblank GetFlipStatus \
    AddFlipEvent DeleteFlipEvent IsOutputSupported ConfigureOutput GetOutputStatus GetResolutionStatus; do
    wraps+=("--wrap=sceVideoOut$f")
done
for f in printf puts fprintf fputs fwrite pthread_create abort exit _exit _Exit avformat_open_input \
    avformat_find_stream_info avcodec_open2 avcodec_send_packet avcodec_receive_frame av_read_frame swr_convert_frame \
    avcodec_free_context avformat_close_input swr_free; do
    wraps+=("--wrap=$f")
done
"$lld" -r -o "$work/renpy.o" --wrap=chdir "${wraps[@]}" -u ps5_agc_gate2_run "${objects[@]}" --whole-archive "$out/librenpy-modules.a" \
    --no-whole-archive --start-group "${libraries[@]}" --end-group >"$log/isolate.log" 2>&1 || fail isolate
"$nm" --defined-only --extern-only "$work/renpy.o" | awk '{print $NF}' | grep -E '^retro_' | sort -u >"$work/api.txt"
[[ -s $work/api.txt ]] || { echo "no retro_* symbols in the Ren'Py core" >&2; exit 1; }
# Common symbols (OpenSSL's OPENSSL_ia32cap_P) stay global too, renamed as the API is: a relocatable link leaves them
# unallocated, and one made local would be absolute. The app's link gives them their space.
"$nm" --extern-only "$work/renpy.o" | awk '$2 == "C" { print $NF }' | sort -u >"$work/common.txt"
cat "$work/api.txt" "$work/common.txt" >"$work/keep.txt"
sed 's/.*/& renpy_&/' "$work/keep.txt" >"$work/rename.txt"
# FreeBSD's _setjmp and _longjmp (OpenSSL's) are the console's setjmp and longjmp, which also keep the signal mask
printf '_setjmp setjmp\n_longjmp longjmp\n' >>"$work/rename.txt"
# (without debug information: CPython's build keeps it, and it would be most of the archive)
"$objcopy" --strip-debug --keep-global-symbols="$work/keep.txt" "$work/renpy.o" "$work/renpy-local.o"
"$objcopy" --redefine-syms="$work/rename.txt" "$work/renpy-local.o" "$work/renpy-ps5.o"
"$nm" --undefined-only "$work/renpy-ps5.o" | awk '{print $NF}' | sort -u >"$work/undefined.txt"
rm -f "$out/librenpy.a"
"$ar" rcs "$out/librenpy.a" "$work/renpy-ps5.o"

# What is still undefined must come from the console (its libc and modules) or the app: anything else is reported.
: >"$work/console.txt"
for stub in "$sdk/target/lib"/*.so; do
    # WebKit's module and libkernel_sys's stub give a title nothing at run time (the latter's functions not also in
    # libkernel's: readlink crashed Ren'Py on the console)
    [[ ${stub##*/} == libScePosixForWebKit.so || ${stub##*/} == libkernel_sys.so ]] && continue
    "$nm" --defined-only -D "$stub" 2>/dev/null | awk 'NF >= 2 { print $NF }' >>"$work/console.txt"
done
# the AGC calls: the app links them through its own stubs (tooling/ps5-stubs), not ps5-opengl's
grep -ohE '\bsceAgc[A-Za-z0-9_]+[[:space:]]*\(' "$root"/tooling/ps5-stubs/*.c | tr -d ' (' >>"$work/console.txt"
# What the app's link adds (tooling/radv/radv-link.sh): the SDK's C++ runtime and unwinder, compiler-rt's builtins
# (__emutls_get_address), and the SDK's platform layer, whose ps5_<name> it links in for <name>. Not the SDK's
# libc.a, which the app does not link: the console's libc is reached through the stubs above.
"$nm" --defined-only "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a" \
    "$sdk/target/lib/libps5platform.a" 2>/dev/null | awk 'NF >= 2 && $2 ~ /[TDBRVW]/ { print $NF }' |
    sed 's/^ps5_//' >>"$work/console.txt"
echo __emutls_get_address >>"$work/console.txt"
LC_ALL=C sort -u -o "$work/console.txt" "$work/console.txt"
# The app provides dirfd, nl_langinfo, uselocale, strcasestr and ___mb_cur_max (src/platform/ps5_shims.c), and its
# link the C++ runtime's __dso_handle (as for EasyRPG Player's core).
LC_ALL=C comm -23 "$work/undefined.txt" "$work/console.txt" | grep -vxE 'dirfd|nl_langinfo|uselocale|strcasestr|___mb_cur_max|__dso_handle' \
    >"$work/unresolved.txt" || true

# ---- the engine: the host's (Ren'Py's Python, the standard library, the packages are the same on the console)
engine_key="engine-ps5-2 $(cat "$host/.engine") $(cat "$host/.compile" 2>/dev/null)"
if ! fresh engine "$engine_key"; then
    [[ -f $host/.compile ]] || { echo "Ren'Py's common scripts are not compiled: run tools/build-renpy.sh host" >&2; exit 1; }
    rm -rf "$out/assets"
    mkdir -p "$out/assets"
    cp -R "$host/assets/renpy8" "$out/assets/renpy8"
    stamp engine "$engine_key"
fi

# ---- the licences the app carries for the core (THIRD_PARTY_NOTICES.md lists the rest): PacBrew's own copies where
# it ships them, and the Apache License 2.0 that OpenSSL is under (the copy in requests' wheel, the same text)
rm -rf "$out/licenses"
mkdir -p "$out/licenses"
for pair in "$root/third_party/renpy/sphinx/source/license.rst:LICENSE-RenPy.rst" \
    "$out/work/python/Python-$python_version/LICENSE:LICENSE-Python.txt" \
    "$out/work/ffmpeg/ffmpeg-$ffmpeg_version/COPYING.LGPLv2.1:LICENSE-FFmpeg.txt" \
    "$out/work/assimp/assimp-$assimp_version/LICENSE:LICENSE-Assimp.txt" \
    "$out/ps5-opengl/LICENSE:LICENSE-ps5-opengl.txt" \
    "$out/ps5-opengl/THIRD_PARTY_NOTICES.md:NOTICES-ps5-opengl.md" \
    "$hb/share/licenses/SDL2/LICENSE.txt:LICENSE-SDL2.txt" \
    "$hb/share/doc/libjpeg-turbo/LICENSE.md:LICENSE-libjpeg-turbo.md" \
    "$hb/share/doc/xz/COPYING:LICENSE-xz.txt" \
    "$out/assets/renpy8/lib/site-packages/requests-2.33.0.dist-info/licenses/LICENSE:LICENSE-Apache-2.0.txt"; do
    [[ -f ${pair%%:*} ]] || { echo "missing licence: ${pair%%:*}" >&2; exit 1; }
    cp "${pair%%:*}" "$out/licenses/${pair##*:}"
done

echo "==> [renpy ps5] $(wc -l <"$work/api.txt" | tr -d ' ') libretro functions as renpy_retro_*," \
    "$(wc -l <"$work/undefined.txt" | tr -d ' ') symbols left for the app's link"
if [[ -s $work/unresolved.txt ]]; then
    echo "==> [renpy ps5] not provided by the console or the app ($(wc -l <"$work/unresolved.txt" | tr -d ' ')):" \
        "$(tr '\n' ' ' <"$work/unresolved.txt")" >&2
    exit 1
fi
echo "==> [renpy ps5] $out/librenpy.a"
