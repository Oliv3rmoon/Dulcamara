# Ren'Py core, milestone 2: the PS5 build

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** the core of milestone 1, cross-built for the PS5 as `build/renpy-ps5/librenpy.a` (the libretro API renamed
`renpy_retro_*`, every other symbol local) with its engine folder, linked into Dulcamara's `eboot.bin` and packaged
as `PPSA99010/renpy8/`, so that the test game can be run on a console.

**Spec:** `docs/superpowers/specs/2026-10-29-renpy-core-design.md` (section 1, milestone 2). **Milestone 1's plan:**
`docs/superpowers/plans/2026-10-29-renpy-core.md`, whose Global Constraints hold here too.

## What milestone 1 established that this plan builds on

- `tools/build-renpy.sh host` builds, in stages, CPython 3.12.8, FFmpeg 4.3.1 (LGPL), Assimp 5.4.3, Cython's C for
  Ren'Py's 61 modules (`build/renpy-host/renpy-src/tmp/gen3-static`), the engine folder and the checks. The generated
  C and the engine folder are the same for the PS5: the PS5 build reuses them, and its build Python is the host one.
- A checkout path with spaces builds through `build/renpy-<target>` linked to a folder in TMPDIR.
- The PS5 payload SDK's own `prospero-clang` links test programs (FreeBSD ELF payloads), which autoconf's checks
  need; it works with CI's `clang-18`/`llvm-config-18` and with Homebrew's LLVM here, from a path without spaces.
- CPython 3.12.8 configures and builds `libpython3.12.a` for the PS5 with a FreeBSD host
  (`tools/patches/python-3.12.8-cross-freebsd.diff`), PacBrew's OpenSSL, zlib, bzip2 and xz, and three cross answers
  (`ac_cv_file__dev_ptmx=no`, `ac_cv_file__dev_ptc=no`, `ac_cv_buggy_getaddrinfo=no`).
- What libpython needs that neither PacBrew, the console's libc stubs nor the app provide: 36 libc functions
  (`getpwnam_r`, `pipe2`, `utimensat`, `clock_nanosleep`...), and 7 that resolve only through
  `libScePosixForWebKit` (`getaddrinfo`, `isatty`...), which a native title does not load.

## Tasks

### Task M2.1: CPython for the PS5, in the build script

- `tools/build-renpy.sh ps5`: builds the host target first (its build Python, the generated C, the engine), copies
  the payload SDK under the build folder, links PacBrew's sysroot there, and builds `libpython3.12.a`,
  `libexpat.a` and `libHacl_Hash_SHA2.a` with the configure above.
- `src/core/renpy/ps5_libc.c` (PS5 only, inside the core's isolated object): stand-ins for the functions the console
  lacks, each doing what a sandboxed console can (an error with `errno`, or the nearest call that exists).
- Check: the list of what libpython leaves unresolved, computed as above with the core's own objects subtracted, is
  empty (the app's `ps5_shims.c` keeps providing `dirfd` and `nl_langinfo`).

### Task M2.2: FFmpeg, Assimp and Ren'Py's modules for the PS5

- FFmpeg 4.3.1 with milestone 1's options, `--enable-cross-compile --target-os=freebsd --arch=x86_64`, nasm's x86
  assembly when the build machine has nasm.
- Assimp with a PS5 CMake toolchain file like `tools/build-easyrpg.sh`'s.
- Ren'Py's 61 modules compiled from the host's generated C with the PS5 compiler, PacBrew's SDL2 2.30, SDL2_image,
  FreeType, HarfBuzz, fribidi, libpng, libjpeg and libwebp, and `sdl_renames.h`.

### Task M2.3: OpenGL and the isolated archive

- ps5-opengl SDK 1.0.2, pinned as Nightshade's `tools/deps.json` pins it (with its SHA-256); `gl_egl.c`'s PS5 path
  (default display, a pbuffer, a compatibility context).
- `ld -r` of the core, Ren'Py's modules, Python, FFmpeg, Assimp, PacBrew's libraries and ps5-opengl's; every symbol
  but the libretro API made local, the API renamed `renpy_retro_*`, as `tools/build-easyrpg.sh` does. Check: the
  undefined list holds only the C and C++ runtimes and the console's own modules.

### Task M2.4: into the app

- `cores.h`/`cores.c`: `CORE_RENPY` (Ren'Py 8), with Ren'Py's default buttons and `frame_time`.
- `host.c`: `RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY` answers the app's folder.
- `library.c`: a folder with `game/` holding `.rpy`, `.rpyc` or `.rpa` files is a Ren'Py game (`CORE_RENPY`); the rest
  of the library's rules are milestone 3's.
- Makefile and `tools/build.sh`: the archive linked, the system modules ps5-opengl needs imported, `renpy8/` copied
  into `dist/PPSA99010/`; CI builds it (the PS5 job's timeout and caches grown for the two builds).
- Check: `make` builds `eboot.bin` with the core in it, `tools/check-imports.py` passes, and the ZIP holds
  `renpy8/renpy/__init__.py`.

### Task M2.5: the console

- Andrew (or a tester) copies `tests/renpy-game` (made by `tests/test_renpy_core.py`'s `make_game`) to
  `/data/dulcamara/games/` and plays it; `/data/dulcamara/logs` comes back. Not something this session can do.

## Stop rule

From the spec: if CPython does not build for the PS5 SDK, stop and report. It builds (above); the remaining risk is
the console run (M2.5): ps5-opengl's EGL beside Dulcamara's Vulkan screen, and its compatibility context.
