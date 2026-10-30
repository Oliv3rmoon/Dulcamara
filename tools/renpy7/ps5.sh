# Dulcamara - the PS5 half of tools/build-renpy7.sh, which sources it with its variables and helpers (root, out,
# renpy8, prefix, cache, log, jobs, fetch, unpack, fresh, stamp, digest, fail, the versions). It cross-builds the core
# of build/renpy7-host for the PS5, as tools/renpy/ps5.sh does Ren'Py 8's, from that build's tools and libraries: the
# SDK copy, PacBrew, FFmpeg for the PS5 and ps5-opengl (build/renpy-ps5).
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   build/renpy7-ps5/librenpy7.a     the core: one relocatable object, every symbol local but the libretro API, which
#                                    is renamed renpy7_retro_*
#   build/renpy7-ps5/assets/renpy7   the engine, shipped beside eboot.bin

host=$(cd "$root/build/renpy7-host" && pwd -P)
hostpy="$host/python/bin/python2.7"
[[ -x $hostpy && -f $host/gen/inittab.c && -f $host/.compile ]] || {
    echo "Ren'Py 7's host build is incomplete: run tools/build-renpy7.sh host" >&2
    exit 1
}
[[ -f $renpy8/librenpy.a && -f $renpy8/ps5-opengl/sdk/lib/libPS5OpenGL.a ]] || {
    echo "Ren'Py 8's PS5 build is incomplete: run tools/build-renpy.sh" >&2
    exit 1
}

# ---- the tools: Ren'Py 8's build's (its LLVM, its copy of the SDK, its PacBrew)
if command -v llvm-config-18 >/dev/null; then
    llvm_bin=$(llvm-config-18 --bindir)
elif [[ -x /opt/homebrew/opt/llvm/bin/llvm-config ]]; then
    llvm_bin=/opt/homebrew/opt/llvm/bin
else
    llvm_bin=$(llvm-config --bindir)
fi
export PATH="$llvm_bin:$PATH"
lld=$(command -v ld.lld)
ar="$llvm_bin/llvm-ar"
ranlib="$llvm_bin/llvm-ranlib"
nm="$llvm_bin/llvm-nm"
objcopy="$llvm_bin/llvm-objcopy"
sdk="$renpy8/sdk"
hb="$renpy8/pacbrew/user/homebrew"
opengl="$renpy8/ps5-opengl"
export PKG_CONFIG_LIBDIR="$hb/lib/pkgconfig:$hb/libdata/pkgconfig" PKG_CONFIG_PATH="" PKG_CONFIG_SYSROOT_DIR="$renpy8/pacbrew"
cc="$sdk/bin/prospero-clang"
ps5_flags="-O2 -fPIC -march=znver2 -ffunction-sections -fdata-sections"
unset CC CXX CFLAGS CXXFLAGS LDFLAGS

# ---- CPython 2.7 for the PS5: a FreeBSD host (its headers are FreeBSD's) with the console's libc, the modules of
# tools/renpy7/Setup.local. Only the library is built: Python itself runs on the build machine (the host build's).
build_python_ps5() {
    echo "==> [renpy7 ps5] CPython $python_version"
    unpack "Python-$python_version.tgz" "$out/work/python"
    rm -rf "$out/python"
    local src="$out/work/python/Python-$python_version" p
    : >"$log/python.log"
    for p in "$root"/tools/patches/python-2.7.18-*.diff; do
        patch -d "$src" -p1 <"$p" >>"$log/python.log" 2>&1 || fail python
    done
    cp "$root/tools/renpy7/Setup.local" "$src/Modules/Setup.local"
    # cross answers: no pseudo-terminals; getaddrinfo is the console's (the core stands in for it, ps5_libc.c); the
    # rest is what an x86-64 FreeBSD answers
    printf '%s\n' ac_cv_file__dev_ptmx=no ac_cv_file__dev_ptc=no ac_cv_buggy_getaddrinfo=no \
        ac_cv_have_long_long_format=yes ac_cv_little_endian_double=yes ac_cv_x87_double_rounding=no \
        ac_cv_tanh_preserves_zero_sign=yes ac_cv_wchar_t_signed=yes ac_cv_rshift_extends_sign=yes \
        ac_cv_have_size_t_format=yes >"$src/config.site"
    # PacBrew's zlib and bzip2 headers, and no others: its expat.h would come before Python's own expat (whose
    # names pyexpat's build changes), as Setup.local's flags follow CFLAGS
    local headers="$out/work/python/pacbrew-include" h
    mkdir -p "$headers"
    for h in zlib.h zconf.h bzlib.h; do ln -sf "$hb/include/$h" "$headers/$h"; done
    (cd "$src" && CONFIG_SITE=config.site ./configure --host=x86_64-unknown-freebsd11 --build="$(./config.guess)" \
        --prefix="$out/python" --disable-shared --enable-unicode=ucs4 --without-ensurepip --disable-ipv6 \
        CC="$cc" CFLAGS="$ps5_flags -I$headers" LDFLAGS="-L$hb/lib" AR="$ar" RANLIB="$ranlib" \
        PYTHON_FOR_BUILD="$hostpy") >>"$log/python.log" 2>&1 || fail python
    make -C "$src" -j"$jobs" libpython2.7.a >>"$log/python.log" 2>&1 || fail python
    mkdir -p "$out/python/include/python2.7" "$out/python/lib"
    cp -R "$src/Include/." "$out/python/include/python2.7/"
    cp "$src/pyconfig.h" "$out/python/include/python2.7/"
    cp "$src/libpython2.7.a" "$out/python/lib/"
}
python_key="python-ps5-2 $python_version $(digest "$root/tools/renpy7/Setup.local" "$root"/tools/patches/python-2.7.18-*.diff)"
fresh python "$python_key" || { build_python_ps5; stamp python "$python_key"; }

# ---- Ren'Py's and pygame_sdl2's modules: the host build's generated C, compiled for the PS5 with the renamed SDL
# calls.
pc_cflags() { pkg-config --cflags sdl2 SDL2_image freetype2 harfbuzz fribidi libpng libjpeg libwebp zlib; }
build_modules_ps5() {
    echo "==> [renpy7 ps5] Ren'Py's and pygame_sdl2's modules"
    local objects="$out/modules"
    rm -rf "$objects"
    mkdir -p "$objects"
    local cflags=($ps5_flags -g0 -w -fno-strict-aliasing -include "$root/src/core/renpy/sdl_renames.h"
        $(module_includes "$host") -I"$prefix/include" -I"$out/python/include/python2.7" $(pc_cflags)
        -I"$hb/include")
    local pids=() objs=() f o
    : >"$log/modules.log"
    while read -r f; do
        o="$objects/$(basename "$(dirname "$f")")-$(basename "${f%.*}").o"
        objs+=("$o")
        "$cc" -std=gnu99 "${cflags[@]}" -c "$f" -o "$o" >>"$log/modules.log" 2>&1 &
        pids+=($!)
        if (( ${#pids[@]} >= jobs )); then wait "${pids[0]}" || fail modules; pids=("${pids[@]:1}"); fi
    done <"$host/sources.txt"
    for f in "${pids[@]}"; do wait "$f" || fail modules; done
    rm -f "$out/librenpy7-modules.a"
    "$ar" rcs "$out/librenpy7-modules.a" "${objs[@]}"
}
modules_key="modules-ps5-1 $python_key $(cat "$host/.generate") $(cat "$renpy8/.ffmpeg") $(digest "$host/sources.txt" "$root/src/core/renpy/sdl_renames.h")"
fresh modules "$modules_key" || { build_modules_ps5; stamp modules "$modules_key"; }

# ---- the core's own C, and everything in one object
echo "==> [renpy7 ps5] the core"
libretro="$root/third_party/easyrpg-player/builds/libretro/libretro-common/include"
core="$out/core"
rm -rf "$core"
mkdir -p "$core"
core_flags=($ps5_flags -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -DDR_ENGINE_FOLDER='"renpy7"'
    -DDR_RENPY_VERSION='"7.8.7"' -I"$libretro" -isystem "$out/python/include/python2.7" -I"$opengl/sdk/include"
    $(pc_cflags))
objects=()
: >"$log/core.log"
for f in core python2 hooks input log handoff frames audio gl gl_egl ps5_libc; do
    "$cc" "${core_flags[@]}" -c "$root/src/core/renpy/$f.c" -o "$core/$f.o" >>"$log/core.log" 2>&1 ||
        { cat "$log/core.log" >&2; exit 1; }
    objects+=("$core/$f.o")
done
"$cc" $ps5_flags -w -I"$out/python/include/python2.7" -c "$host/gen/inittab.c" -o "$core/inittab.o" \
    >>"$log/core.log" 2>&1 || fail core
objects+=("$core/inittab.o")

# Every library the core uses goes into the object, as in Ren'Py 8's core: only what it references is taken.
libraries=("$out/python/lib/libpython2.7.a" "$prefix/lib/libavformat.a" "$prefix/lib/libavcodec.a"
    "$prefix/lib/libswscale.a" "$prefix/lib/libswresample.a" "$prefix/lib/libavutil.a" "$opengl/sdk/lib/libPS5OpenGL.a")
for name in SDL2_image SDL2 freetype harfbuzz fribidi png16 jpeg webpdemux webp sharpyuv bz2 lzma iconv z; do
    [[ -f $hb/lib/lib$name.a ]] || { echo "PacBrew has no lib$name.a" >&2; exit 1; }
    libraries+=("$hb/lib/lib$name.a")
done
work="$out/isolate"
rm -rf "$work"
mkdir -p "$work"
"$lld" -r -o "$work/renpy7.o" -u ps5_agc_gate2_run "${objects[@]}" --whole-archive "$out/librenpy7-modules.a" \
    --no-whole-archive --start-group "${libraries[@]}" --end-group >"$log/isolate.log" 2>&1 || fail isolate
"$nm" --defined-only --extern-only "$work/renpy7.o" | awk '{print $NF}' | grep -E '^retro_' | sort -u >"$work/api.txt"
[[ -s $work/api.txt ]] || { echo "no retro_* symbols in the Ren'Py 7 core" >&2; exit 1; }
# Common symbols (OpenSSL's OPENSSL_ia32cap_P) stay global too, renamed as the API is: a relocatable link leaves them
# unallocated, and one made local would be absolute. The app's link gives them their space.
"$nm" --extern-only "$work/renpy7.o" | awk '$2 == "C" { print $NF }' | sort -u >"$work/common.txt"
cat "$work/api.txt" "$work/common.txt" >"$work/keep.txt"
sed 's/.*/& renpy7_&/' "$work/keep.txt" >"$work/rename.txt"
"$objcopy" --strip-debug --keep-global-symbols="$work/keep.txt" "$work/renpy7.o" "$work/renpy7-local.o"
"$objcopy" --redefine-syms="$work/rename.txt" "$work/renpy7-local.o" "$work/renpy7-ps5.o"
"$nm" --undefined-only "$work/renpy7-ps5.o" | awk '{print $NF}' | sort -u >"$work/undefined.txt"
rm -f "$out/librenpy7.a"
"$ar" rcs "$out/librenpy7.a" "$work/renpy7-ps5.o"

# What is still undefined must come from the console (its libc and modules) or the app: anything else is reported.
: >"$work/console.txt"
for stub in "$sdk/target/lib"/*.so; do
    [[ ${stub##*/} == libScePosixForWebKit.so ]] && continue
    "$nm" --defined-only -D "$stub" 2>/dev/null | awk 'NF >= 2 { print $NF }' >>"$work/console.txt"
done
# the AGC calls: the app links them through its own stubs (tooling/ps5-stubs), not ps5-opengl's
grep -ohE '\bsceAgc[A-Za-z0-9_]+[[:space:]]*\(' "$root"/tooling/ps5-stubs/*.c | tr -d ' (' >>"$work/console.txt"
"$nm" --defined-only "$sdk/target/lib/libc.a" "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" 2>/dev/null |
    awk 'NF { print $NF }' >>"$work/console.txt"
LC_ALL=C sort -u -o "$work/console.txt" "$work/console.txt"
LC_ALL=C comm -23 "$work/undefined.txt" "$work/console.txt" | grep -vxE 'dirfd|nl_langinfo|__dso_handle' \
    >"$work/unresolved.txt" || true

# ---- the engine: the host's
engine_key="engine-ps5-1 $(cat "$host/.engine") $(cat "$host/.compile")"
if ! fresh engine "$engine_key"; then
    rm -rf "$out/assets"
    mkdir -p "$out/assets"
    cp -R "$host/assets/renpy7" "$out/assets/renpy7"
    stamp engine "$engine_key"
fi

# ---- the licences the app carries for this core, beside Ren'Py 8's (FFmpeg's and ps5-opengl's are those)
rm -rf "$out/licenses"
mkdir -p "$out/licenses"
for pair in "$root/third_party/renpy7/sphinx/source/license.rst:LICENSE-RenPy7.rst" \
    "$out/work/python/Python-$python_version/LICENSE:LICENSE-Python2.txt" \
    "$root/third_party/pygame_sdl2/COPYING.ZLIB:LICENSE-pygame_sdl2.txt" \
    "$root/third_party/pygame_sdl2/COPYING.LGPL21:LICENSE-pygame_sdl2-LGPL.txt"; do
    [[ -f ${pair%%:*} ]] || { echo "missing licence: ${pair%%:*}" >&2; exit 1; }
    cp "${pair%%:*}" "$out/licenses/${pair##*:}"
done

echo "==> [renpy7 ps5] $(wc -l <"$work/api.txt" | tr -d ' ') libretro functions as renpy7_retro_*," \
    "$(wc -l <"$work/undefined.txt" | tr -d ' ') symbols left for the app's link"
if [[ -s $work/unresolved.txt ]]; then
    echo "==> [renpy7 ps5] not provided by the console or the app ($(wc -l <"$work/unresolved.txt" | tr -d ' ')):" \
        "$(tr '\n' ' ' <"$work/unresolved.txt")" >&2
    exit 1
fi
echo "==> [renpy7 ps5] $out/librenpy7.a"
