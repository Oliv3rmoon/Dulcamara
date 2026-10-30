# Dulcamara - what tools/build-renpy.sh (Ren'Py 8) and tools/build-renpy7.sh (Ren'Py 7) share, sourced by each after
# it sets root: where downloads and builds go, and the helpers of their stages.
# SPDX-License-Identifier: GPL-3.0-or-later

cache="$root/.deps/renpy"
jobs=${BUILD_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
sha256() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
digest() { cat "$@" | sha256 | cut -c1-16; }

# build_folder <name>: build/<name>, printed as its real path. Autotools (libtool, CPython's and FFmpeg's configure)
# cannot build under a path with spaces in it, as a checkout's may have: then build/<name> is a link to a folder in
# TMPDIR whose path has none.
build_folder() {
    local wanted="$root/build/$1" real
    if [[ $wanted == *[[:space:]]* ]]; then
        real="${TMPDIR:-/tmp}"
        real="${real%/}/dulcamara-$1-$(printf '%s' "$wanted" | sha256 | cut -c1-12)"
        [[ $real == *[[:space:]]* ]] && { echo "no folder without spaces for the build (TMPDIR has them)" >&2; exit 2; }
        mkdir -p "$real" "$(dirname "$wanted")"
        if [[ ! -L $wanted ]]; then
            rm -rf "$wanted"
            ln -s "$real" "$wanted"
        fi
    fi
    mkdir -p "$wanted"
    (cd "$wanted" && pwd -P)
}

# fail <stage>: the end of its log ($log/<stage>.log), and stop
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
