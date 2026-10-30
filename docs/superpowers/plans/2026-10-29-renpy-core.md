# Ren'Py core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ren'Py 8 runs as a second libretro core in Dulcamara, proved first on the build machine (milestone 1), then cross-built for the PS5 (2), integrated and released (3), and followed by a Ren'Py 7 core (4).

**Architecture:** The core is `renpy_retro_*`: CPython 3.12, Ren'Py 8.5.3's Cython modules (its pygame_sdl2 included), FFmpeg, Assimp and SDL2 in one static archive, built like EasyRPG's. Ren'Py runs on a thread of its own and hands control back at every frame it shows and every wait. Its SDL calls for OpenGL, time, waits, timers, sound and controllers are renamed at compile time (`sdl_renames.h`, forced into Ren'Py's C) to the core's own functions: Ren'Py draws into an OpenGL framebuffer the core owns (EGL on the PS5 and Linux, CGL on macOS) and the core reads it back for `video_refresh`.

**Tech Stack:** C11 (the core), Python 3.12 (Ren'Py, the boot script, the tests), Cython 3.1.4, bash (the build), libretro, SDL2 (dummy video and audio drivers, a virtual controller), OpenGL 2.0+ through EGL or CGL.

**Spec:** `docs/superpowers/specs/2026-10-29-renpy-core-design.md`

**Scope of this plan:** milestone 1 in full. Milestones 2-4 are listed at the end at task level; each gets its own plan file (`docs/superpowers/plans/2026-10-29-renpy-core-m2.md` and so on), written from what milestone 1 built, and shown to Andrew before it starts. They depend on facts milestone 1 establishes (the build script's stages, which Python modules Ren'Py really imports, the hook list), so writing their steps now would be guessing.

## Where this plan refines the spec

Found while reading Ren'Py 8.5.3's source and Ren'Py's own build (renpy-build at the same tag) and the PS5 libraries. Andrew should confirm these before work starts:

1. **pygame_sdl2 is no longer separate.** Since 8.x it lives in Ren'Py's tree as `renpy/pygame` and is built from Ren'Py's own `src/Setup`. There is no pygame_sdl2 pin.
2. **The versions are Ren'Py 8.5.3's own:** Ren'Py `8.5.3.26051504`, CPython 3.12.8, FFmpeg 4.3.1 with the two FFmpeg fixes Ren'Py applies, Cython 3.1.4, and its pure-Python packages at the versions of its `uv.lock`.
3. **Two pieces the spec's table missed:** Assimp 5.4.3 (`renpy.gl2.assimp` is imported unconditionally; glTF and STL importers only, as Ren'Py's build) and SDL2_image. Plus the pure-Python packages Ren'Py imports at start: six, ecdsa, requests and its dependencies, rsa.
4. **Drawing without SDL's offscreen driver.** PacBrew's SDL2 has no EGL (only OSMesa, software). Instead SDL runs its dummy video driver and Ren'Py's `SDL_GL_*` calls go to the core, which makes the context itself (EGL pbuffer on the PS5 through ps5-opengl, EGL surfaceless on Linux, CGL on macOS) and binds a framebuffer object of its own. Ren'Py's gl2 takes whatever framebuffer is bound when it starts as the screen's (`gl2draw.pyx`, `init_fbo` reads `GL_FRAMEBUFFER_BINDING`), so it draws there unchanged. Ren'Py loads every GL function through `SDL_GL_GetProcAddress` (`renpy/uguu/gl.pyx`), which the core answers.
5. **Sound without an SDL audio driver.** Ren'Py opens its mixer with `SDL_OpenAudio` (48 kHz float stereo, `renpysound_core.c`). The core takes that callback and calls it itself from the frontend's sound callback (`RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK`, which `host_feed_audio` already drives for EasyRPG), converting to 16-bit. This keeps sound at normal speed during fast-forward, which one batch per `retro_run` would not.
6. **The host proof runs on two machines.** On macOS (this computer) with CGL; in CI on Ubuntu with Mesa's surfaceless EGL (llvmpipe), which is the PS5's EGL path.
7. **Ren'Py is a submodule** (`third_party/renpy`, pinned at the tag's commit), as EasyRPG Player is, rather than a tarball.
8. **On the PS5, OpenGL is ps5-opengl 1.0.2** (the SDK Nightshade pins). It is OpenGL 4.6 Core, with compatibility contexts "best effort" (its `docs/limitations.md`). Ren'Py's desktop gl2 asks for GL 2.0 without a profile, so it needs that compatibility context. ps5-opengl is Mesa, like the app's RADV, so it is linked inside the core's isolated object to keep their symbols apart. This is milestone 2's second risk after CPython.

## Global Constraints

- Ren'Py `8.5.3.26051504`, commit `39895c1e017f0b36ffea2447d97eccd69d76ee1c`, as the submodule `third_party/renpy`.
- CPython 3.12.8, `Python-3.12.8.tar.xz`, SHA-256 `c909157bb25ec114e5869124cc2a9c4a4d4c1e957ca4ff553f1edc692101154e`.
- FFmpeg 4.3.1, `ffmpeg-4.3.1.tar.gz`, SHA-256 `45035f15d6f192772de2309c846e1d60472694f479679354a39c699719e53772`, LGPL options only: never `--enable-gpl` or `--enable-nonfree`, and the build checks `#define CONFIG_GPL 0`. No encoders, no network.
- Assimp 5.4.3, `assimp-5.4.3.tar.gz` (GitHub's tag archive), SHA-256 `66dfbaee288f2bc43172440a55d0235dfc7bf885dda6435c038e8000e79582cb`, glTF and STL importers only.
- fribidi 1.0.7 (`5ab5f21e9f2fc57b4b40f8ea8f14dba78a5cc46d9cf94bc5e00a58e6886a935d`) and SDL2_image 2.6.2 (`48355fb4d8d00bac639cd1c4f4a7661c4afef2c212af60b340e06b7059814777`), built only where the machine (or PacBrew) lacks them.
- Cython 3.1.4 and setuptools 80.10.2 run only on the build machine; the engine's pure-Python packages are those of Ren'Py's `uv.lock` (`tools/renpy/wheels.txt`). Every download is checked against its SHA-256.
- The core's libretro API is `renpy_retro_*` on the PS5; every other symbol in its archive is local. On the build machine the core keeps the plain `retro_*` names, as EasyRPG's host build does.
- The engine is `<core assets directory>/renpy8/` (`renpy/`, `renpy.py`, `dulcamara_boot.py`, `lib/python312.zip`, `lib/site-packages/`). Saves are `<save directory>/saves/renpy/<config.save_directory>`.
- Pictures are XRGB8888, at most 1920x1080. Sound is 48 kHz stereo, 16-bit.
- No game files are bundled or downloaded. The test game is written here, and its picture and sound are generated by the test.
- Commits: `git -c gpg.format=ssh -c user.signingkey=~/.ssh/id_ed25519.pub commit -S`, no AI attribution, no git config changes. Pushes: `feature/renpy-core` (and CI test branches) only. Leave `third_party/liblcf`'s uncommitted change alone.
- Style: match the repository. C files start `/*\n * Dulcamara - <what it is>\n * SPDX-License-Identifier: GPL-3.0-or-later\n */`, shell and Python files the same in `#` comments; comments are plain sentences about what the code does; C11, no warnings under `-Wall -Wextra`.
- `libretro.h` comes from `third_party/easyrpg-player/builds/libretro/libretro-common/include`, as for the rest of the app.

## Review Focus

The five conditions most likely to bite a player that the spec implies but does not name. Each has its test in the task that owns the code.

1. **A game folder named in Japanese, with spaces and brackets** (JoiPlay games often are): it starts, plays and saves. Test: `test_a_folder_named_in_japanese_with_spaces` (Task 9).
2. **A game that sets `config.sound_sample_rate = 44100`:** its sound plays at the right pitch, because the core tells Ren'Py the mixer runs at 48 kHz and Ren'Py resamples. Tests: `check_audio` (Task 5), `test_a_game_at_44100_hz` (Task 9).
3. **A game larger than the picture can be (2560x1440):** the picture is 1920x1080, the same shape. Tests: `frames_fit` checks (Task 4), `test_a_game_larger_than_the_picture` (Task 9).
4. **A script error:** Ren'Py's error is written to Dulcamara's log in full and shown on screen, and the game can still be closed. Test: `test_a_game_with_an_error` (Task 9).
5. **Closing a game from Dulcamara while Ren'Py waits for input, then starting another without restarting the app:** the core stops Ren'Py within seconds and the next game starts with a fresh interpreter. Tests: `check_stop` (Task 3), `test_a_second_game_in_the_same_process` (Task 9).

---

## File structure

| File | Responsibility |
|---|---|
| `third_party/renpy` | Ren'Py 8.5.3 source (submodule) |
| `tools/build-renpy.sh` | Builds the core and the engine folder: `host` in milestone 1, `ps5` in milestone 2 |
| `tools/renpy/Setup.local` | The CPython modules built into the core |
| `tools/renpy/wheels.txt` | Pinned wheels: the build tools (Cython, setuptools) and the engine's pure-Python packages |
| `tools/patches/ffmpeg-4.3.1-sse.diff`, `ffmpeg-4.3.1-ff_seek_frame_binary.diff` | The two FFmpeg fixes Ren'Py's build applies |
| `src/core/renpy/handoff.c/.h` | Control between `retro_run` and Ren'Py's thread; the clock |
| `src/core/renpy/frames.c/.h` | The pictures: three buffers between the threads, row flip, size fitting |
| `src/core/renpy/audio.c/.h` | Ren'Py's mixer, called for the frontend's sound, float to 16-bit |
| `src/core/renpy/gl.c/.h`, `gl_backend.h`, `gl_egl.c`, `gl_cgl.c` | The OpenGL context, the framebuffer Ren'Py draws into, the read-back |
| `src/core/renpy/sdl_renames.h` | Forced into Ren'Py's C: its SDL calls renamed to the core's |
| `src/core/renpy/hooks.c/.h` | What those renamed calls do |
| `src/core/renpy/input.c/.h` | The virtual SDL controller the RetroPad drives |
| `src/core/renpy/log.c/.h` | Log lines from any thread, handed to the frontend in `retro_run` |
| `src/core/renpy/python.c/.h` | Ren'Py's thread: CPython, the `_dulcamara` module, starting and stopping |
| `src/core/renpy/boot.py` | Shipped as `renpy8/dulcamara_boot.py`: the clock, the log, error logging, then Ren'Py |
| `src/core/renpy/core.c` | The libretro API |
| `tests/core/renpy_unit.c` | Checks without Ren'Py: handoff, frames, audio, GL read-back |
| `tests/core/renpy_headless.c` | A headless frontend that plays the test game |
| `tests/renpy-game/game/*.rpy` | The test game |
| `tests/test_renpy_core.py` | Builds the host core and runs both programs |
| `.github/workflows/build.yml` | A `renpy` job running the host check on Ubuntu |

---

## Milestone 1: host proof

### Task 1: Ren'Py's source and the pinned inputs

**Files:**
- Modify: `.gitmodules` (via `git submodule add`)
- Create: `third_party/renpy` (submodule), `tools/renpy/Setup.local`, `tools/renpy/wheels.txt`, `tools/patches/ffmpeg-4.3.1-sse.diff`, `tools/patches/ffmpeg-4.3.1-ff_seek_frame_binary.diff`

**Interfaces:**
- Produces: `third_party/renpy` at commit `39895c1e017f0b36ffea2447d97eccd69d76ee1c`; `wheels.txt` lines `<build|engine> <sha256> <url>`; `Setup.local` for CPython's `Modules/`.

- [ ] **Step 1: Add the submodule at the pinned commit**

```bash
cd checkouts/nightshade-player
git submodule add https://github.com/renpy/renpy.git third_party/renpy
git -C third_party/renpy fetch --depth 1 origin 39895c1e017f0b36ffea2447d97eccd69d76ee1c
git -C third_party/renpy checkout --detach 39895c1e017f0b36ffea2447d97eccd69d76ee1c
git -C third_party/renpy rev-parse HEAD
```

Expected: `39895c1e017f0b36ffea2447d97eccd69d76ee1c`.

- [ ] **Step 2: Write `tools/renpy/Setup.local`**

```
# Dulcamara - the CPython modules built into Ren'Py's core (tools/build-renpy.sh copies this to Python's
# Modules/Setup.local). Ren'Py's own list (renpy-build's Python-3.12.8-Setup.stdlib) without ctypes, which needs
# libffi (PacBrew has none and Ren'Py does not import it at start), and without the terminal and macOS modules.
# The rest of the standard library is Python, in the engine's lib/python312.zip.
# SPDX-License-Identifier: GPL-3.0-or-later

*static*
_asyncio -DPy_BUILD_CORE_BUILTIN _asynciomodule.c
_bisect -DPy_BUILD_CORE_BUILTIN _bisectmodule.c
_bz2 -DPy_BUILD_CORE_BUILTIN _bz2module.c -lbz2
_contextvars -DPy_BUILD_CORE_BUILTIN _contextvarsmodule.c
_csv -DPy_BUILD_CORE_BUILTIN _csv.c
_datetime -DPy_BUILD_CORE_BUILTIN _datetimemodule.c
_heapq -DPy_BUILD_CORE_BUILTIN _heapqmodule.c
_json -DPy_BUILD_CORE_BUILTIN -I$(srcdir)/Include/internal _json.c
_lsprof -DPy_BUILD_CORE_BUILTIN _lsprof.c rotatingtree.c
_opcode -DPy_BUILD_CORE_BUILTIN _opcode.c
_pickle -DPy_BUILD_CORE_BUILTIN _pickle.c
_queue -DPy_BUILD_CORE_BUILTIN _queuemodule.c
_random -DPy_BUILD_CORE_BUILTIN _randommodule.c
_statistics -DPy_BUILD_CORE_BUILTIN _statisticsmodule.c
_struct -DPy_BUILD_CORE_BUILTIN _struct.c
_typing -DPy_BUILD_CORE_BUILTIN _typingmodule.c
_zoneinfo -DPy_BUILD_CORE_BUILTIN _zoneinfo.c
array -DPy_BUILD_CORE_BUILTIN arraymodule.c
binascii -DPy_BUILD_CORE_BUILTIN binascii.c
cmath -DPy_BUILD_CORE_BUILTIN cmathmodule.c
math -DPy_BUILD_CORE_BUILTIN mathmodule.c
mmap -DPy_BUILD_CORE_BUILTIN mmapmodule.c
select -DPy_BUILD_CORE_BUILTIN selectmodule.c
unicodedata -DPy_BUILD_CORE_BUILTIN unicodedata.c
zlib -DPy_BUILD_CORE_BUILTIN zlibmodule.c -lz
_lzma -DPy_BUILD_CORE_BUILTIN _lzmamodule.c -llzma
pyexpat -DPy_BUILD_CORE_BUILTIN expat/xmlparse.c expat/xmlrole.c expat/xmltok.c pyexpat.c -I$(srcdir)/Modules/expat -DHAVE_EXPAT_CONFIG_H -DXML_POOR_ENTROPY -DUSE_PYEXPAT_CAPI
_elementtree -DPy_BUILD_CORE_BUILTIN -I$(srcdir)/Modules/expat -DHAVE_EXPAT_CONFIG_H -DUSE_PYEXPAT_CAPI _elementtree.c
_md5 md5module.c -DPy_BUILD_CORE_BUILTIN -I$(srcdir)/Modules/_hacl/include _hacl/Hacl_Hash_MD5.c -D_BSD_SOURCE -D_DEFAULT_SOURCE
_sha1 sha1module.c -DPy_BUILD_CORE_BUILTIN -I$(srcdir)/Modules/_hacl/include _hacl/Hacl_Hash_SHA1.c -D_BSD_SOURCE -D_DEFAULT_SOURCE
_sha2 sha2module.c -DPy_BUILD_CORE_BUILTIN -I$(srcdir)/Modules/_hacl/include _hacl/Hacl_Hash_SHA2.c -D_BSD_SOURCE -D_DEFAULT_SOURCE
_sha3 sha3module.c -DPy_BUILD_CORE_BUILTIN -I$(srcdir)/Modules/_hacl/include _hacl/Hacl_Hash_SHA3.c -D_BSD_SOURCE -D_DEFAULT_SOURCE
_blake2 -DPy_BUILD_CORE_BUILTIN _blake2/blake2module.c _blake2/blake2b_impl.c _blake2/blake2s_impl.c -D_BSD_SOURCE -D_DEFAULT_SOURCE
_codecs_cn -DPy_BUILD_CORE_BUILTIN cjkcodecs/_codecs_cn.c
_codecs_hk -DPy_BUILD_CORE_BUILTIN cjkcodecs/_codecs_hk.c
_codecs_iso2022 -DPy_BUILD_CORE_BUILTIN cjkcodecs/_codecs_iso2022.c
_codecs_jp -DPy_BUILD_CORE_BUILTIN cjkcodecs/_codecs_jp.c
_codecs_kr -DPy_BUILD_CORE_BUILTIN cjkcodecs/_codecs_kr.c
_codecs_tw -DPy_BUILD_CORE_BUILTIN cjkcodecs/_codecs_tw.c
_multibytecodec -DPy_BUILD_CORE_BUILTIN cjkcodecs/multibytecodec.c
_socket -DPy_BUILD_CORE_BUILTIN socketmodule.c
_ssl -DPy_BUILD_CORE_BUILTIN _ssl.c $(OPENSSL_INCLUDES) $(OPENSSL_LDFLAGS) $(OPENSSL_LIBS)
_hashlib -DPy_BUILD_CORE_BUILTIN _hashopenssl.c $(OPENSSL_INCLUDES) $(OPENSSL_LDFLAGS) -lcrypto
fcntl -DPy_BUILD_CORE_BUILTIN fcntlmodule.c
_posixsubprocess -DPy_BUILD_CORE_BUILTIN _posixsubprocess.c

*disabled*
_ctypes
_ctypes_test
_curses
_curses_panel
_dbm
_gdbm
_tkinter
_uuid
_sqlite3
_decimal
_multiprocessing
_posixshmem
_scproxy
_crypt
audioop
grp
nis
ossaudiodev
readline
resource
spwd
syslog
termios
xxlimited
xxlimited_35
xxsubtype
_testcapi
_testinternalcapi
_testbuffer
_testclinic
_testimportmultiple
_testmultiphase
_testsinglephase
_xxsubinterpreters
_xxinterpchannels
_xxtestfuzz
```

- [ ] **Step 3: Write `tools/renpy/wheels.txt`**

```
# Dulcamara - the wheels tools/build-renpy.sh unpacks: "build" ones run on the build machine (Cython turns Ren'Py's
# .pyx into C; Ren'Py's setup.py needs setuptools), "engine" ones go into the engine's lib/site-packages (the
# pure-Python packages Ren'Py imports, at the versions of Ren'Py 8.5.3's uv.lock). <kind> <sha256> <url>
# SPDX-License-Identifier: GPL-3.0-or-later
build d194d95e4fa029a3f6c7d46bdd16d973808c7ea4797586911fdb67cb98b1a2c6 https://files.pythonhosted.org/packages/7c/24/f7351052cf9db771fe4f32fca47fd66e6d9b53d8613b17faf7d130a9d553/cython-3.1.4-py3-none-any.whl
build 95b30ddfb717250edb492926c92b5221f7ef3fbcc2b07579bcd4a27da21d0173 https://files.pythonhosted.org/packages/94/b8/f1f62a5e3c0ad2ff1d189590bfa4c46b4f3b6e49cef6f26c6ee4e575394d/setuptools-80.10.2-py3-none-any.whl
engine 4721f391ed90541fddacab5acf947aa0d3dc7d27b2e1e8eda2be8970586c3274 https://files.pythonhosted.org/packages/b7/ce/149a00dd41f10bc29e5921b496af8b574d8413afcd5e30dfa0ed46c2cc5e/six-1.17.0-py2.py3-none-any.whl
engine 30638e27cf77b7e15c4c4cc1973720149e1033827cfd00661ca5c8cc0cdb24c3 https://files.pythonhosted.org/packages/cb/a3/460c57f094a4a165c84a1341c373b0a4f5ec6ac244b998d5021aade89b77/ecdsa-0.19.1-py2.py3-none-any.whl
engine 3324635456fa185245e24865e810cecec7b4caf933d7eb133dcde67d48cee69b https://files.pythonhosted.org/packages/56/5d/c814546c2333ceea4ba42262d8c4d55763003e767fa169adc693bd524478/requests-2.33.0-py3-none-any.whl
engine 9fb4c81ebbb1ce9531cce37674bbc6f1360472bc18ca9a553ede278ef7276897 https://files.pythonhosted.org/packages/7f/3e/5db95bcf282c52709639744ca2a8b149baccf648e39c8cc87553df9eae0c/urllib3-2.7.0-py3-none-any.whl
engine 771a87f49d9defaf64091e6e6fe9c18d4833f140bd19464795bc32d966ca37ea https://files.pythonhosted.org/packages/0e/61/66938bbb5fc52dbdf84594873d5b51fb1f7c7794e9c0f5bd885f30bc507b/idna-3.11-py3-none-any.whl
engine 9943707519e4add1115f44c2bc244f782c0249876bf51b6599fee1ffbedd685c https://files.pythonhosted.org/packages/e6/ad/3cc14f097111b4de0040c83a525973216457bbeeb63739ef1ed275c1c021/certifi-2026.1.4-py3-none-any.whl
engine 7a32c560861a02ff789ad905a2fe94e3f840803362c84fecf1851cb4cf3dc37f https://files.pythonhosted.org/packages/0a/4c/925909008ed5a988ccbb72dcc897407e5d6d3bd72410d69e051fc0c14647/charset_normalizer-3.4.4-py3-none-any.whl
engine 68635866661c6836b8d39430f97a996acbd61bfa49406748ea243539fe239762 https://files.pythonhosted.org/packages/64/8d/0133e4eb4beed9e425d9a98ed6e081a55d195481b7632472be1af08d2f6b/rsa-4.9.1-py3-none-any.whl
engine 1eb26d860996a18e9b6ed05e7aae0e9fc21619fcee6af91cca9bad4fbea224bf https://files.pythonhosted.org/packages/44/b5/a96872e5184f354da9c84ae119971a0a4c221fe9b27a4d94bd43f2596727/pyasn1-0.6.2-py3-none-any.whl
engine 929292d34f5872e70396626ef385ec22355a1fae8ad29e1a734c3e43f9fbc216 https://files.pythonhosted.org/packages/da/71/ae30dadffc90b9006d77af76b393cb9dfbfc9629f339fc1574a1c52e6806/future-1.0.0-py3-none-any.whl
```

- [ ] **Step 4: Copy Ren'Py's two FFmpeg fixes**

They are FFmpeg's own commits (LGPL), as Ren'Py's build applies them. From renpy-build at the same tag:

```bash
base=https://raw.githubusercontent.com/renpy/renpy-build/renpy-8.5.3.26051504/patches
curl -fsSL "$base/ffmpeg-4.3.1-sse.diff" -o tools/patches/ffmpeg-4.3.1-sse.diff
curl -fsSL "$base/ffmpeg-4.3.1-ff_seek_frame_binary.diff" -o tools/patches/ffmpeg-4.3.1-ff_seek_frame_binary.diff
head -5 tools/patches/ffmpeg-4.3.1-sse.diff
```

Expected: a `From ... Mon Sep 17 00:00:00 2001` FFmpeg commit header.

- [ ] **Step 5: Commit**

```bash
git add .gitmodules third_party/renpy tools/renpy tools/patches/ffmpeg-4.3.1-*.diff
git -c gpg.format=ssh -c user.signingkey=~/.ssh/id_ed25519.pub commit -S -m "Ren'Py core: Ren'Py 8.5.3's source and the pinned inputs of its build"
```

### Task 2: The test game and the failing core check

**Files:**
- Create: `tests/renpy-game/game/options.rpy`, `tests/renpy-game/game/screens.rpy`, `tests/renpy-game/game/script.rpy`, `tests/test_renpy_core.py`

**Interfaces:**
- Consumes: nothing yet (the test fails until Task 9).
- Produces: the game's log lines `DULCAMARA-TEST: started`, `advanced`, `choice=<left|right>`, `saved`, `loaded choice=<...>`; the harness command line `renpy-headless <assets> play|load|start|error <game> <data> [--then <game>]` and its report lines, which Task 9 must print exactly as the test reads them.

- [ ] **Step 1: Write `tests/renpy-game/game/options.rpy`**

```renpy
# Dulcamara - the test game of tests/test_renpy_core.py: its name and where Ren'Py keeps its saves. The test writes
# size.rpy beside this file (the screen size and sound rate of each run), its picture and its sound.
# SPDX-License-Identifier: GPL-3.0-or-later

define config.name = "Dulcamara test game"
define config.version = "1.0"
define config.save_directory = "dulcamara-test-game"
define config.has_autosave = False
define config.has_quicksave = False
define config.window_icon = None
```

- [ ] **Step 2: Write `tests/renpy-game/game/screens.rpy`**

```renpy
# Dulcamara - the test game's two screens: the dialogue and the menu.
# SPDX-License-Identifier: GPL-3.0-or-later

screen say(who, what):
    window id "window":
        background Solid("#000000c0")
        xfill True
        yalign 1.0
        ysize 180
        padding (40, 30)
        text what id "what" size 32 color "#ffffff"

# Left has the focus when the menu opens, so Down then Cross chooses Right.
screen choice(items):
    vbox:
        xalign 0.5
        yalign 0.5
        spacing 20
        for i in items:
            textbutton i.caption:
                action i.action
                default_focus (1 if i.caption == "Left" else 0)
                text_size 40
                text_idle_color "#a0a0a0"
                text_hover_color "#ffffff"
```

- [ ] **Step 3: Write `tests/renpy-game/game/script.rpy`**

```renpy
# Dulcamara - the test game: a picture, a sound, a line of dialogue, a menu and a save. It writes what it does to
# the log ("DULCAMARA-TEST: ..."), and the headless frontend acts on that. With DULCAMARA_TEST_LOAD=1 it loads its
# save instead of starting over.
# SPDX-License-Identifier: GPL-3.0-or-later

init python:
    import os

    def dulcamara_test(message):
        print("DULCAMARA-TEST: " + message)

    def dulcamara_after_load():
        dulcamara_test("loaded choice=" + str(store.choice))

    config.after_load_callbacks.append(dulcamara_after_load)

default choice = None

# No main menu: the game starts at once.
label main_menu:
    return

label start:
    $ dulcamara_test("started")
    if os.environ.get("DULCAMARA_TEST_LOAD") == "1" and renpy.can_load("1-1"):
        $ renpy.load("1-1")
    scene expression Solid("#203040")
    show dulcamara at truecenter
    play music "audio/tone.wav"
    "Hello from Dulcamara's test game."
    $ dulcamara_test("advanced")
    menu:
        "Left":
            $ choice = "left"
        "Right":
            $ choice = "right"
    $ dulcamara_test("choice=" + choice)
    $ renpy.save("1-1")
    $ dulcamara_test("saved")
    "Saved."
    $ renpy.quit()
```

- [ ] **Step 4: Write `tests/test_renpy_core.py`**

```python
#!/usr/bin/env python3
# Dulcamara - Ren'Py 8's core, built for this computer and run headless on the test game of tests/renpy-game.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the libretro core the PS5 app links (tools/build-renpy.sh host: the same Ren'Py, CPython, FFmpeg and
Assimp, and the same renamed SDL calls) with the headless frontend of tests/core/renpy_headless.c, and plays
tests/renpy-game: it must load, give a picture, change it when Cross is pressed, play its sound, take a menu choice
and save, and a second run must load that save. Also runs the core's own checks (tests/core/renpy_unit.c): the
handoff and its clock, the pictures, the sound and the OpenGL read-back."""

import math
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import wave
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/renpy-host"
HEADLESS = BUILD / "core-check/renpy-headless"
UNIT = BUILD / "core-check/renpy-unit"
ASSETS = BUILD / "assets"
GAME = ROOT / "tests/renpy-game"


def write_png(path, width, height):
    """The test game's picture: a gradient, so that no two rows are alike."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows += bytes(((x + y) & 255, x & 255, y & 255))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows)))
                     + chunk(b"IEND", b""))


def write_tone(path, seconds=1.0, rate=48000, hz=440):
    """The test game's sound: a second of a 440 Hz tone, which the game loops."""
    samples = b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * hz * i / rate)))
                       for i in range(int(seconds * rate)))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(samples)


def make_game(directory, width=1280, height=720, sound_rate=48000, broken=False):
    """A copy of the test game in `directory`, with its picture, its sound and this run's screen size."""
    shutil.copytree(GAME, directory)
    game = directory / "game"
    (game / "images").mkdir(exist_ok=True)
    (game / "audio").mkdir(exist_ok=True)
    write_png(game / "images/dulcamara.png", 256, 256)
    write_tone(game / "audio/tone.wav")
    (game / "size.rpy").write_text(f"define config.screen_width = {width}\n"
                                   f"define config.screen_height = {height}\n"
                                   f"define config.sound_sample_rate = {sound_rate}\n", encoding="utf-8")
    if broken:
        (game / "broken.rpy").write_text("label broken:\n    $ this is not python\n", encoding="utf-8")
    return directory


class RenpyCoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which(os.environ.get("CC", "cc")) or not shutil.which("cmake"):
            raise unittest.SkipTest("needs a C compiler and cmake")
        build = subprocess.run(["bash", str(ROOT / "tools/build-renpy.sh"), "host"], capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError("tools/build-renpy.sh host failed:\n" + build.stdout[-4000:] + build.stderr[-4000:])
        print(build.stdout.strip().splitlines()[-1])

    def run_core(self, mode, game, data, *more, env=None):
        result = subprocess.run([str(HEADLESS), str(ASSETS), mode, str(game), str(data), *map(str, more)],
                                capture_output=True, text=True, errors="replace", timeout=600, check=False,
                                env={**os.environ, **(env or {})})
        print(f"\n{mode} {game.name}:")
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout[-6000:] + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)
        return result.stdout

    def play_and_check(self, game, data):
        output = self.run_core("play", game, data)
        for line in ("XRGB8888", "input: the picture changed after Cross", "sound: playing",
                     "DULCAMARA-TEST: choice=right", "DULCAMARA-TEST: saved", "ended by the game"):
            self.assertIn(line, output)
        return output

    def test_the_core_checks(self):
        result = subprocess.run([str(UNIT)], capture_output=True, text=True, timeout=120, check=False)
        print("\ncore checks:")
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)

    def test_play_save_and_load(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game, data = make_game(work / "Test Game"), work / "data"
            output = self.play_and_check(game, data)
            self.assertIn("picture: 1280x720", output)
            self.assertTrue(list((data / "saves/renpy/dulcamara-test-game").glob("1-1-*.save")),
                            "the save is in <data>/saves/renpy/<config.save_directory>")
            output = self.run_core("load", game, data, env={"DULCAMARA_TEST_LOAD": "1"})
            self.assertIn("DULCAMARA-TEST: loaded choice=right", output)

    def test_a_folder_named_in_japanese_with_spaces(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            self.play_and_check(make_game(work / "テスト ゲーム (体験版)"), work / "データ")

    def test_a_second_game_in_the_same_process(self):
        # The app closes one game and starts the next without restarting: Python must start over cleanly.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            first, second = make_game(work / "First"), make_game(work / "Second", 1920, 1080)
            output = self.run_core("play", first, work / "data", "--then", second)
            self.assertIn("second game: started, picture 1920x1080, stopped", output)

    def test_a_game_larger_than_the_picture(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Large", 2560, 1440), work / "data")
            self.assertIn("picture: 1920x1080", output)

    def test_a_game_at_44100_hz(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            self.play_and_check(make_game(work / "Slow sound", sound_rate=44100), work / "data")

    def test_a_game_with_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("error", make_game(work / "Broken", broken=True), work / "data")
            self.assertIn("Ren'Py reported an error", output)
            self.assertIn("broken.rpy", output)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 5: Run it to see it fail**

Run: `python3 -m unittest tests.test_renpy_core -v`
Expected: ERROR in `setUpClass`: `tools/build-renpy.sh host failed` (`No such file or directory`).

- [ ] **Step 6: Commit**

```bash
git add tests/renpy-game tests/test_renpy_core.py
git -c gpg.format=ssh -c user.signingkey=~/.ssh/id_ed25519.pub commit -S -m "Ren'Py core: the test game and its headless check"
```

### Task 3: The handoff and its clock

**Files:**
- Create: `src/core/renpy/handoff.h`, `src/core/renpy/handoff.c`, `tests/core/renpy_unit.c`

**Interfaces:**
- Produces (exact):

```c
void handoff_reset(void);
bool handoff_frame(int64_t frame_usec, int64_t wait_usec);
void handoff_stop(void);
void handoff_engine_begin(void);
bool handoff_yield(void);
void handoff_engine_end(void);
bool handoff_on_engine_thread(void);
bool handoff_stopping(void);
bool handoff_engine_ended(void);
int64_t handoff_now_usec(void);
```

- [ ] **Step 1: Write `src/core/renpy/handoff.h`**

```c
/*
 * Dulcamara - Ren'Py's core: control handed between the frontend's retro_run and Ren'Py's own thread, and the clock
 * Ren'Py keeps time by. Ren'Py runs until it shows a frame or waits, then gives control back. The clock moves only
 * when the frontend runs a frame, by that frame's time, so fast-forward runs Ren'Py's transitions faster.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_HANDOFF_H
#define DR_HANDOFF_H

#include <stdbool.h>
#include <stdint.h>

/* Before Ren'Py's thread starts: no frames, the clock at zero, not stopping. */
void handoff_reset(void);

/* The frontend: one frame. The clock moves on by frame_usec and Ren'Py runs until it gives control back, for at
 * most wait_usec of real time. True when it gave control back (or has ended) in that time; false when it is still
 * busy (starting, loading), and then it carries on by itself and gives control back at a later frame. */
bool handoff_frame(int64_t frame_usec, int64_t wait_usec);
/* The frontend: from now on Ren'Py never waits again (its game is being closed). */
void handoff_stop(void);

/* Ren'Py's thread, first: marks it as the thread that hands control back. It runs at once, without a frame. */
void handoff_engine_begin(void);
/* Ren'Py's thread: gives control back and waits for the next frame. False once the core is stopping, and then
 * at once on every later call. */
bool handoff_yield(void);
/* Ren'Py's thread, last. */
void handoff_engine_end(void);

bool handoff_on_engine_thread(void);
bool handoff_stopping(void);
bool handoff_engine_ended(void);
int64_t handoff_now_usec(void); /* the clock: microseconds of the game's time since it started */

#endif
```

- [ ] **Step 2: Write the handoff checks in `tests/core/renpy_unit.c`**

```c
/*
 * Dulcamara - the checks of Ren'Py's core that need neither Ren'Py nor Python: the handoff between the frontend and
 * Ren'Py's thread and its clock (handoff.c), the pictures (frames.c), the sound (audio.c) and, when this computer
 * has OpenGL without a screen, the read-back (gl.c). tests/test_renpy_core.py runs it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../../src/core/renpy/handoff.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                           \
    do                                                                                                             \
    {                                                                                                              \
        ++checks;                                                                                                  \
        if (!(cond))                                                                                               \
        {                                                                                                          \
            ++failures;                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static int64_t real_usec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* ---------------------------------------------------------------- the handoff */

static int64_t clock_seen[8];
static int resumed;

static void *five_frames(void *unused)
{
    (void)unused;
    handoff_engine_begin();
    for (int i = 0; i < 5 && handoff_yield(); ++i)
        clock_seen[resumed++] = handoff_now_usec();
    handoff_engine_end();
    return NULL;
}

/* Each frame lets Ren'Py run once, and Ren'Py sees the clock move by exactly one frame each time. */
static void check_lockstep(void)
{
    handoff_reset();
    resumed = 0;
    pthread_t thread;
    pthread_create(&thread, NULL, five_frames, NULL);
    for (int i = 0; i < 6; ++i)
        CHECK(handoff_frame(16667, 2000000), "frame %d: Ren'Py's thread gave control back", i);
    pthread_join(thread, NULL);
    CHECK(resumed == 5, "Ren'Py's thread ran five frames, not %d", resumed);
    CHECK(clock_seen[0] >= 16667, "the clock had moved when Ren'Py first ran (%lld)", (long long)clock_seen[0]);
    for (int i = 1; i < resumed; ++i)
        CHECK(clock_seen[i] - clock_seen[i - 1] == 16667, "one frame between runs %d and %d (%lld us)", i - 1, i,
              (long long)(clock_seen[i] - clock_seen[i - 1]));
    CHECK(!handoff_on_engine_thread(), "the frontend's thread is not Ren'Py's");
}

static void *busy_then_yield(void *unused)
{
    (void)unused;
    handoff_engine_begin();
    usleep(300000); /* Ren'Py starting: a long time without giving control back */
    while (handoff_yield())
    {
    }
    handoff_engine_end();
    return NULL;
}

/* A frame does not wait for Ren'Py longer than it may: the frontend stays responsive while Ren'Py starts. */
static void check_a_busy_ren_py(void)
{
    handoff_reset();
    pthread_t thread;
    pthread_create(&thread, NULL, busy_then_yield, NULL);
    const int64_t start = real_usec();
    const bool back = handoff_frame(16667, 20000);
    const int64_t took = real_usec() - start;
    CHECK(!back, "a frame while Ren'Py is busy does not get control back");
    CHECK(took < 150000, "a frame while Ren'Py is busy returns in its time (%lld us)", (long long)took);
    CHECK(handoff_now_usec() == 16667, "the clock moved by one frame (%lld)", (long long)handoff_now_usec());
    bool later = false;
    for (int i = 0; i < 100 && !later; ++i)
        later = handoff_frame(16667, 20000);
    CHECK(later, "once Ren'Py gives control back, the next frame gets it");
    handoff_stop();
    pthread_join(thread, NULL);
}

static bool yielded_after_stop = true;

static void *wait_until_stopped(void *unused)
{
    (void)unused;
    handoff_engine_begin();
    while (handoff_yield())
    {
    }
    yielded_after_stop = handoff_yield();
    handoff_engine_end();
    return NULL;
}

/* Closing the game while Ren'Py waits: the wait ends, every later one returns at once, and the end is seen. */
static void check_stop(void)
{
    handoff_reset();
    pthread_t thread;
    pthread_create(&thread, NULL, wait_until_stopped, NULL);
    handoff_frame(16667, 200000);
    handoff_stop();
    CHECK(handoff_stopping(), "stopping");
    pthread_join(thread, NULL);
    CHECK(!yielded_after_stop, "a wait after the stop returns false at once");
    CHECK(handoff_engine_ended(), "Ren'Py's thread ended");
    const int64_t start = real_usec();
    CHECK(handoff_frame(16667, 2000000), "a frame after the end gets control back");
    CHECK(real_usec() - start < 100000, "and does not wait");
}

int main(void)
{
    check_lockstep();
    check_a_busy_ren_py();
    check_stop();
    printf("%d checks, %d failed\n", checks, failures);
    printf(failures ? "FAIL\n" : "PASS\n");
    return failures ? 1 : 0;
}
```

- [ ] **Step 3: Run it to see it fail**

Run: `mkdir -p build/renpy-unit-try && cc -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -pthread tests/core/renpy_unit.c -o build/renpy-unit-try/unit`
Expected: link errors, `undefined symbol: handoff_reset` (and the others).

- [ ] **Step 4: Write `src/core/renpy/handoff.c`**

```c
/*
 * Dulcamara - Ren'Py's core: the handoff (handoff.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two counters keep the threads apart: `frames` (the frontend's frames) and `yields` (the times Ren'Py gave control
 * back). A frame waits for `yields` to move; Ren'Py, giving control back, waits for `frames` to move. Ren'Py that
 * was busy through several frames carries on at once when it gives control back, at the clock's latest time.
 */
#include "handoff.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t to_engine = PTHREAD_COND_INITIALIZER;
static pthread_cond_t to_frontend = PTHREAD_COND_INITIALIZER;
static pthread_t engine;
static bool engine_known, stopping, ended;
static int64_t now_usec;
static uint64_t frames, engine_frame, yields;

void handoff_reset(void)
{
    pthread_mutex_lock(&lock);
    engine_known = stopping = ended = false;
    now_usec = 0;
    frames = engine_frame = yields = 0;
    pthread_mutex_unlock(&lock);
}

/* wait_usec from now, on the clock pthread_cond_timedwait uses */
static struct timespec deadline(int64_t wait_usec)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    const int64_t ns = (int64_t)t.tv_nsec + (wait_usec % 1000000) * 1000;
    t.tv_sec += (time_t)(wait_usec / 1000000 + ns / 1000000000);
    t.tv_nsec = (long)(ns % 1000000000);
    return t;
}

bool handoff_frame(int64_t frame_usec, int64_t wait_usec)
{
    pthread_mutex_lock(&lock);
    now_usec += frame_usec;
    ++frames;
    const uint64_t before = yields;
    pthread_cond_broadcast(&to_engine);
    const struct timespec until = deadline(wait_usec);
    while (yields == before && !ended)
        if (pthread_cond_timedwait(&to_frontend, &lock, &until) == ETIMEDOUT)
            break;
    const bool back = yields != before || ended;
    pthread_mutex_unlock(&lock);
    return back;
}

void handoff_stop(void)
{
    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_broadcast(&to_engine);
    pthread_cond_broadcast(&to_frontend);
    pthread_mutex_unlock(&lock);
}

void handoff_engine_begin(void)
{
    pthread_mutex_lock(&lock);
    engine = pthread_self();
    engine_known = true;
    engine_frame = frames;
    pthread_mutex_unlock(&lock);
}

bool handoff_yield(void)
{
    pthread_mutex_lock(&lock);
    ++yields;
    pthread_cond_broadcast(&to_frontend);
    while (!stopping && frames == engine_frame)
        pthread_cond_wait(&to_engine, &lock);
    engine_frame = frames;
    const bool go_on = !stopping;
    pthread_mutex_unlock(&lock);
    return go_on;
}

void handoff_engine_end(void)
{
    pthread_mutex_lock(&lock);
    ended = true;
    pthread_cond_broadcast(&to_frontend);
    pthread_mutex_unlock(&lock);
}

bool handoff_on_engine_thread(void)
{
    pthread_mutex_lock(&lock);
    const bool is = engine_known && !ended && pthread_equal(engine, pthread_self());
    pthread_mutex_unlock(&lock);
    return is;
}

bool handoff_stopping(void)
{
    pthread_mutex_lock(&lock);
    const bool is = stopping;
    pthread_mutex_unlock(&lock);
    return is;
}

bool handoff_engine_ended(void)
{
    pthread_mutex_lock(&lock);
    const bool is = ended;
    pthread_mutex_unlock(&lock);
    return is;
}

int64_t handoff_now_usec(void)
{
    pthread_mutex_lock(&lock);
    const int64_t now = now_usec;
    pthread_mutex_unlock(&lock);
    return now;
}
```

- [ ] **Step 5: Run it to see it pass, under the sanitizers**

Run: `cc -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -g -fsanitize=address,undefined -pthread tests/core/renpy_unit.c src/core/renpy/handoff.c -o build/renpy-unit-try/unit && build/renpy-unit-try/unit`
Expected: `N checks, 0 failed` then `PASS`. Also run it once with `-fsanitize=thread` in place of `address,undefined`: no data race reported.

- [ ] **Step 6: Commit**

```bash
git add src/core/renpy/handoff.h src/core/renpy/handoff.c tests/core/renpy_unit.c
git -c gpg.format=ssh -c user.signingkey=~/.ssh/id_ed25519.pub commit -S -m "Ren'Py core: the handoff between retro_run and Ren'Py's thread, and its clock"
```

### Task 4: The pictures

**Files:**
- Create: `src/core/renpy/frames.h`, `src/core/renpy/frames.c`
- Modify: `tests/core/renpy_unit.c` (add `check_frames`)

**Interfaces:**
- Produces (exact):

```c
void frames_reset(void);
uint32_t *frames_back(int w, int h);
void frames_publish(void);
const uint32_t *frames_take(int *w, int *h, bool *fresh);
void frames_copy_flipped(uint32_t *dst, const uint32_t *src, int w, int h);
void frames_fit(int w, int h, int max_w, int max_h, int *out_w, int *out_h);
```

- [ ] **Step 1: Write `src/core/renpy/frames.h`**

```c
/*
 * Dulcamara - Ren'Py's core: the pictures Ren'Py draws, on their way to the frontend. Three buffers, so that Ren'Py
 * can draw the next picture while the frontend still shows the last one: Ren'Py writes the back one and publishes
 * it, the frontend takes the newest and keeps it until it takes another.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_FRAMES_H
#define DR_FRAMES_H

#include <stdbool.h>
#include <stdint.h>

void frames_reset(void); /* no pictures, the buffers freed */

/* Ren'Py's thread: the buffer for the next picture, w x h XRGB8888, rows top to bottom. NULL without memory. */
uint32_t *frames_back(int w, int h);
/* Ren'Py's thread: what was written into frames_back is the newest picture. */
void frames_publish(void);
/* The frontend: the newest picture, valid until the next call; fresh when it is new since the last call. NULL
 * before the first. */
const uint32_t *frames_take(int *w, int *h, bool *fresh);

/* OpenGL's rows run bottom to top: copies src's rows into dst, top to bottom. */
void frames_copy_flipped(uint32_t *dst, const uint32_t *src, int w, int h);
/* w x h made to fit max_w x max_h, keeping its shape; sizes that fit already are kept. */
void frames_fit(int w, int h, int max_w, int max_h, int *out_w, int *out_h);

#endif
```

- [ ] **Step 2: Add the failing checks to `tests/core/renpy_unit.c`**

Add `#include "../../src/core/renpy/frames.h"` beside the other include, this function before `main`, and the call `check_frames();` in `main` after `check_stop();`:

```c
/* ---------------------------------------------------------------- the pictures */

static void check_frames(void)
{
    frames_reset();
    int w = 0, h = 0;
    bool fresh = true;
    CHECK(frames_take(&w, &h, &fresh) == NULL && !fresh, "no picture before the first");

    uint32_t *back = frames_back(4, 2);
    for (int i = 0; i < 8; ++i)
        back[i] = (uint32_t)i;
    frames_publish();
    const uint32_t *shown = frames_take(&w, &h, &fresh);
    CHECK(shown && w == 4 && h == 2 && fresh && shown[7] == 7, "the first picture, 4x2 and fresh");
    shown = frames_take(&w, &h, &fresh);
    CHECK(shown && !fresh, "the same picture again is not fresh");

    /* the frontend's picture stays as it was while Ren'Py draws the next two */
    for (int n = 1; n <= 2; ++n)
    {
        back = frames_back(4, 2);
        for (int i = 0; i < 8; ++i)
            back[i] = (uint32_t)(n * 100 + i);
        frames_publish();
    }
    CHECK(shown[0] == 0, "the frontend's picture is untouched while Ren'Py draws (%u)", shown[0]);
    shown = frames_take(&w, &h, &fresh);
    CHECK(shown && fresh && shown[0] == 200, "the newest picture comes next");

    const uint32_t gl_rows[6] = {1, 2, 3, 4, 5, 6}; /* bottom row first */
    uint32_t rows[6];
    frames_copy_flipped(rows, gl_rows, 3, 2);
    CHECK(rows[0] == 4 && rows[2] == 6 && rows[3] == 1, "rows turned over");

    int fw, fh;
    frames_fit(1280, 720, 1920, 1080, &fw, &fh);
    CHECK(fw == 1280 && fh == 720, "1280x720 fits as it is");
    frames_fit(1920, 1080, 1920, 1080, &fw, &fh);
    CHECK(fw == 1920 && fh == 1080, "1920x1080 fits as it is");
    frames_fit(2560, 1440, 1920, 1080, &fw, &fh);
    CHECK(fw == 1920 && fh == 1080, "2560x1440 becomes 1920x1080 (%dx%d)", fw, fh);
    frames_fit(1080, 1920, 1920, 1080, &fw, &fh);
    CHECK(fw == 608 && fh == 1080, "a portrait 1080x1920 becomes 608x1080 (%dx%d)", fw, fh);
    frames_fit(3000, 1000, 1920, 1080, &fw, &fh);
    CHECK(fw == 1920 && fh == 640, "3000x1000 becomes 1920x640 (%dx%d)", fw, fh);
    frames_reset();
}
```

- [ ] **Step 3: Run it to see it fail**

Run: `cc -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -g -fsanitize=address,undefined -pthread tests/core/renpy_unit.c src/core/renpy/handoff.c -o build/renpy-unit-try/unit`
Expected: `undefined symbol: frames_reset` and the others.

- [ ] **Step 4: Write `src/core/renpy/frames.c`**

```c
/*
 * Dulcamara - Ren'Py's core: the pictures (frames.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "frames.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    uint32_t *pixels;
    size_t capacity; /* in pixels */
    int w, h;
} Slot;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Slot slots[3];
static int back = 0, ready = 1, front = 2;
static bool ready_is_new, any;

void frames_reset(void)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 3; ++i)
        free(slots[i].pixels);
    memset(slots, 0, sizeof(slots));
    back = 0, ready = 1, front = 2;
    ready_is_new = any = false;
    pthread_mutex_unlock(&lock);
}

uint32_t *frames_back(int w, int h)
{
    if (w <= 0 || h <= 0)
        return NULL;
    pthread_mutex_lock(&lock);
    Slot *s = &slots[back];
    const size_t need = (size_t)w * (size_t)h;
    if (need > s->capacity)
    {
        uint32_t *grown = realloc(s->pixels, need * sizeof(uint32_t));
        if (!grown)
        {
            pthread_mutex_unlock(&lock);
            return NULL;
        }
        s->pixels = grown;
        s->capacity = need;
    }
    s->w = w;
    s->h = h;
    uint32_t *pixels = s->pixels;
    pthread_mutex_unlock(&lock);
    return pixels;
}

void frames_publish(void)
{
    pthread_mutex_lock(&lock);
    const int was_ready = ready;
    ready = back;
    back = was_ready;
    ready_is_new = any = true;
    pthread_mutex_unlock(&lock);
}

const uint32_t *frames_take(int *w, int *h, bool *fresh)
{
    pthread_mutex_lock(&lock);
    const bool is_new = ready_is_new;
    if (is_new)
    {
        const int was_front = front;
        front = ready;
        ready = was_front;
        ready_is_new = false;
    }
    const Slot *s = &slots[front];
    const uint32_t *pixels = any && s->pixels && s->w ? s->pixels : NULL;
    *w = pixels ? s->w : 0;
    *h = pixels ? s->h : 0;
    if (fresh)
        *fresh = is_new && pixels;
    pthread_mutex_unlock(&lock);
    return pixels;
}

void frames_copy_flipped(uint32_t *dst, const uint32_t *src, int w, int h)
{
    for (int y = 0; y < h; ++y)
        memcpy(dst + (size_t)y * (size_t)w, src + (size_t)(h - 1 - y) * (size_t)w, (size_t)w * sizeof(uint32_t));
}

void frames_fit(int w, int h, int max_w, int max_h, int *out_w, int *out_h)
{
    if (w <= max_w && h <= max_h)
    {
        *out_w = w;
        *out_h = h;
    }
    else if ((long long)w * max_h >= (long long)h * max_w)
    {
        *out_w = max_w;
        *out_h = (int)(((long long)h * max_w + w / 2) / w);
    }
    else
    {
        *out_h = max_h;
        *out_w = (int)(((long long)w * max_h + h / 2) / h);
    }
}
```

- [ ] **Step 5: Run it to see it pass**

Run: `cc -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -g -fsanitize=address,undefined -pthread tests/core/renpy_unit.c src/core/renpy/handoff.c src/core/renpy/frames.c -o build/renpy-unit-try/unit && build/renpy-unit-try/unit`
Expected: `PASS`.

- [ ] **Step 6: Commit**

```bash
git add src/core/renpy/frames.h src/core/renpy/frames.c tests/core/renpy_unit.c
git -c gpg.format=ssh -c user.signingkey=~/.ssh/id_ed25519.pub commit -S -m "Ren'Py core: three picture buffers between Ren'Py's thread and the frontend"
```

### Task 5: The sound

**Files:**
- Create: `src/core/renpy/audio.h`, `src/core/renpy/audio.c`
- Modify: `tests/core/renpy_unit.c` (add `check_audio`)

**Interfaces:**
- Produces (exact):

```c
#define DR_AUDIO_RATE 48000
typedef void (*dr_mix_fn)(void *userdata, uint8_t *stream, int len);
int audio_open(int *freq, uint8_t *channels, dr_mix_fn fn, void *userdata);
void audio_pause(bool paused);
void audio_lock(void);
void audio_unlock(void);
void audio_close(void);
void audio_render(int16_t *out, size_t frames);
void audio_float_to_s16(const float *in, int16_t *out, size_t samples);
```

- [ ] **Step 1: Write `src/core/renpy/audio.h`**

```c
/*
 * Dulcamara - Ren'Py's core: Ren'Py's mixer, played for the frontend. Ren'Py opens its sound with SDL_OpenAudio and
 * a callback that mixes 32-bit float stereo (renpysound_core.c); the core keeps the callback and calls it whenever the
 * frontend asks for sound, then hands on 16-bit samples. Ren'Py is told the mixer runs at 48 kHz, whatever it asked
 * for, and resamples its own sounds to that.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_AUDIO_H
#define DR_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DR_AUDIO_RATE 48000

typedef void (*dr_mix_fn)(void *userdata, uint8_t *stream, int len);

/* SDL_OpenAudio as Ren'Py calls it: keeps fn and writes back the format the mixer really has (48 kHz, stereo), as
 * SDL does into `desired` when there is no `obtained`. Paused until audio_pause(false). Returns 0. */
int audio_open(int *freq, uint8_t *channels, dr_mix_fn fn, void *userdata);
void audio_pause(bool paused);
void audio_lock(void); /* SDL_LockAudio: the callback does not run while it is held; may be taken again inside */
void audio_unlock(void);
void audio_close(void);

/* The frontend: frames of sound, 16-bit stereo; silence while nothing is open, or while paused. */
void audio_render(int16_t *out, size_t frames);
void audio_float_to_s16(const float *in, int16_t *out, size_t samples);

#endif
```

- [ ] **Step 2: Add the failing checks**

Add `#include "../../src/core/renpy/audio.h"` and `#include <math.h>`, this before `main`, and `check_audio();` in `main`:

```c
/* ---------------------------------------------------------------- the sound */

static float mixed;

static void fake_mixer(void *userdata, uint8_t *stream, int len)
{
    (void)userdata;
    float *samples = (float *)(void *)stream;
    for (int i = 0; i < len / (int)sizeof(float); ++i)
        samples[i] += mixed; /* Ren'Py's mixer adds into a stream SDL has filled with silence */
}

static void check_audio(void)
{
    int freq = 44100;
    uint8_t channels = 1;
    CHECK(audio_open(&freq, &channels, fake_mixer, NULL) == 0, "opened");
    CHECK(freq == DR_AUDIO_RATE && channels == 2, "Ren'Py is told 48000 Hz stereo (%d Hz, %u channels)", freq,
          channels);
    static int16_t out[1600 * 2];
    mixed = 0.5f;
    audio_render(out, 1600);
    CHECK(out[0] == 0 && out[3199] == 0, "silent until Ren'Py starts the sound");
    audio_pause(false);
    audio_render(out, 1600);
    CHECK(out[0] == 16384 && out[3199] == 16384, "half volume is 16384 (%d, %d)", out[0], out[3199]);
    mixed = 2.0f;
    audio_render(out, 4);
    CHECK(out[0] == 32767, "too loud is clipped (%d)", out[0]);
    mixed = -2.0f;
    audio_render(out, 4);
    CHECK(out[0] == -32768, "too loud the other way is clipped (%d)", out[0]);
    audio_lock();
    audio_lock(); /* SDL_LockAudio may be taken twice */
    audio_unlock();
    audio_unlock();
    audio_close();
    audio_render(out, 4);
    CHECK(out[0] == 0, "silent once closed");
    const float odd[3] = {0.0f, NAN, -1.0f};
    int16_t converted[3];
    audio_float_to_s16(odd, converted, 3);
    CHECK(converted[1] == 0 && converted[2] == -32768, "NaN is silence, -1 is -32768");
}
```

- [ ] **Step 3: Run it to see it fail**

Run the Task 4 Step 5 command.
Expected: `undefined symbol: audio_open` and the others.

- [ ] **Step 4: Write `src/core/renpy/audio.c`**

```c
/*
 * Dulcamara - Ren'Py's core: the sound (audio.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "audio.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t lock;
static pthread_once_t lock_made = PTHREAD_ONCE_INIT;
static dr_mix_fn mixer;
static void *mixer_data;
static bool paused = true;

static void make_lock(void)
{
    pthread_mutexattr_t recursive;
    pthread_mutexattr_init(&recursive);
    pthread_mutexattr_settype(&recursive, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock, &recursive);
    pthread_mutexattr_destroy(&recursive);
}

void audio_lock(void)
{
    pthread_once(&lock_made, make_lock);
    pthread_mutex_lock(&lock);
}

void audio_unlock(void)
{
    pthread_mutex_unlock(&lock);
}

int audio_open(int *freq, uint8_t *channels, dr_mix_fn fn, void *userdata)
{
    audio_lock();
    *freq = DR_AUDIO_RATE;
    *channels = 2;
    mixer = fn;
    mixer_data = userdata;
    paused = true;
    audio_unlock();
    return 0;
}

void audio_pause(bool on)
{
    audio_lock();
    paused = on;
    audio_unlock();
}

void audio_close(void)
{
    audio_lock();
    mixer = NULL;
    mixer_data = NULL;
    paused = true;
    audio_unlock();
}

void audio_float_to_s16(const float *in, int16_t *out, size_t samples)
{
    for (size_t i = 0; i < samples; ++i)
    {
        const float v = in[i] * 32768.0f;
        out[i] = v != v ? 0 : v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v;
    }
}

void audio_render(int16_t *out, size_t frames)
{
    float mixed[1024 * 2];
    while (frames)
    {
        const size_t n = frames < 1024 ? frames : 1024;
        audio_lock();
        const bool playing = mixer && !paused;
        if (playing)
        {
            memset(mixed, 0, n * 2 * sizeof(float));
            mixer(mixer_data, (uint8_t *)mixed, (int)(n * 2 * sizeof(float)));
        }
        audio_unlock();
        if (playing)
            audio_float_to_s16(mixed, out, n * 2);
        else
            memset(out, 0, n * 2 * sizeof(int16_t));
        out += n * 2;
        frames -= n;
    }
}
```

- [ ] **Step 5: Run it to see it pass**

Run: `cc -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -g -fsanitize=address,undefined -pthread tests/core/renpy_unit.c src/core/renpy/handoff.c src/core/renpy/frames.c src/core/renpy/audio.c -lm -o build/renpy-unit-try/unit && build/renpy-unit-try/unit`
Expected: `PASS`. Then `rm -rf build/renpy-unit-try` (the build script makes the real one from Task 8 on).

- [ ] **Step 6: Commit**

```bash
git add src/core/renpy/audio.h src/core/renpy/audio.c tests/core/renpy_unit.c
git -c gpg.format=ssh -c user.signingkey=~/.ssh/id_ed25519.pub commit -S -m "Ren'Py core: Ren'Py's mixer, played through the frontend's sound callback"
```

### Task 6: The build script's first stage: Python, FFmpeg, Assimp and the host libraries

**Files:**
- Create: `tools/build-renpy.sh`

**Interfaces:**
- Consumes: `tools/renpy/Setup.local`, `tools/renpy/wheels.txt`, `tools/patches/ffmpeg-4.3.1-*.diff` (Task 1).
- Produces: `build/renpy-host/python/bin/python3.12` (built-in modules from Setup.local, `python3.12-config --embed`), `build/renpy-host/prefix/{include,lib}` (FFmpeg's `libavformat libavcodec libswscale libswresample libavutil`, `libassimp`, and fribidi / SDL2_image when the machine lacks them), `build/renpy-host/tools/` (Cython and setuptools, unpacked), `build/renpy-host/bin/cython`. Task 7 extends the script; the variables `root target renpy out prefix cache jobs cc cxx py log` and the functions `fetch fresh stamp fail` are used by later stages.

- [ ] **Step 1: Write `tools/build-renpy.sh` with its first stage**

```bash
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
sha256() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
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
```

- [ ] **Step 2: Run the stage**

Run: `bash tools/build-renpy.sh host` (in the background; about 10 minutes the first time)
Expected: ends with `Cython version 3.1.4` and `==> [renpy] .../build/renpy-host/python/bin/python3.12`; `build/renpy-host/prefix/lib/libavcodec.a` and `libassimp.a` exist. A second run prints the same two lines at once (the stage is fresh).

- [ ] **Step 3: Check the Python it built**

Run: `build/renpy-host/python/bin/python3.12 -c "import ssl, socket, lzma, bz2, json, zlib; print(ssl.OPENSSL_VERSION)" && build/renpy-host/python/bin/python3.12-config --embed --ldflags`
Expected: an OpenSSL 3 version line, then flags that include `-lpython3.12`.

- [ ] **Step 4: Commit**

```bash
git add tools/build-renpy.sh
git -c gpg.format=ssh -c user.signingkey=~/.ssh/id_ed25519.pub commit -S -m "Ren'Py core: the build's first stage: CPython 3.12.8, FFmpeg 4.3.1 (LGPL), Assimp and the host libraries"
```

### Task 7: Ren'Py's compiled modules, their renamed SDL calls, and the engine folder

**Files:**
- Create: `src/core/renpy/sdl_renames.h`, `src/core/renpy/boot.py`
- Modify: `tools/build-renpy.sh` (stages `modules` and `engine`)

**Interfaces:**
- Consumes: Task 6's `$py`, `$out/bin/cython`, `$prefix`.
- Produces: `build/renpy-host/librenpy-modules.a` (Ren'Py's Cython and C, compiled with `-include sdl_renames.h`), `build/renpy-host/gen/inittab.c` defining `void renpy_extend_inittab(void)`, `build/renpy-host/modules.txt` (one module name a line), `build/renpy-host/assets/renpy8/` (the engine). Every `dr_*` name in `sdl_renames.h` must be defined in `hooks.c` (Task 8) with SDL's own signature.

- [ ] **Step 1: Write `src/core/renpy/sdl_renames.h`**

```c
/*
 * Dulcamara - Ren'Py's core: forced into every C file of Ren'Py's (tools/build-renpy.sh compiles them with -include
 * this file). The SDL calls through which Ren'Py draws, keeps time, waits, plays sound and finds controllers are
 * renamed to the core's own (hooks.c). Because the names change before SDL.h is read, SDL's own declarations are
 * renamed too, so the core's functions keep SDL's signatures exactly. The rest of SDL is called as it is, and SDL
 * itself is not compiled with this file.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_SDL_RENAMES_H
#define DR_SDL_RENAMES_H

/* the controller: Ren'Py sees only the core's (input.c) */
#define SDL_Init dr_Init
#define SDL_InitSubSystem dr_InitSubSystem
#define SDL_IsGameController dr_IsGameController
#define SDL_JoystickOpen dr_JoystickOpen
#define SDL_GameControllerOpen dr_GameControllerOpen

/* time and waiting: the core's clock (handoff.c) */
#define SDL_GetTicks dr_GetTicks
#define SDL_GetTicks64 dr_GetTicks64
#define SDL_Delay dr_Delay
#define SDL_AddTimer dr_AddTimer
#define SDL_RemoveTimer dr_RemoveTimer
#define SDL_PollEvent dr_PollEvent
#define SDL_WaitEvent dr_WaitEvent
#define SDL_WaitEventTimeout dr_WaitEventTimeout

/* the window and the display: the picture's size */
#define SDL_CreateWindow dr_CreateWindow
#define SDL_GetWindowSize dr_GetWindowSize
#define SDL_SetWindowSize dr_SetWindowSize
#define SDL_SetWindowFullscreen dr_SetWindowFullscreen
#define SDL_GetWindowFlags dr_GetWindowFlags
#define SDL_GetNumVideoDisplays dr_GetNumVideoDisplays
#define SDL_GetDisplayBounds dr_GetDisplayBounds
#define SDL_GetDisplayUsableBounds dr_GetDisplayUsableBounds
#define SDL_GetNumDisplayModes dr_GetNumDisplayModes
#define SDL_GetDisplayMode dr_GetDisplayMode
#define SDL_GetDesktopDisplayMode dr_GetDesktopDisplayMode
#define SDL_GetCurrentDisplayMode dr_GetCurrentDisplayMode

/* OpenGL: the core's context and framebuffer (gl.c) */
#define SDL_GL_LoadLibrary dr_GL_LoadLibrary
#define SDL_GL_UnloadLibrary dr_GL_UnloadLibrary
#define SDL_GL_GetProcAddress dr_GL_GetProcAddress
#define SDL_GL_ExtensionSupported dr_GL_ExtensionSupported
#define SDL_GL_SetAttribute dr_GL_SetAttribute
#define SDL_GL_GetAttribute dr_GL_GetAttribute
#define SDL_GL_ResetAttributes dr_GL_ResetAttributes
#define SDL_GL_CreateContext dr_GL_CreateContext
#define SDL_GL_MakeCurrent dr_GL_MakeCurrent
#define SDL_GL_GetCurrentWindow dr_GL_GetCurrentWindow
#define SDL_GL_GetCurrentContext dr_GL_GetCurrentContext
#define SDL_GL_GetDrawableSize dr_GL_GetDrawableSize
#define SDL_GL_SetSwapInterval dr_GL_SetSwapInterval
#define SDL_GL_GetSwapInterval dr_GL_GetSwapInterval
#define SDL_GL_SwapWindow dr_GL_SwapWindow
#define SDL_GL_DeleteContext dr_GL_DeleteContext

/* sound: the core's mixer (audio.c) */
#define SDL_OpenAudio dr_OpenAudio
#define SDL_PauseAudio dr_PauseAudio
#define SDL_LockAudio dr_LockAudio
#define SDL_UnlockAudio dr_UnlockAudio
#define SDL_CloseAudio dr_CloseAudio

#endif
```

- [ ] **Step 2: Check the list against Ren'Py's code**

Run: `grep -rhoE "\bSDL_[A-Za-z0-9_]+\s*\(" third_party/renpy/renpy --include=*.pyx --include=*.pxd third_party/renpy/src/*.c | sed 's/\s*($//' | sort -u | grep -E "SDL_(GL_|Delay|GetTicks|Wait|PollEvent|.*Timer|.*Audio|GetDisplay|GetDesktop|GetCurrentDisplay|GetNumDisplay|GetNumVideo|CreateWindow|GetWindowSize|SetWindowSize|SetWindowFullscreen|GetWindowFlags|Init|IsGameController|JoystickOpen|GameControllerOpen|GetPerformance)"`
Expected: every name it prints is in `sdl_renames.h`, except calls that are fine as SDL does them. If it prints `SDL_GetPerformanceCounter`/`SDL_GetPerformanceFrequency`, add both (`dr_GetPerformanceCounter` returns the clock in microseconds, `dr_GetPerformanceFrequency` returns 1000000) here and in Task 8's `hooks.c`. If it prints another time, wait or GL call not listed, add it the same way and note it in the commit message.

- [ ] **Step 3: Write `src/core/renpy/boot.py`**

```python
# Dulcamara - starts Ren'Py 8 inside Dulcamara's core. src/core/renpy/python.c runs this file, shipped as the
# engine's dulcamara_boot.py, as __main__ on Ren'Py's own thread, with sys.argv = [this file, the engine folder, the
# game folder, the saves folder].
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ren'Py keeps the core's time: time.time and the other clocks read the clock the frontend moves, and time.sleep
on Ren'Py's thread waits by it, giving control back meanwhile. Its output goes to Dulcamara's log, its errors are
logged in full before it shows them, and its saves go where the core says."""

import importlib.abc
import io
import os
import runpy
import sys
import threading
import time

import _dulcamara

engine, game, saves = sys.argv[1:4]


class Log(io.TextIOBase):
    """sys.stdout (level 1) and sys.stderr (level 2): each line to Dulcamara's log."""

    def __init__(self, level):
        super().__init__()
        self.level = level
        self.pending = ""

    def write(self, text):
        self.pending += text
        *lines, self.pending = self.pending.split("\n")
        for line in lines:
            _dulcamara.log(self.level, line)
        return len(text)

    def flush(self):
        if self.pending:
            _dulcamara.log(self.level, self.pending)
            self.pending = ""

    def isatty(self):
        return False


sys.stdout = Log(1)
sys.stderr = Log(2)

renpy_thread = threading.get_ident()
real_sleep = time.sleep


def sleep(seconds):
    if threading.get_ident() == renpy_thread:
        _dulcamara.sleep(seconds)
    else:
        real_sleep(seconds)


time.time = _dulcamara.time
time.monotonic = _dulcamara.monotonic
time.perf_counter = _dulcamara.monotonic
time.sleep = sleep


def log_error(text):
    _dulcamara.log(2, "Ren'Py reported an error:")
    for line in text.splitlines():
        _dulcamara.log(2, line)


def wrap_error_reports(module):
    """renpy.error.report_exception writes traceback.txt; renpy.display.error.report_parse_errors shows the script's
    errors. Each is logged in full, then Ren'Py goes on to show it as it would."""
    if module.__name__ == "renpy.error":
        report_exception = module.report_exception

        def logged_report_exception(e, *args, **kwargs):
            te = report_exception(e, *args, **kwargs)
            try:
                with open(te.traceback_fn, encoding="utf-8", errors="replace") as f:
                    log_error(f.read())
            except Exception:
                log_error(repr(e))
            return te

        module.report_exception = logged_report_exception
    elif module.__name__ == "renpy.display.error":
        report_parse_errors = module.report_parse_errors

        def logged_report_parse_errors(errors, error_fn, *args, **kwargs):
            log_error("\n".join(errors))
            return report_parse_errors(errors, error_fn, *args, **kwargs)

        module.report_parse_errors = logged_report_parse_errors


class ErrorReports(importlib.abc.MetaPathFinder):
    """Wraps the error reports as their modules load: Ren'Py imports them itself, and again when it reloads."""

    names = ("renpy.error", "renpy.display.error")

    def find_spec(self, name, path, target=None):
        if name not in self.names:
            return None
        for finder in sys.meta_path:
            if finder is self or not hasattr(finder, "find_spec"):
                continue
            spec = finder.find_spec(name, path, target)
            if spec is not None and spec.loader is not None:
                exec_module = spec.loader.exec_module

                def exec_and_wrap(module, exec_module=exec_module):
                    exec_module(module)
                    wrap_error_reports(module)

                spec.loader.exec_module = exec_and_wrap
                return spec
        return None


sys.meta_path.insert(0, ErrorReports())
os.environ["RENPY_PATH_TO_SAVES"] = saves
# MultiPersistent data goes to ~/.renpy (or ~/Library/RenPy on macOS): with HOME the saves folder, it stays with
# the saves, and the host check never writes into the user's own home.
os.environ["HOME"] = saves
sys.argv = [os.path.join(engine, "renpy.py"), game]
status = 0
try:
    runpy.run_path(sys.argv[0], run_name="__main__")
except SystemExit as e:
    status = e.code if isinstance(e.code, int) else 0
except BaseException:
    import traceback

    log_error(traceback.format_exc())
    status = 1
finally:
    sys.stdout.flush()
    sys.stderr.flush()
_dulcamara.log(1, f"Ren'Py ended ({status})")
```

- [ ] **Step 4: Add the `modules` and `engine` stages to `tools/build-renpy.sh`**

Append after the deps stage (before the final `echo`, which moves to the end of the file):

```bash
# ---- Ren'Py's own C: setup.py turns its .pyx into C for a static build (RENPY_STATIC: each module's init function
# named after its whole dotted name), then each module listed in src/Setup is compiled with the core's renamed SDL
# calls (src/core/renpy/sdl_renames.h) and registered in an inittab. tinyfiledialogs (renpy.tfd) is left out: it is
# optional, and opens desktop file dialogs.
sdl_cflags() { pkg-config --cflags sdl2 SDL2_image freetype2 harfbuzz fribidi libpng libjpeg libwebp zlib; }
build_modules() {
    echo "==> [renpy] Ren'Py's modules (Cython $("$out/bin/cython" --version 2>&1 | awk '{print $NF}'))"
    local src="$out/renpy-src" objects="$out/modules" generated
    rm -rf "$src" "$objects" "$out/gen"
    mkdir -p "$src" "$objects" "$out/gen"
    git -C "$renpy" archive HEAD | tar -x -C "$src"
    (cd "$src" && RENPY_STATIC=1 RENPY_REGENERATE_CYTHON=1 RENPY_CYTHON="$out/bin/cython" PYTHONPATH="$out/tools" \
        CC="$cc" CXX="$cxx" CFLAGS="$(sdl_cflags)" "$py" setup.py generate) >"$log/modules.log" 2>&1 ||
        fail modules
    generated="$src/tmp/gen3-static"
    [[ -d $generated ]] || { echo "Ren'Py's setup.py did not write $generated" >&2; exit 1; }
    local cflags=(-O2 -g -fPIC -w -fno-strict-aliasing -include "$root/src/core/renpy/sdl_renames.h"
        -I"$src/src" -I"$generated" $("$out/python/bin/python3.12-config" --includes) $(sdl_cflags)
        -I"$prefix/include")
    local modules=() sources=() line parts s
    while read -r line; do
        line=${line%%#*}
        read -r -a parts <<<"$line"
        [[ ${#parts[@]} -eq 0 || ${parts[0]} == renpy.tfd ]] && continue
        modules+=("${parts[0]}")
        for s in "${parts[@]:1}"; do
            sources+=("$src/src/${s/..\/tmp\/gen\//../tmp/gen3-static/}")
        done
    done <"$src/src/Setup"
    printf '%s\n' "${modules[@]}" >"$out/modules.txt"
    # compile every source once, $jobs at a time
    local pids=() objs=() f o
    for f in $(printf '%s\n' "${sources[@]}" | sort -u); do
        o="$objects/$(basename "${f%.*}").o"
        objs+=("$o")
        if [[ $f == *.cc ]]; then
            "$cxx" -std=c++17 "${cflags[@]}" -c "$f" -o "$o" >>"$log/modules.log" 2>&1 &
        else
            "$cc" -std=gnu99 "${cflags[@]}" -c "$f" -o "$o" >>"$log/modules.log" 2>&1 &
        fi
        pids+=($!)
        if (( ${#pids[@]} >= jobs )); then wait "${pids[0]}" || fail modules; pids=("${pids[@]:1}"); fi
    done
    for f in "${pids[@]}"; do wait "$f" || fail modules; done
    {
        printf '/* Dulcamara - written by tools/build-renpy.sh: the engine'"'"'s compiled modules, built in. */\n'
        printf '#include <Python.h>\n\n'
        for s in "${modules[@]}"; do printf 'PyMODINIT_FUNC PyInit_%s(void);\n' "${s//./_}"; done
        printf '\nstatic struct _inittab renpy_modules[] = {\n'
        for s in "${modules[@]}"; do printf '    {"%s", PyInit_%s},\n' "$s" "${s//./_}"; done
        printf '    {NULL, NULL},\n};\n\nvoid renpy_extend_inittab(void)\n{\n    PyImport_ExtendInittab(renpy_modules);\n}\n'
    } >"$out/gen/inittab.c"
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
    cp "$root/src/core/renpy/boot.py" "$engine/dulcamara_boot.py"
    unpack_wheels engine "$engine/lib/site-packages"
    "$py" -m compileall -q -j 0 --invalidation-mode unchecked-hash "$engine/renpy" "$engine/lib/site-packages" \
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

modules_key="modules-1 $(git -C "$renpy" rev-parse HEAD) $deps_key $(digest "$root/src/core/renpy/sdl_renames.h")"
if ! fresh modules "$modules_key"; then
    build_modules
    stamp modules "$modules_key"
fi
engine_key="engine-1 $modules_key $(digest "$root/src/core/renpy/boot.py" "$root/tools/renpy/wheels.txt")"
if ! fresh engine "$engine_key"; then
    build_engine
    stamp engine "$engine_key"
fi
echo "==> [renpy] $(wc -l <"$out/modules.txt" | tr -d ' ') Ren'Py modules built in, the engine in $out/assets/renpy8"
```

Remove the earlier closing `echo "==> [renpy] $py"` and the bare `"$out/bin/cython" --version` line; the new final line replaces them.

- [ ] **Step 5: Run it**

Run: `bash tools/build-renpy.sh host`
Expected: ends `==> [renpy] N Ren'Py modules built in, the engine in .../build/renpy-host/assets/renpy8` with N about 70. If `setup.py generate` fails, read `build/renpy-host/logs/modules.log`: a missing Python module in our CPython is fixed in `Setup.local`; a missing build tool in `wheels.txt`.

- [ ] **Step 6: Check what was built**

Run:
```bash
nm build/renpy-host/librenpy-modules.a 2>/dev/null | grep -c ' T PyInit_renpy_'
nm build/renpy-host/librenpy-modules.a 2>/dev/null | grep -E ' U (dr_|_dr_)' | awk '{print $2}' | sort -u
build/renpy-host/python/bin/python3.12 -c "import zipfile; z = zipfile.ZipFile('build/renpy-host/assets/renpy8/lib/python312.zip'); print(len(z.namelist()), 'encodings/__init__.pyc' in z.namelist())"
ls build/renpy-host/assets/renpy8 build/renpy-host/assets/renpy8/renpy/common | head -20
```
Expected: the count of `PyInit_renpy_` functions equals the module count minus `_renpy` (which is `PyInit__renpy`); the `dr_` list shows the renamed calls Ren'Py really uses (the functions Task 8 defines); the zip holds several hundred files and `True`; the engine has `renpy/`, `renpy.py`, `dulcamara_boot.py`, `lib/`, and `renpy/common` has `00*.rpy` files.

- [ ] **Step 7: Commit**

```bash
git add src/core/renpy/sdl_renames.h src/core/renpy/boot.py tools/build-renpy.sh
git -c gpg.format=ssh -c user.signingkey=~/.ssh/id_ed25519.pub commit -S -m "Ren'Py core: Ren'Py's modules with their SDL calls renamed to the core's, and the engine folder"
```

### Task 8: OpenGL, the hooks, the controller and the log

**Files:**
- Create: `src/core/renpy/gl.h`, `gl.c`, `gl_backend.h`, `gl_egl.c`, `gl_cgl.c`, `hooks.h`, `hooks.c`, `input.h`, `input.c`, `log.h`, `log.c`
- Modify: `tests/core/renpy_unit.c` (add `check_gl`), `tools/build-renpy.sh` (stage `core`: the unit check)

**Interfaces:**
- Consumes: `frames_back`, `frames_publish`, `frames_copy_flipped`, `frames_fit` (Task 4); `audio_*` (Task 5); `handoff_*` (Task 3); the `dr_*` names of `sdl_renames.h` (Task 7).
- Produces (exact):

```c
/* gl.h */
bool gl_create(int w, int h, int depth_bits, char *error, size_t size);
void *gl_proc(const char *name);
bool gl_make_current(bool on);
void gl_resize(int w, int h);
void gl_present(void);
void gl_flush_pending(void);
void gl_destroy(void);
const char *gl_renderer(void);
/* hooks.h */
#define DR_MAX_W 1920
#define DR_MAX_H 1080
void hooks_reset(void);
void hooks_sleep_until(int64_t until_usec);
/* input.h */
void input_attach(void);
bool input_is_ours(int device_index);
void input_set(uint16_t retropad, int16_t left_x, int16_t left_y);
void input_detach(void);
/* log.h */
void log_queue(int level, const char *fmt, ...);
void log_flush(void (*emit)(int level, const char *line));
```

- [ ] **Step 1: Write the failing GL check in `tests/core/renpy_unit.c`**

Add `#include "../../src/core/renpy/gl.h"`, this before `main`, and `check_gl();` as the last check in `main`:

```c
/* ---------------------------------------------------------------- OpenGL */

/* The read-back: a picture cleared orange, its top half then blue, comes out XRGB8888 with its top row first.
 * Skipped where this computer has no OpenGL without a screen (DULCAMARA_NO_GL, or no context). */
static void check_gl(void)
{
    char error[256] = "";
    if (getenv("DULCAMARA_NO_GL") || !gl_create(64, 32, 0, error, sizeof(error)))
    {
        printf("gl: skipped (%s)\n", error[0] ? error : "DULCAMARA_NO_GL");
        return;
    }
    printf("gl: %s\n", gl_renderer());
    void (*clear_color)(float, float, float, float) = (void (*)(float, float, float, float))gl_proc("glClearColor");
    void (*clear)(unsigned) = (void (*)(unsigned))gl_proc("glClear");
    void (*enable)(unsigned) = (void (*)(unsigned))gl_proc("glEnable");
    void (*disable)(unsigned) = (void (*)(unsigned))gl_proc("glDisable");
    void (*scissor)(int, int, int, int) = (void (*)(int, int, int, int))gl_proc("glScissor");
    CHECK(clear_color && clear && enable && disable && scissor, "GL's functions found");
    if (!(clear_color && clear && enable && disable && scissor))
        return;
    frames_reset();
    clear_color(1.0f, 0.5f, 0.0f, 1.0f);
    clear(0x4000);   /* GL_COLOR_BUFFER_BIT */
    enable(0x0C11);  /* GL_SCISSOR_TEST: GL's rows 16..31, the top half */
    scissor(0, 16, 64, 16);
    clear_color(0.0f, 0.0f, 1.0f, 1.0f);
    clear(0x4000);
    disable(0x0C11);
    gl_present();
    gl_flush_pending(); /* Ren'Py waits after it draws: the picture read for it is published then */
    int w = 0, h = 0;
    bool fresh = false;
    const uint32_t *p = frames_take(&w, &h, &fresh);
    CHECK(p && w == 64 && h == 32 && fresh, "a fresh 64x32 picture (%dx%d)", w, h);
    if (p)
    {
        CHECK((p[0] & 0xffffff) == 0x0000ff, "the top row is blue (%06x): GL's rows are turned over",
              p[0] & 0xffffff);
        const uint32_t bottom = p[31 * 64] & 0xffffff;
        CHECK(bottom == 0xff8000 || bottom == 0xff7f00, "the bottom row is orange (%06x)", bottom);
    }
    gl_destroy();
    frames_reset();
}
```

- [ ] **Step 2: Write `src/core/renpy/gl.h` and `gl_backend.h`**

```c
/*
 * Dulcamara - Ren'Py's core: the OpenGL context Ren'Py draws with, made by the core instead of SDL (on the PS5 and
 * Linux through EGL, gl_egl.c; on macOS through CGL, gl_cgl.c), and a framebuffer object of the window's size bound
 * as Ren'Py starts, which Ren'Py's gl2 then takes as the screen's (gl2draw.pyx, init_fbo). Each shown frame is read
 * into a pixel-pack buffer and copied out one frame later, when the read has finished, so the GPU is never waited
 * on; the last frame before Ren'Py waits is copied out when it waits (gl_flush_pending).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_GL_H
#define DR_GL_H

#include <stdbool.h>
#include <stddef.h>

/* A context current on this thread, with the framebuffer bound. False with a reason in error. */
bool gl_create(int w, int h, int depth_bits, char *error, size_t size);
void *gl_proc(const char *name); /* SDL_GL_GetProcAddress */
bool gl_make_current(bool on);
void gl_resize(int w, int h); /* the window's new size: the framebuffer's storage follows, its name stays */
void gl_present(void);        /* SDL_GL_SwapWindow: read this frame, publish the one before */
void gl_flush_pending(void);  /* publish the frame still being read */
void gl_destroy(void);
const char *gl_renderer(void); /* "<GL_RENDERER>, OpenGL <GL_VERSION>" for the log */

#endif
```

```c
/*
 * Dulcamara - Ren'Py's core: what gl.c needs from EGL or CGL.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_GL_BACKEND_H
#define DR_GL_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

/* A desktop OpenGL context (2.0 or later, compatibility), current on this thread, with no window. */
bool backend_create(char *error, size_t size);
void *backend_proc(const char *name);
bool backend_make_current(bool on);
void backend_destroy(void);

#endif
```

- [ ] **Step 3: Write `src/core/renpy/gl.c`**

```c
/*
 * Dulcamara - Ren'Py's core: OpenGL (gl.h). GL's names are declared here, the few the core needs, so that no GL
 * header is needed; the functions come from the backend, as Ren'Py's do.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gl.h"

#include "frames.h"
#include "gl_backend.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLubyte, GLboolean;
typedef intptr_t GLsizeiptr, GLintptr;

#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_STREAM_READ 0x88E1
#define GL_READ_ONLY 0x88B8
#define GL_MAP_READ_BIT 0x0001
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

static struct
{
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    void (*GenRenderbuffers)(GLsizei, GLuint *);
    void (*DeleteRenderbuffers)(GLsizei, const GLuint *);
    void (*BindRenderbuffer)(GLenum, GLuint);
    void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (*GenBuffers)(GLsizei, GLuint *);
    void (*DeleteBuffers)(GLsizei, const GLuint *);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
    void *(*MapBuffer)(GLenum, GLenum);
    void *(*MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
    GLboolean (*UnmapBuffer)(GLenum);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (*GetIntegerv)(GLenum, GLint *);
    void (*PixelStorei)(GLenum, GLint);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    const GLubyte *(*GetString)(GLenum);
} gl;

static bool made;
static GLuint fbo, color, depth, pbo[2];
static int width, height;
static int reading[2][2]; /* the size each pixel-pack buffer holds a frame of; 0 when it holds none */
static int next_pbo;

static const char *load(void)
{
#define LOAD(name)                                                                                                 \
    if (!(*(void **)&gl.name = backend_proc("gl" #name)))                                                          \
        return "gl" #name;
    LOAD(GenFramebuffers) LOAD(DeleteFramebuffers) LOAD(BindFramebuffer) LOAD(CheckFramebufferStatus)
    LOAD(FramebufferRenderbuffer) LOAD(GenRenderbuffers) LOAD(DeleteRenderbuffers) LOAD(BindRenderbuffer)
    LOAD(RenderbufferStorage) LOAD(GenBuffers) LOAD(DeleteBuffers) LOAD(BindBuffer) LOAD(BufferData)
    LOAD(UnmapBuffer) LOAD(ReadPixels) LOAD(GetIntegerv) LOAD(PixelStorei) LOAD(Viewport) LOAD(GetString)
#undef LOAD
    /* glMapBuffer in desktop GL (2.1 has no glMapBufferRange), glMapBufferRange where that is all there is */
    *(void **)&gl.MapBuffer = backend_proc("glMapBuffer");
    *(void **)&gl.MapBufferRange = backend_proc("glMapBufferRange");
    return gl.MapBuffer || gl.MapBufferRange ? NULL : "glMapBuffer";
}

static void storage(void)
{
    gl.BindRenderbuffer(GL_RENDERBUFFER, color);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    if (depth)
    {
        gl.BindRenderbuffer(GL_RENDERBUFFER, depth);
        gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    }
    gl.BindRenderbuffer(GL_RENDERBUFFER, 0);
}

bool gl_create(int w, int h, int depth_bits, char *error, size_t size)
{
    if (made)
        gl_destroy();
    if (!backend_create(error, size))
        return false;
    const char *missing = load();
    if (missing)
    {
        snprintf(error, size, "OpenGL has no %s", missing);
        backend_destroy();
        return false;
    }
    width = w;
    height = h;
    depth = 0;
    gl.GenFramebuffers(1, &fbo);
    gl.GenRenderbuffers(1, &color);
    if (depth_bits > 0)
        gl.GenRenderbuffers(1, &depth);
    storage();
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (depth)
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    const GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        snprintf(error, size, "the %dx%d framebuffer is incomplete (0x%04x)", w, h, status);
        made = true;
        gl_destroy();
        return false;
    }
    gl.Viewport(0, 0, w, h);
    gl.GenBuffers(2, pbo);
    memset(reading, 0, sizeof(reading));
    next_pbo = 0;
    made = true; /* the framebuffer stays bound: Ren'Py takes it as the screen's */
    return true;
}

void *gl_proc(const char *name)
{
    return backend_proc(name);
}

bool gl_make_current(bool on)
{
    return backend_make_current(on);
}

void gl_resize(int w, int h)
{
    if (!made || (w == width && h == height) || w <= 0 || h <= 0)
        return;
    width = w;
    height = h;
    storage();
}

/* Copies out the frame pixel-pack buffer `slot` holds, if any, and publishes it. */
static void publish(int slot)
{
    const int w = reading[slot][0], h = reading[slot][1];
    if (!w)
        return;
    reading[slot][0] = reading[slot][1] = 0;
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]);
    const void *pixels = gl.MapBuffer ? gl.MapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY)
                                      : gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)w * h * 4,
                                                          GL_MAP_READ_BIT);
    if (pixels)
    {
        uint32_t *out = frames_back(w, h);
        if (out)
        {
            frames_copy_flipped(out, (const uint32_t *)pixels, w, h);
            frames_publish();
        }
        gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
}

typedef struct
{
    GLint framebuffer, pack_buffer, pack_alignment;
} Bindings;

static Bindings save_bindings(void)
{
    Bindings b = {0, 0, 4};
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &b.framebuffer);
    gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &b.pack_buffer);
    gl.GetIntegerv(GL_PACK_ALIGNMENT, &b.pack_alignment);
    return b;
}

static void restore_bindings(Bindings b)
{
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)b.pack_buffer);
    gl.PixelStorei(GL_PACK_ALIGNMENT, b.pack_alignment);
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)b.framebuffer);
}

void gl_present(void)
{
    if (!made)
        return;
    const Bindings b = save_bindings();
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    const int slot = next_pbo;
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]);
    gl.BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)width * height * 4, NULL, GL_STREAM_READ);
    gl.ReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    reading[slot][0] = width;
    reading[slot][1] = height;
    next_pbo = slot ^ 1;
    publish(next_pbo); /* the frame before this one, read a frame ago */
    restore_bindings(b);
}

void gl_flush_pending(void)
{
    if (!made || !reading[next_pbo ^ 1][0])
        return;
    const Bindings b = save_bindings();
    publish(next_pbo ^ 1);
    restore_bindings(b);
}

void gl_destroy(void)
{
    if (!made)
        return;
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.DeleteBuffers(2, pbo);
    gl.DeleteFramebuffers(1, &fbo);
    gl.DeleteRenderbuffers(1, &color);
    if (depth)
        gl.DeleteRenderbuffers(1, &depth);
    fbo = color = depth = 0;
    made = false;
    backend_destroy();
}

const char *gl_renderer(void)
{
    static char text[256];
    if (!made)
        return "no OpenGL";
    snprintf(text, sizeof(text), "%s, OpenGL %s", (const char *)gl.GetString(GL_RENDERER),
             (const char *)gl.GetString(GL_VERSION));
    return text;
}
```

- [ ] **Step 4: Write `src/core/renpy/gl_cgl.c` and `gl_egl.c`**

```c
/*
 * Dulcamara - Ren'Py's core: OpenGL through CGL, on macOS (the host check): a context with no window, as legacy
 * OpenGL 2.1, the version Ren'Py's gl2 asks for (2.0) is within.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define GL_SILENCE_DEPRECATION
#include "gl_backend.h"

#include <OpenGL/OpenGL.h>
#include <dlfcn.h>
#include <stdio.h>

static CGLContextObj context;
static void *framework;

bool backend_create(char *error, size_t size)
{
    const CGLPixelFormatAttribute attributes[] = {kCGLPFAAccelerated, kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8,
                                                  kCGLPFAAllowOfflineRenderers, 0};
    CGLPixelFormatObj format = NULL;
    GLint count = 0;
    if (CGLChoosePixelFormat(attributes, &format, &count) != kCGLNoError || !format)
    {
        snprintf(error, size, "CGL: no pixel format");
        return false;
    }
    const CGLError made = CGLCreateContext(format, NULL, &context);
    CGLDestroyPixelFormat(format);
    if (made != kCGLNoError || CGLSetCurrentContext(context) != kCGLNoError)
    {
        snprintf(error, size, "CGL: no context (%d)", (int)made);
        return false;
    }
    if (!framework)
        framework = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
    return true;
}

void *backend_proc(const char *name)
{
    return framework ? dlsym(framework, name) : NULL;
}

bool backend_make_current(bool on)
{
    return CGLSetCurrentContext(on ? context : NULL) == kCGLNoError;
}

void backend_destroy(void)
{
    CGLSetCurrentContext(NULL);
    if (context)
        CGLDestroyContext(context);
    context = NULL;
}
```

```c
/*
 * Dulcamara - Ren'Py's core: OpenGL through EGL. On the PS5, ps5-opengl's display with a small pbuffer (milestone 2);
 * on Linux (the host check in CI), Mesa's surfaceless platform. A compatibility context: Ren'Py's gl2 asks for
 * OpenGL 2.0 without a profile.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gl_backend.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;
static EGLSurface surface = EGL_NO_SURFACE;

bool backend_create(char *error, size_t size)
{
#if defined(__PROSPERO__)
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    display = platform_display ? platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                               : eglGetDisplay(EGL_DEFAULT_DISPLAY);
#endif
    EGLint major = 0, minor = 0, count = 0;
    EGLConfig config = NULL;
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                        EGL_NONE};
    const EGLint context_attributes[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                         EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR, EGL_NONE};
    const EGLint pbuffer_attributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const char *step = "eglGetDisplay";
    if (display == EGL_NO_DISPLAY)
        goto failed;
    step = "eglInitialize";
    if (!eglInitialize(display, &major, &minor))
        goto failed;
    step = "eglBindAPI";
    if (!eglBindAPI(EGL_OPENGL_API))
        goto failed;
    step = "eglChooseConfig";
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1)
        goto failed;
    step = "eglCreatePbufferSurface";
    surface = eglCreatePbufferSurface(display, config, pbuffer_attributes);
    if (surface == EGL_NO_SURFACE)
        goto failed;
    step = "eglCreateContext";
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT)
        goto failed;
    step = "eglMakeCurrent";
    if (!eglMakeCurrent(display, surface, surface, context))
        goto failed;
    return true;
failed:
    snprintf(error, size, "EGL: %s failed (0x%04x)", step, (unsigned)eglGetError());
    backend_destroy();
    return false;
}

void *backend_proc(const char *name)
{
    return (void *)eglGetProcAddress(name);
}

bool backend_make_current(bool on)
{
    return on ? eglMakeCurrent(display, surface, surface, context)
              : eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void backend_destroy(void)
{
    if (display == EGL_NO_DISPLAY)
        return;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context != EGL_NO_CONTEXT)
        eglDestroyContext(display, context);
    if (surface != EGL_NO_SURFACE)
        eglDestroySurface(display, surface);
    eglTerminate(display);
    display = EGL_NO_DISPLAY;
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;
}
```

- [ ] **Step 5: Write `src/core/renpy/log.h` and `log.c`**

```c
/*
 * Dulcamara - Ren'Py's core: log lines from any thread (Ren'Py's, its helpers'), held until retro_run hands them to
 * the frontend's log on the frontend's thread.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_LOG_H
#define DR_LOG_H

/* level 1: information, 2: a warning or an error */
void log_queue(int level, const char *fmt, ...);
/* The frontend: every held line, oldest first. */
void log_flush(void (*emit)(int level, const char *line));

#endif
```

```c
/*
 * Dulcamara - Ren'Py's core: the held log lines (log.h). When more arrive than it holds between two frames (a long
 * traceback while the frontend is busy), the oldest are dropped and the drop is logged.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#define LINES 512
#define LINE_SIZE 1024

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char lines[LINES][LINE_SIZE];
static int levels[LINES];
static unsigned first, count, dropped;

void log_queue(int level, const char *fmt, ...)
{
    char line[LINE_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    pthread_mutex_lock(&lock);
    if (count == LINES)
    {
        first = (first + 1) % LINES;
        --count;
        ++dropped;
    }
    const unsigned at = (first + count) % LINES;
    snprintf(lines[at], LINE_SIZE, "%s", line);
    levels[at] = level;
    ++count;
    pthread_mutex_unlock(&lock);
}

void log_flush(void (*emit)(int level, const char *line))
{
    char line[LINE_SIZE];
    for (;;)
    {
        pthread_mutex_lock(&lock);
        if (dropped)
        {
            snprintf(line, sizeof(line), "(%u log lines dropped)", dropped);
            dropped = 0;
            pthread_mutex_unlock(&lock);
            emit(2, line);
            continue;
        }
        if (!count)
        {
            pthread_mutex_unlock(&lock);
            return;
        }
        snprintf(line, sizeof(line), "%s", lines[first]);
        const int level = levels[first];
        first = (first + 1) % LINES;
        --count;
        pthread_mutex_unlock(&lock);
        emit(level, line);
    }
}
```

- [ ] **Step 6: Write `src/core/renpy/input.h` and `input.c`**

```c
/*
 * Dulcamara - Ren'Py's core: the controller Ren'Py sees, an SDL virtual controller the RetroPad drives. Ren'Py's
 * own controller bindings then apply: SDL's A (the bottom button, Cross) confirms, B (Circle) opens the game menu,
 * the shoulders roll back and forward, the D-pad moves the focus.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_INPUT_H
#define DR_INPUT_H

#include <stdbool.h>
#include <stdint.h>

void input_attach(void); /* once SDL's joysticks are up (Ren'Py's thread) */
bool input_is_ours(int device_index);
/* The frontend: the RetroPad's buttons (bit = RETRO_DEVICE_ID_JOYPAD_*) and left stick. */
void input_set(uint16_t retropad, int16_t left_x, int16_t left_y);
void input_detach(void);

#endif
```

```c
/*
 * Dulcamara - Ren'Py's core: the controller (input.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "input.h"

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_Joystick *pad;
static SDL_JoystickID pad_id = -1;
static uint16_t held;
static int16_t stick_x, stick_y;

/* The RetroPad's buttons as SDL's, by where they sit: the RetroPad's B is the bottom face button, SDL's A. L2 and R2
 * are SDL's triggers (axes), so -1 here. */
static const int BUTTONS[16] = {
    SDL_CONTROLLER_BUTTON_A,            /* B */
    SDL_CONTROLLER_BUTTON_X,            /* Y */
    SDL_CONTROLLER_BUTTON_BACK,         /* SELECT */
    SDL_CONTROLLER_BUTTON_START,        /* START */
    SDL_CONTROLLER_BUTTON_DPAD_UP,      /* UP */
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,    /* DOWN */
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,    /* LEFT */
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,   /* RIGHT */
    SDL_CONTROLLER_BUTTON_B,            /* A */
    SDL_CONTROLLER_BUTTON_Y,            /* X */
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, /* L */
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, /* R */
    -1,                                 /* L2 */
    -1,                                 /* R2 */
    SDL_CONTROLLER_BUTTON_LEFTSTICK,    /* L3 */
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,   /* R3 */
};

static void apply(void)
{
    for (int b = 0; b < 16; ++b)
        if (BUTTONS[b] >= 0)
            SDL_JoystickSetVirtualButton(pad, BUTTONS[b], (Uint8)((held >> b) & 1));
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTX, stick_x);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTY, stick_y);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, (held >> 12) & 1 ? 32767 : 0);
    SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, (held >> 13) & 1 ? 32767 : 0);
}

void input_attach(void)
{
    pthread_mutex_lock(&lock);
    if (!pad)
    {
        SDL_VirtualJoystickDesc d;
        SDL_zero(d);
        d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        d.naxes = SDL_CONTROLLER_AXIS_MAX;
        d.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
        d.button_mask = (1u << d.nbuttons) - 1;
        d.axis_mask = (1u << d.naxes) - 1;
        d.name = "Dulcamara controller";
        const int index = SDL_JoystickAttachVirtualEx(&d);
        if (index >= 0 && !SDL_IsGameController(index))
        {
            /* an SDL without a mapping for virtual controllers: the one their button order means */
            char guid[64], mapping[512];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
            snprintf(mapping, sizeof(mapping),
                     "%s,Dulcamara controller,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,"
                     "rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
                     "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,",
                     guid);
            SDL_GameControllerAddMapping(mapping);
        }
        pad = index >= 0 ? SDL_JoystickOpen(index) : NULL;
        pad_id = pad ? SDL_JoystickInstanceID(pad) : -1;
        if (pad)
            apply();
    }
    pthread_mutex_unlock(&lock);
}

bool input_is_ours(int device_index)
{
    pthread_mutex_lock(&lock);
    const bool ours = pad && SDL_JoystickGetDeviceInstanceID(device_index) == pad_id;
    pthread_mutex_unlock(&lock);
    return ours;
}

void input_set(uint16_t retropad, int16_t left_x, int16_t left_y)
{
    pthread_mutex_lock(&lock);
    held = retropad;
    stick_x = left_x;
    stick_y = left_y;
    if (pad)
        apply();
    pthread_mutex_unlock(&lock);
}

void input_detach(void)
{
    pthread_mutex_lock(&lock);
    if (pad)
    {
        SDL_JoystickClose(pad);
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_JoystickGetDeviceInstanceID(i) == pad_id)
                SDL_JoystickDetachVirtual(i);
    }
    pad = NULL;
    pad_id = -1;
    held = 0;
    stick_x = stick_y = 0;
    pthread_mutex_unlock(&lock);
}
```

- [ ] **Step 7: Write `src/core/renpy/hooks.h` and `hooks.c`**

```c
/*
 * Dulcamara - Ren'Py's core: what the SDL calls renamed in Ren'Py's code (sdl_renames.h) do instead (hooks.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_HOOKS_H
#define DR_HOOKS_H

#include <stdint.h>

#define DR_MAX_W 1920 /* the largest picture */
#define DR_MAX_H 1080
#define DR_DISPLAY_W 3840 /* the display Ren'Py is told it has: room for any window it asks for */
#define DR_DISPLAY_H 2160

void hooks_reset(void); /* before a game: no timers, no window, GL's attributes at SDL's defaults */
/* Ren'Py's thread: waits until the clock reaches until_usec, firing its timers and giving control back each frame. */
void hooks_sleep_until(int64_t until_usec);

#endif
```

```c
/*
 * Dulcamara - Ren'Py's core: what Ren'Py's SDL calls do instead (sdl_renames.h renames them in Ren'Py's code; this
 * file calls the real SDL). Ren'Py draws into the core's OpenGL context (gl.c), keeps the core's time and waits
 * through it (handoff.c), mixes into the core's sound (audio.c) and sees only the core's controller (input.c). SDL
 * still runs underneath, with its dummy video and audio drivers, for the window object, events and threads.
 *
 * On a thread other than Ren'Py's (its helpers), SDL_Delay and the waits are SDL's own, in real time.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "hooks.h"

#include "audio.h"
#include "frames.h"
#include "gl.h"
#include "handoff.h"
#include "input.h"
#include "log.h"

#include <SDL.h>
#include <pthread.h>
#include <string.h>

/* ---------------------------------------------------------------- state */

#define TIMERS 64

typedef struct
{
    SDL_TimerID id; /* 0: free */
    int64_t due_usec;
    SDL_TimerCallback callback;
    void *param;
    Uint32 interval;
} Timer;

static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static Timer timers[TIMERS];
static SDL_TimerID next_timer = 1;
static int gl_attributes[64];
static SDL_Window *gl_window;
static int context_token; /* SDL_GLContext is a pointer: the core's one context is this address */
static int window_w = 1280, window_h = 720;

void hooks_reset(void)
{
    pthread_mutex_lock(&timer_lock);
    memset(timers, 0, sizeof(timers));
    next_timer = 1;
    pthread_mutex_unlock(&timer_lock);
    memset(gl_attributes, 0, sizeof(gl_attributes));
    gl_window = NULL;
    window_w = 1280;
    window_h = 720;
}

/* ---------------------------------------------------------------- time */

static void fire_timers(void)
{
    const int64_t now = handoff_now_usec();
    for (int i = 0; i < TIMERS; ++i)
    {
        pthread_mutex_lock(&timer_lock);
        Timer t = timers[i];
        const bool due = t.id && t.due_usec <= now;
        pthread_mutex_unlock(&timer_lock);
        if (!due)
            continue;
        const Uint32 again = t.callback(t.interval, t.param); /* pygame's: pushes its event */
        pthread_mutex_lock(&timer_lock);
        if (timers[i].id == t.id)
        {
            if (again)
            {
                timers[i].interval = again;
                timers[i].due_usec = now + (int64_t)again * 1000;
            }
            else
                timers[i].id = 0;
        }
        pthread_mutex_unlock(&timer_lock);
    }
}

void hooks_sleep_until(int64_t until_usec)
{
    gl_flush_pending();
    while (handoff_now_usec() < until_usec)
    {
        fire_timers();
        if (!handoff_yield())
            return;
    }
    fire_timers();
}

Uint32 dr_GetTicks(void)
{
    return (Uint32)(handoff_now_usec() / 1000);
}

Uint64 dr_GetTicks64(void)
{
    return (Uint64)(handoff_now_usec() / 1000);
}

void dr_Delay(Uint32 ms)
{
    if (!handoff_on_engine_thread())
    {
        SDL_Delay(ms);
        return;
    }
    hooks_sleep_until(handoff_now_usec() + (int64_t)ms * 1000);
}

SDL_TimerID dr_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *param)
{
    pthread_mutex_lock(&timer_lock);
    SDL_TimerID id = 0;
    for (int i = 0; i < TIMERS && !id; ++i)
        if (!timers[i].id)
        {
            id = next_timer++;
            timers[i] = (Timer){id, handoff_now_usec() + (int64_t)interval * 1000, callback, param, interval};
        }
    pthread_mutex_unlock(&timer_lock);
    if (!id)
        SDL_SetError("Dulcamara: no free timer");
    return id;
}

SDL_bool dr_RemoveTimer(SDL_TimerID id)
{
    SDL_bool removed = SDL_FALSE;
    pthread_mutex_lock(&timer_lock);
    for (int i = 0; i < TIMERS; ++i)
        if (id && timers[i].id == id)
        {
            timers[i].id = 0;
            removed = SDL_TRUE;
        }
    pthread_mutex_unlock(&timer_lock);
    return removed;
}

int dr_PollEvent(SDL_Event *event)
{
    if (handoff_on_engine_thread())
        fire_timers();
    return SDL_PollEvent(event);
}

int dr_WaitEventTimeout(SDL_Event *event, int timeout)
{
    if (!handoff_on_engine_thread())
        return SDL_WaitEventTimeout(event, timeout);
    const int64_t until = timeout < 0 ? INT64_MAX : handoff_now_usec() + (int64_t)timeout * 1000;
    for (;;)
    {
        fire_timers();
        if (SDL_PollEvent(event))
            return 1;
        if (handoff_now_usec() >= until)
            return 0;
        gl_flush_pending();
        if (!handoff_yield())
        {
            /* the game is being closed: Ren'Py is asked to quit (python.c); meanwhile its waits end at once */
            if (event)
            {
                memset(event, 0, sizeof(*event));
                event->type = SDL_QUIT;
            }
            return 1;
        }
    }
}

int dr_WaitEvent(SDL_Event *event)
{
    return dr_WaitEventTimeout(event, -1);
}

/* ---------------------------------------------------------------- the controller */

static void controllers_up(Uint32 flags)
{
    if (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER))
        input_attach();
}

int dr_Init(Uint32 flags)
{
    const int r = SDL_Init(flags);
    if (r == 0)
        controllers_up(flags);
    return r;
}

int dr_InitSubSystem(Uint32 flags)
{
    const int r = SDL_InitSubSystem(flags);
    if (r == 0)
        controllers_up(flags);
    return r;
}

SDL_bool dr_IsGameController(int index)
{
    return input_is_ours(index) ? SDL_IsGameController(index) : SDL_FALSE;
}

SDL_Joystick *dr_JoystickOpen(int index)
{
    if (!input_is_ours(index))
    {
        SDL_SetError("Dulcamara: Ren'Py uses its own controller");
        return NULL;
    }
    return SDL_JoystickOpen(index);
}

SDL_GameController *dr_GameControllerOpen(int index)
{
    if (!input_is_ours(index))
    {
        SDL_SetError("Dulcamara: Ren'Py uses its own controller");
        return NULL;
    }
    return SDL_GameControllerOpen(index);
}

/* ---------------------------------------------------------------- the window and the display */

SDL_Window *dr_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    (void)x, (void)y;
    if ((flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) || w <= 0 || h <= 0)
        w = DR_MAX_W, h = DR_MAX_H;
    frames_fit(w, h, DR_MAX_W, DR_MAX_H, &w, &h);
    flags &= ~(Uint32)(SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP |
                       SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_MAXIMIZED);
    SDL_Window *window = SDL_CreateWindow(title, 0, 0, w, h, flags | SDL_WINDOW_HIDDEN);
    if (window)
    {
        window_w = w;
        window_h = h;
        log_queue(1, "renpy: the picture is %dx%d", w, h);
    }
    return window;
}

void dr_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    (void)window;
    if (w)
        *w = window_w;
    if (h)
        *h = window_h;
}

void dr_SetWindowSize(SDL_Window *window, int w, int h)
{
    frames_fit(w, h, DR_MAX_W, DR_MAX_H, &w, &h);
    window_w = w;
    window_h = h;
    SDL_SetWindowSize(window, w, h);
    gl_resize(w, h);
}

int dr_SetWindowFullscreen(SDL_Window *window, Uint32 flags)
{
    if (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP))
        dr_SetWindowSize(window, DR_MAX_W, DR_MAX_H);
    return 0;
}

Uint32 dr_GetWindowFlags(SDL_Window *window)
{
    return (SDL_GetWindowFlags(window) | SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS |
            SDL_WINDOW_MOUSE_FOCUS) &
           ~(Uint32)SDL_WINDOW_HIDDEN;
}

int dr_GetNumVideoDisplays(void)
{
    return 1;
}

int dr_GetDisplayBounds(int index, SDL_Rect *rect)
{
    (void)index;
    *rect = (SDL_Rect){0, 0, DR_DISPLAY_W, DR_DISPLAY_H};
    return 0;
}

int dr_GetDisplayUsableBounds(int index, SDL_Rect *rect)
{
    return dr_GetDisplayBounds(index, rect);
}

int dr_GetNumDisplayModes(int index)
{
    (void)index;
    return 1;
}

int dr_GetDisplayMode(int index, int mode, SDL_DisplayMode *out)
{
    (void)index, (void)mode;
    *out = (SDL_DisplayMode){SDL_PIXELFORMAT_RGB888, DR_DISPLAY_W, DR_DISPLAY_H, 60, NULL};
    return 0;
}

int dr_GetDesktopDisplayMode(int index, SDL_DisplayMode *out)
{
    return dr_GetDisplayMode(index, 0, out);
}

int dr_GetCurrentDisplayMode(int index, SDL_DisplayMode *out)
{
    return dr_GetDisplayMode(index, 0, out);
}

/* ---------------------------------------------------------------- OpenGL */

int dr_GL_LoadLibrary(const char *path)
{
    (void)path;
    return 0;
}

void dr_GL_UnloadLibrary(void)
{
}

void *dr_GL_GetProcAddress(const char *name)
{
    return gl_proc(name);
}

SDL_bool dr_GL_ExtensionSupported(const char *extension)
{
    const unsigned char *(*get_string)(unsigned) = (const unsigned char *(*)(unsigned))gl_proc("glGetString");
    const char *all = get_string ? (const char *)get_string(0x1F03 /* GL_EXTENSIONS */) : NULL;
    if (!all || !extension || !*extension)
        return SDL_FALSE;
    const size_t n = strlen(extension);
    for (const char *at = strstr(all, extension); at; at = strstr(at + n, extension))
        if ((at == all || at[-1] == ' ') && (at[n] == ' ' || at[n] == '\0'))
            return SDL_TRUE;
    return SDL_FALSE;
}

int dr_GL_SetAttribute(SDL_GLattr attribute, int value)
{
    if ((int)attribute >= 0 && (int)attribute < 64)
        gl_attributes[attribute] = value;
    return 0;
}

int dr_GL_GetAttribute(SDL_GLattr attribute, int *value)
{
    switch (attribute)
    {
    case SDL_GL_RED_SIZE:
    case SDL_GL_GREEN_SIZE:
    case SDL_GL_BLUE_SIZE:
    case SDL_GL_ALPHA_SIZE:
        *value = 8;
        return 0;
    case SDL_GL_DOUBLEBUFFER:
        *value = 1;
        return 0;
    default:
        *value = (int)attribute >= 0 && (int)attribute < 64 ? gl_attributes[attribute] : 0;
        return 0;
    }
}

void dr_GL_ResetAttributes(void)
{
    memset(gl_attributes, 0, sizeof(gl_attributes));
}

SDL_GLContext dr_GL_CreateContext(SDL_Window *window)
{
    char error[256];
    if (!gl_create(window_w, window_h, gl_attributes[SDL_GL_DEPTH_SIZE], error, sizeof(error)))
    {
        log_queue(2, "renpy: no OpenGL: %s", error);
        SDL_SetError("%s", error);
        return NULL;
    }
    log_queue(1, "renpy: %s", gl_renderer());
    gl_window = window;
    return &context_token;
}

int dr_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
    (void)window;
    return gl_make_current(context != NULL) ? 0 : -1;
}

SDL_Window *dr_GL_GetCurrentWindow(void)
{
    return gl_window;
}

SDL_GLContext dr_GL_GetCurrentContext(void)
{
    return gl_window ? &context_token : NULL;
}

void dr_GL_GetDrawableSize(SDL_Window *window, int *w, int *h)
{
    dr_GetWindowSize(window, w, h);
}

int dr_GL_SetSwapInterval(int interval)
{
    (void)interval;
    return 0;
}

int dr_GL_GetSwapInterval(void)
{
    return 1;
}

void dr_GL_SwapWindow(SDL_Window *window)
{
    (void)window;
    gl_present();
    if (handoff_on_engine_thread())
        handoff_yield(); /* a frame shown: control goes back to the frontend */
}

void dr_GL_DeleteContext(SDL_GLContext context)
{
    (void)context;
    gl_destroy();
    gl_window = NULL;
}

/* ---------------------------------------------------------------- sound */

int dr_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    int freq = desired->freq;
    Uint8 channels = desired->channels;
    audio_open(&freq, &channels, desired->callback, desired->userdata);
    desired->freq = freq;
    desired->channels = channels;
    desired->format = AUDIO_F32SYS;
    desired->size = (Uint32)desired->samples * channels * (Uint32)sizeof(float);
    if (obtained)
        *obtained = *desired;
    if (freq != DR_AUDIO_RATE || channels != 2)
        log_queue(2, "renpy: the mixer is not 48 kHz stereo");
    return 0;
}

void dr_PauseAudio(int pause_on)
{
    audio_pause(pause_on != 0);
}

void dr_LockAudio(void)
{
    audio_lock();
}

void dr_UnlockAudio(void)
{
    audio_unlock();
}

void dr_CloseAudio(void)
{
    audio_close();
}
```

If Task 7 Step 2 added `SDL_GetPerformanceCounter`/`SDL_GetPerformanceFrequency`, add here:

```c
Uint64 dr_GetPerformanceCounter(void)
{
    return (Uint64)handoff_now_usec();
}

Uint64 dr_GetPerformanceFrequency(void)
{
    return 1000000;
}
```

- [ ] **Step 8: Add the `core` stage to `tools/build-renpy.sh`, the unit check first**

Append before the final `echo`:

```bash
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
    "$root/tests/core/renpy_unit.c" "$root"/src/core/renpy/{handoff,frames,audio,gl}.c "$gl_backend" \
    "${gl_libs[@]}" -lm -o "$check/renpy-unit" >"$log/core.log" 2>&1 || fail core
```

Run: `bash tools/build-renpy.sh host && build/renpy-host/core-check/renpy-unit`
Expected: `gl: Apple M5 Pro, OpenGL 2.1 Metal - 91.7` (or the machine's renderer), `N checks, 0 failed`, `PASS`. On a machine without GL: `gl: skipped (...)` and still `PASS`.

- [ ] **Step 9: Compile hooks.c and input.c against SDL, warnings as errors**

Run: `cc -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -pthread $(pkg-config --cflags sdl2) -c src/core/renpy/hooks.c -o /dev/null && cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -pthread $(pkg-config --cflags sdl2) -c src/core/renpy/input.c -o /dev/null && cc -std=c11 -Wall -Wextra -Werror -c src/core/renpy/log.c -o /dev/null`
Expected: no output. Then check every renamed call has its definition:
`for f in $(grep -oE 'dr_[A-Za-z0-9_]+' src/core/renpy/sdl_renames.h | sort -u); do grep -q "^[A-Za-z_].* $f(\|^[A-Za-z_].*\*$f(" src/core/renpy/hooks.c || echo "not defined: $f"; done`
Expected: no `not defined` line.

- [ ] **Step 10: Commit**

```bash
git add src/core/renpy/gl.h src/core/renpy/gl.c src/core/renpy/gl_backend.h src/core/renpy/gl_egl.c src/core/renpy/gl_cgl.c src/core/renpy/hooks.h src/core/renpy/hooks.c src/core/renpy/input.h src/core/renpy/input.c src/core/renpy/log.h src/core/renpy/log.c tests/core/renpy_unit.c tools/build-renpy.sh
git -c gpg.format=ssh -c user.signingkey=~/.ssh/id_ed25519.pub commit -S -m "Ren'Py core: its own OpenGL context and read-back, the SDL calls Ren'Py makes, the controller and the log"
```

### Task 9: Python, the libretro API and the headless frontend

**Files:**
- Create: `src/core/renpy/python.h`, `src/core/renpy/python.c`, `src/core/renpy/core.c`, `tests/core/renpy_headless.c`
- Modify: `tools/build-renpy.sh` (link `renpy-headless`)

**Interfaces:**
- Consumes: everything above; `renpy_extend_inittab()` (Task 7's `gen/inittab.c`); the engine folder.
- Produces: `bool python_start(const char *engine_dir, const char *game_dir, const char *saves_dir, char *error, size_t size)`, `void python_stop(void)`; the libretro API (`retro_*`); `build/renpy-host/core-check/renpy-headless`.

- [ ] **Step 1: Write `src/core/renpy/python.h`**

```c
/*
 * Dulcamara - Ren'Py's core: Ren'Py's own thread and the Python it runs in.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_PYTHON_H
#define DR_PYTHON_H

#include <stdbool.h>
#include <stddef.h>

/* Starts Ren'Py on its own thread for the game in game_dir (the folder that holds game/), with the engine folder
 * engine_dir (renpy8/) and its saves in saves_dir. False with a reason when it cannot start. */
bool python_start(const char *engine_dir, const char *game_dir, const char *saves_dir, char *error, size_t size);
/* Asks Ren'Py to quit at once and waits for its thread, ten seconds at most. */
void python_stop(void);

#endif
```

- [ ] **Step 2: Write `src/core/renpy/python.c`**

```c
/*
 * Dulcamara - Ren'Py's core: Ren'Py's own thread, with the Python it runs in. CPython is linked in (its standard
 * library is the engine folder's lib/python312.zip), Ren'Py's compiled modules are built in (inittab.c, written by
 * tools/build-renpy.sh), and _dulcamara gives the boot script (boot.py, the engine's dulcamara_boot.py) the core's
 * clock and log. Each game gets an interpreter of its own, started for it on this thread (so this thread is
 * Python's main one) and finalised when it ends. Python handles no signals: those are the frontend's.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "python.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "audio.h"
#include "frames.h"
#include "handoff.h"
#include "hooks.h"
#include "input.h"
#include "log.h"

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

void renpy_extend_inittab(void); /* inittab.c */

static pthread_t thread;
static bool thread_live;
static char engine[1024], game[1024], saves[1024];
static double wall_start; /* seconds since 1970 when the game started: time.time() is this plus the clock */

/* Whether another thread may raise an exception in Ren'Py's: only while its interpreter is up. */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_changed = PTHREAD_COND_INITIALIZER;
static bool interpreter_up, stopper_inside;
static unsigned long renpy_ident;

/* ---------------------------------------------------------------- _dulcamara */

static PyObject *py_time(PyObject *self, PyObject *unused)
{
    (void)self, (void)unused;
    return PyFloat_FromDouble(wall_start + (double)handoff_now_usec() / 1e6);
}

static PyObject *py_monotonic(PyObject *self, PyObject *unused)
{
    (void)self, (void)unused;
    return PyFloat_FromDouble((double)handoff_now_usec() / 1e6);
}

static PyObject *py_sleep(PyObject *self, PyObject *arg)
{
    (void)self;
    const double seconds = PyFloat_AsDouble(arg);
    if (seconds == -1.0 && PyErr_Occurred())
        return NULL;
    const int64_t until = handoff_now_usec() + (int64_t)(seconds > 0 ? seconds * 1e6 : 0);
    Py_BEGIN_ALLOW_THREADS
    hooks_sleep_until(until);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static PyObject *py_log(PyObject *self, PyObject *args)
{
    (void)self;
    int level;
    const char *text;
    if (!PyArg_ParseTuple(args, "is", &level, &text))
        return NULL;
    log_queue(level, "renpy: %s", text);
    Py_RETURN_NONE;
}

static PyMethodDef methods[] = {
    {"time", py_time, METH_NOARGS, "time.time() by the core's clock."},
    {"monotonic", py_monotonic, METH_NOARGS, "time.monotonic() by the core's clock."},
    {"sleep", py_sleep, METH_O, "time.sleep() on Ren'Py's thread, by the core's clock."},
    {"log", py_log, METH_VARARGS, "log(level, text): a line of Dulcamara's log."},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "_dulcamara", NULL, -1, methods,
                                    NULL, NULL, NULL, NULL};

static PyObject *init_dulcamara(void)
{
    return PyModule_Create(&module);
}

/* ---------------------------------------------------------------- Ren'Py's thread */

static bool ok(PyStatus status, const char *what)
{
    if (!PyStatus_Exception(status))
        return true;
    log_queue(2, "renpy: Python could not %s: %s", what, status.err_msg ? status.err_msg : "no reason given");
    return false;
}

static void search_path(PyConfig *config, const char *path)
{
    wchar_t *wide = Py_DecodeLocale(path, NULL);
    if (wide)
    {
        PyWideStringList_Append(&config->module_search_paths, wide);
        PyMem_RawFree(wide);
    }
}

static bool start_interpreter(const char *boot)
{
    PyPreConfig pre;
    PyPreConfig_InitIsolatedConfig(&pre);
    pre.utf8_mode = 1; /* file names are UTF-8, as the console's are */
    if (!ok(Py_PreInitialize(&pre), "start"))
        return false;
    PyImport_AppendInittab("_dulcamara", init_dulcamara);
    renpy_extend_inittab();

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.install_signal_handlers = 0;
    config.write_bytecode = 0;
    config.site_import = 0;
    config.use_hash_seed = 1; /* no random seed needed to start */
    config.hash_seed = 0;
    config.buffered_stdio = 0;
    config.configure_c_stdio = 0;
    config.parse_argv = 0;
    config.pathconfig_warnings = 0;
    config.module_search_paths_set = 1;
    char path[1100];
    snprintf(path, sizeof(path), "%s/lib/python312.zip", engine);
    search_path(&config, path);
    snprintf(path, sizeof(path), "%s/lib/site-packages", engine);
    search_path(&config, path);
    search_path(&config, engine);
    char *argv[] = {(char *)boot, engine, game, saves};
    const bool up = ok(PyConfig_SetBytesString(&config, &config.home, engine), "set its home") &&
                    ok(PyConfig_SetBytesString(&config, &config.program_name, "renpy"), "set its name") &&
                    ok(PyConfig_SetBytesArgv(&config, 4, argv), "set its arguments") &&
                    ok(Py_InitializeFromConfig(&config), "start");
    PyConfig_Clear(&config);
    return up;
}

static void *renpy_main(void *unused)
{
    (void)unused;
    handoff_engine_begin();
    char boot[1100];
    snprintf(boot, sizeof(boot), "%s/dulcamara_boot.py", engine);
    if (start_interpreter(boot))
    {
        pthread_mutex_lock(&state_lock);
        renpy_ident = PyThread_get_thread_ident();
        interpreter_up = true;
        pthread_mutex_unlock(&state_lock);

        PyObject *runpy = PyImport_ImportModule("runpy");
        PyObject *result = runpy ? PyObject_CallMethod(runpy, "run_path", "ss", boot, "__main__") : NULL;
        if (!result)
        {
            if (PyErr_ExceptionMatches(PyExc_SystemExit))
                PyErr_Clear();
            else
                PyErr_Print(); /* to sys.stderr: the log */
        }
        Py_XDECREF(result);
        Py_XDECREF(runpy);

        /* no other thread may be inside Python once it finalises */
        Py_BEGIN_ALLOW_THREADS
        pthread_mutex_lock(&state_lock);
        interpreter_up = false;
        while (stopper_inside)
            pthread_cond_wait(&state_changed, &state_lock);
        pthread_mutex_unlock(&state_lock);
        Py_END_ALLOW_THREADS
        /* The inittab is reset as Python finalises: the next game's start_interpreter extends it again. */
        if (Py_FinalizeEx() < 0)
            log_queue(2, "renpy: Python reported an error as it ended");
    }
    input_detach();
    SDL_Quit();
    audio_close();
    handoff_engine_end();
    return NULL;
}

static double wall_now(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return (double)t.tv_sec + (double)t.tv_usec / 1e6;
}

bool python_start(const char *engine_dir, const char *game_dir, const char *saves_dir, char *error, size_t size)
{
    if (thread_live)
    {
        if (!handoff_engine_ended())
        {
            snprintf(error, size, "Ren'Py is still closing the last game.");
            return false;
        }
        pthread_join(thread, NULL);
        thread_live = false;
    }
    snprintf(engine, sizeof(engine), "%s", engine_dir);
    snprintf(game, sizeof(game), "%s", game_dir);
    snprintf(saves, sizeof(saves), "%s", saves_dir);
    handoff_reset();
    frames_reset();
    hooks_reset();
    audio_close();
    wall_start = wall_now();
    SDL_SetHint("SDL_VIDEODRIVER", "dummy");
    SDL_SetHint("SDL_AUDIODRIVER", "dummy");
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 16u << 20); /* Python and Ren'Py recurse deeply */
    const int failed = pthread_create(&thread, &attributes, renpy_main, NULL);
    pthread_attr_destroy(&attributes);
    if (failed)
    {
        snprintf(error, size, "Ren'Py's thread could not start (%d).", failed);
        return false;
    }
    thread_live = true;
    return true;
}

/* Raises renpy.game.QuitException in Ren'Py's thread (SystemExit before Ren'Py is imported): Ren'Py then quits
 * the way its own Quit does, closing its sound and display. */
static void ask_renpy_to_quit(void)
{
    pthread_mutex_lock(&state_lock);
    const bool can = interpreter_up;
    if (can)
        stopper_inside = true;
    pthread_mutex_unlock(&state_lock);
    if (!can)
        return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *name = PyUnicode_FromString("renpy.game");
    PyObject *renpy_game = name ? PyImport_GetModule(name) : NULL; /* only if Ren'Py has imported it */
    Py_XDECREF(name);
    PyObject *quit = renpy_game ? PyObject_GetAttrString(renpy_game, "QuitException") : NULL;
    if (!quit)
    {
        PyErr_Clear();
        quit = Py_NewRef(PyExc_SystemExit);
    }
    PyThreadState_SetAsyncExc(renpy_ident, quit);
    Py_DECREF(quit);
    Py_XDECREF(renpy_game);
    PyGILState_Release(gil);
    pthread_mutex_lock(&state_lock);
    stopper_inside = false;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_lock);
}

void python_stop(void)
{
    if (!thread_live)
        return;
    handoff_stop();
    ask_renpy_to_quit();
    for (int i = 0; i < 1000 && !handoff_engine_ended(); ++i)
        usleep(10000);
    if (handoff_engine_ended())
    {
        pthread_join(thread, NULL);
        thread_live = false;
    }
    else
        log_queue(2, "renpy: Ren'Py did not stop within ten seconds");
}
```

- [ ] **Step 3: Write `src/core/renpy/core.c`**

```c
/*
 * Dulcamara - Ren'Py 8 as a libretro core: the libretro API (renamed renpy_retro_* in the PS5 build, as
 * tools/build-renpy.sh does). Ren'Py runs on its own thread (python.c); each retro_run hands it one frame's time and
 * takes back the picture it drew (handoff.c, frames.c), and the frontend's sound callback plays its mixer (audio.c).
 * The engine is <core assets directory>/renpy8, and the saves <save directory>/saves/renpy.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "libretro.h"

#include "audio.h"
#include "frames.h"
#include "handoff.h"
#include "hooks.h"
#include "input.h"
#include "log.h"
#include "python.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define FRAME_USEC 16667
#define SOUND_FRAMES 800 /* a frame's worth at 48 kHz */

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;
static retro_usec_t frame_usec = FRAME_USEC;
static bool loaded, shutdown_sent, sound_by_callback, bitmasks;
static int picture_w, picture_h;

static void emit(int level, const char *line)
{
    if (log_cb)
        log_cb(level >= 2 ? RETRO_LOG_WARN : RETRO_LOG_INFO, "%s\n", line);
    else
        fprintf(stderr, "%s\n", line);
}

static void frame_time(retro_usec_t usec)
{
    frame_usec = usec > 0 ? usec : FRAME_USEC;
}

static void sound(void)
{
    int16_t samples[SOUND_FRAMES * 2];
    audio_render(samples, SOUND_FRAMES);
    if (audio_batch_cb)
        audio_batch_cb(samples, SOUND_FRAMES);
}

static void sound_state(bool enabled)
{
    (void)enabled;
}

static bool is_folder(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* path and the folders above it, as mkdir -p */
static bool make_folders(const char *path)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 1; *s; ++s)
        if (*s == '/')
        {
            *s = '\0';
            if (mkdir(p, 0777) != 0 && errno != EEXIST)
                return false;
            *s = '/';
        }
    return mkdir(p, 0777) == 0 || errno == EEXIST;
}

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
    video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
    (void)cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
    input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
    input_state_cb = cb;
}

void retro_init(void)
{
    struct retro_log_callback l;
    log_cb = environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &l) ? l.log : NULL;
}

void retro_deinit(void)
{
    log_cb = NULL;
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "Ren'Py";
    info->library_version = "8.5.3";
    info->valid_extensions = "rpa|rpyc|rpy";
    info->need_fullpath = true;
    info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info *av)
{
    memset(av, 0, sizeof(*av));
    av->geometry.base_width = picture_w ? (unsigned)picture_w : 1280;
    av->geometry.base_height = picture_h ? (unsigned)picture_h : 720;
    av->geometry.max_width = DR_MAX_W;
    av->geometry.max_height = DR_MAX_H;
    av->geometry.aspect_ratio = (float)av->geometry.base_width / (float)av->geometry.base_height;
    av->timing.fps = 60.0;
    av->timing.sample_rate = DR_AUDIO_RATE;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port, (void)device;
}

void retro_reset(void)
{
}

bool retro_load_game(const struct retro_game_info *info)
{
    if (!info || !info->path || !environ_cb)
        return false;
    const char *assets = NULL, *save = NULL;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY, &assets) || !assets)
    {
        emit(2, "renpy: the frontend gave no core assets folder, where the engine (renpy8/) is");
        return false;
    }
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save) || !save)
        environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &save);
    char engine[1024], marker[1100], game_folder[1100], saves[1024];
    snprintf(engine, sizeof(engine), "%s/renpy8", assets);
    snprintf(marker, sizeof(marker), "%s/renpy/__init__.py", engine);
    snprintf(game_folder, sizeof(game_folder), "%s/game", info->path);
    snprintf(saves, sizeof(saves), "%s/saves/renpy", save ? save : info->path);
    struct stat st;
    if (stat(marker, &st) != 0)
    {
        log_queue(2, "renpy: no engine in %s", engine);
        log_flush(emit);
        return false;
    }
    if (!is_folder(game_folder))
    {
        log_queue(2, "renpy: %s has no game folder", info->path);
        log_flush(emit);
        return false;
    }
    if (!make_folders(saves))
        log_queue(2, "renpy: cannot make %s", saves);

    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
        return false;
    struct retro_frame_time_callback timing = {frame_time, FRAME_USEC};
    environ_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, &timing);
    struct retro_audio_callback audio = {sound, sound_state};
    sound_by_callback = environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &audio);
    bitmasks = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
    frame_usec = FRAME_USEC;
    picture_w = picture_h = 0;
    shutdown_sent = false;

    char error[256];
    if (!python_start(engine, info->path, saves, error, sizeof(error)))
    {
        emit(2, error);
        return false;
    }
    log_queue(1, "renpy: %s, engine %s, saves %s", info->path, engine, saves);
    loaded = true;
    return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t count)
{
    (void)type, (void)info, (void)count;
    return false;
}

void retro_unload_game(void)
{
    if (!loaded)
        return;
    python_stop();
    log_flush(emit);
    loaded = false;
}

static uint16_t pad_buttons(void)
{
    if (!input_state_cb)
        return 0;
    if (bitmasks)
        return (uint16_t)input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
    uint16_t bits = 0;
    for (unsigned id = 0; id < 16; ++id)
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id))
            bits |= (uint16_t)(1u << id);
    return bits;
}

void retro_run(void)
{
    if (input_poll_cb)
        input_poll_cb();
    const int16_t lx = input_state_cb ? input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                                       RETRO_DEVICE_ID_ANALOG_X)
                                      : 0;
    const int16_t ly = input_state_cb ? input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                                       RETRO_DEVICE_ID_ANALOG_Y)
                                      : 0;
    input_set(pad_buttons(), lx, ly);
    handoff_frame(frame_usec, frame_usec * 3 / 4);
    log_flush(emit);

    int w = 0, h = 0;
    bool fresh = false;
    const uint32_t *picture = frames_take(&w, &h, &fresh);
    if (picture && (w != picture_w || h != picture_h))
    {
        picture_w = w;
        picture_h = h;
        struct retro_game_geometry geometry = {(unsigned)w, (unsigned)h, DR_MAX_W, DR_MAX_H, (float)w / (float)h};
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
    }
    if (video_cb)
        video_cb(picture && fresh ? picture : NULL, w ? (unsigned)w : 1280, h ? (unsigned)h : 720,
                 (size_t)(w ? w : 1280) * sizeof(uint32_t));
    if (!sound_by_callback)
        sound();
    if (handoff_engine_ended() && !shutdown_sent)
    {
        shutdown_sent = true;
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }
}

size_t retro_serialize_size(void)
{
    return 0; /* Ren'Py's own saves */
}

bool retro_serialize(void *data, size_t size)
{
    (void)data, (void)size;
    return false;
}

bool retro_unserialize(const void *data, size_t size)
{
    (void)data, (void)size;
    return false;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index, (void)enabled, (void)code;
}

unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
    (void)id;
    return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    (void)id;
    return 0;
}
```

- [ ] **Step 4: Write `tests/core/renpy_headless.c`**

```c
/*
 * Dulcamara - Ren'Py 8's libretro core, headless: a frontend with no screen and no sound card that loads a Ren'Py
 * game, plays it with the controller as a player would, and reports what came out. tests/test_renpy_core.py runs it
 * on tests/renpy-game. The game says where it is in its log ("DULCAMARA-TEST: ..."), and the frontend acts on that.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   renpy-headless <assets folder> play|load|start|error <game folder> <data folder> [--then <game folder>]
 *
 *   play   Cross through the dialogue, Down and Cross to choose Right; the game saves and quits.
 *   load   the game loads that save (DULCAMARA_TEST_LOAD=1); Cross, and it quits.
 *   start  the game starts and shows a picture; it is then closed while it waits.
 *   error  the game has a script error: Ren'Py's error is logged and shown; it is then closed.
 *   --then a second game in the same process afterwards, as "start".
 */
#include "libretro.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 7200 /* two minutes of the game's time for each step */

static const char *assets_dir, *data_dir;
static struct retro_audio_callback audio_cb;
static struct retro_frame_time_callback frame_time_cb;
static bool audio_cb_set, frame_time_set, shutdown_asked, xrgb;
static unsigned picture_w, picture_h, geometry_w, geometry_h;
static uint64_t picture_hash, with_picture, dupes, distinct, sound_frames, sound_loud, refreshes;
static uint16_t buttons;
static char log_text[1 << 18];
static size_t log_used;
static int ran;

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    char line[4096];
    va_list va;
    va_start(va, fmt);
    vsnprintf(line, sizeof(line), fmt, va);
    va_end(va);
    const size_t n = strlen(line);
    if (log_used + n + 1 < sizeof(log_text))
    {
        memcpy(log_text + log_used, line, n + 1);
        log_used += n;
    }
    if (getenv("RENPY_HEADLESS_VERBOSE") || strstr(line, "DULCAMARA-TEST: ") || strstr(line, "Ren'Py reported") ||
        strstr(line, ".rpy"))
        printf("log: %s", line);
}

static bool logged(const char *what)
{
    return strstr(log_text, what) != NULL;
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        xrgb = *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
        return xrgb;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = data_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        *(const char **)data = assets_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
        audio_cb = *(const struct retro_audio_callback *)data;
        audio_cb_set = audio_cb.callback != NULL;
        return true;
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        frame_time_cb = *(const struct retro_frame_time_callback *)data;
        frame_time_set = frame_time_cb.callback != NULL;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        geometry_w = ((const struct retro_game_geometry *)data)->base_width;
        geometry_h = ((const struct retro_game_geometry *)data)->base_height;
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        shutdown_asked = true;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        if (data)
            *(bool *)data = true;
        return true;
    default:
        return false;
    }
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    ++refreshes;
    if (!data)
    {
        ++dupes;
        return;
    }
    ++with_picture;
    uint64_t hash = 1469598103934665603ull;
    for (unsigned y = 0; y < height; ++y)
    {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)data + y * pitch);
        for (unsigned x = 0; x < width; ++x)
            hash = (hash ^ (row[x] & 0xffffffu)) * 1099511628211ull;
    }
    if (hash != picture_hash)
        ++distinct;
    picture_hash = hash;
    picture_w = width;
    picture_h = height;
}

static void audio_sample(int16_t left, int16_t right)
{
    (void)left, (void)right;
    ++sound_frames;
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    for (size_t i = 0; i < frames * 2; ++i)
        if (data[i] > 256 || data[i] < -256)
        {
            ++sound_loud;
            break;
        }
    sound_frames += frames;
    return frames;
}

static void input_poll(void)
{
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)buttons;
    return id < 16 ? (buttons >> id) & 1 : 0;
}

static void frame(void)
{
    if (frame_time_set)
        frame_time_cb.callback(frame_time_cb.reference);
    retro_run();
    if (audio_cb_set)
        audio_cb.callback();
    ++ran;
}

static bool until_logged(const char *what)
{
    for (int i = 0; i < MAX_FRAMES && !logged(what) && !shutdown_asked; ++i)
        frame();
    if (!logged(what))
        printf("FAIL: never logged \"%s\"\n", what);
    return logged(what);
}

static bool until_picture(void)
{
    for (int i = 0; i < MAX_FRAMES && !with_picture && !shutdown_asked; ++i)
        frame();
    if (!with_picture)
        printf("FAIL: no picture\n");
    return with_picture > 0;
}

static bool until_shutdown(void)
{
    for (int i = 0; i < MAX_FRAMES && !shutdown_asked; ++i)
        frame();
    if (shutdown_asked)
        printf("ended by the game\n");
    else
        printf("FAIL: the game did not end\n");
    return shutdown_asked;
}

static void frames(int n)
{
    for (int i = 0; i < n && !shutdown_asked; ++i)
        frame();
}

static void press(unsigned id)
{
    buttons = (uint16_t)(1u << id);
    frames(6);
    buttons = 0;
}

static bool load(const char *game)
{
    with_picture = dupes = distinct = refreshes = sound_frames = sound_loud = 0;
    picture_hash = 0;
    shutdown_asked = false;
    log_used = 0;
    log_text[0] = '\0';
    ran = 0;
    struct retro_game_info info = {game, NULL, 0, NULL};
    if (!retro_load_game(&info))
    {
        printf("FAIL: the core did not load %s\n", game);
        return false;
    }
    if (audio_cb_set && audio_cb.set_state)
        audio_cb.set_state(true);
    return true;
}

static void report(void)
{
    printf("frames: %d run, %llu with a new picture, %llu the same picture again, %llu different pictures\n", ran,
           (unsigned long long)with_picture, (unsigned long long)dupes, (unsigned long long)distinct);
    printf("picture: %ux%u, %s\n", picture_w, picture_h, xrgb ? "XRGB8888" : "another pixel format");
    printf("sound: %s (%llu frames, %llu batches with sound in them)\n", sound_loud > 10 ? "playing" : "silent",
           (unsigned long long)sound_frames, (unsigned long long)sound_loud);
}

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "usage: %s <assets folder> play|load|start|error <game folder> <data folder> [--then <game>]\n",
                argv[0]);
        return 2;
    }
    assets_dir = argv[1];
    const char *mode = argv[2], *game = argv[3];
    data_dir = argv[4];
    const char *then = argc > 6 && !strcmp(argv[5], "--then") ? argv[6] : NULL;

    struct retro_system_info info;
    retro_get_system_info(&info);
    printf("core: %s %s\n", info.library_name, info.library_version);
    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    bool ok = load(game);
    bool changed = true, ended = true;
    if (ok && !strcmp(mode, "play"))
    {
        ok = until_logged("DULCAMARA-TEST: started") && until_picture();
        frames(60);
        const uint64_t before = picture_hash;
        press(RETRO_DEVICE_ID_JOYPAD_B); /* Cross */
        ok = ok && until_logged("DULCAMARA-TEST: advanced");
        frames(60);
        changed = picture_hash != before;
        printf("input: the picture %s after Cross\n", changed ? "changed" : "did not change");
        press(RETRO_DEVICE_ID_JOYPAD_DOWN);
        frames(30);
        press(RETRO_DEVICE_ID_JOYPAD_B);
        ok = ok && until_logged("DULCAMARA-TEST: choice=") && until_logged("DULCAMARA-TEST: saved");
        frames(30);
        press(RETRO_DEVICE_ID_JOYPAD_B);
        ended = until_shutdown();
    }
    else if (ok && !strcmp(mode, "load"))
    {
        ok = until_logged("DULCAMARA-TEST: loaded choice=") && until_picture();
        frames(60);
        press(RETRO_DEVICE_ID_JOYPAD_B);
        ended = until_shutdown();
    }
    else if (ok && (!strcmp(mode, "start") || !strcmp(mode, "error")))
    {
        ok = (!strcmp(mode, "error") ? until_logged("Ren'Py reported an error") : until_logged("DULCAMARA-TEST: started"))
             && until_picture();
        frames(60);
    }
    else if (ok)
    {
        printf("FAIL: no mode %s\n", mode);
        ok = false;
    }
    if (geometry_w)
        printf("geometry: %ux%u\n", geometry_w, geometry_h);
    report();
    retro_unload_game();
    printf("stopped\n");

    const bool played = !strcmp(mode, "play") || !strcmp(mode, "load");
    bool verdict = ok && changed && ended;
    if (refreshes != (uint64_t)ran)
        verdict = false, printf("FAIL: %llu frames called video_refresh, of %d\n", (unsigned long long)refreshes, ran);
    if (!xrgb)
        verdict = false, printf("FAIL: the core did not ask for XRGB8888\n");
    if (distinct < (played ? 2u : 1u))
        verdict = false, printf("FAIL: the picture hardly changed\n");
    if (!strcmp(mode, "play") && sound_loud <= 10)
        verdict = false, printf("FAIL: no sound\n");

    if (then)
    {
        const bool second = load(then) && until_logged("DULCAMARA-TEST: started") && until_picture();
        frames(60);
        const unsigned w = picture_w, h = picture_h;
        retro_unload_game(); /* the core's last log lines are printed here: the report comes after them */
        printf("second game: %s, picture %ux%u, stopped\n", second ? "started" : "FAILED", w, h);
        verdict = verdict && second;
    }
    retro_deinit();
    printf(verdict ? "PASS\n" : "FAIL\n");
    return verdict ? 0 : 1;
}
```

- [ ] **Step 5: Link the core into `renpy-headless` in `tools/build-renpy.sh`**

Append after the unit check:

```bash
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
# Ren'Py's modules, then what they use: FFmpeg, Assimp, SDL2_image and SDL2, the text libraries, Python.
"$cxx" -o "$check/renpy-headless" "$check/renpy_headless.o" "${objects[@]}" "$check/gl_backend.o" \
    "$check/inittab.o" "$out/librenpy-modules.a" \
    -L"$prefix/lib" -lavformat -lavcodec -lswscale -lswresample -lavutil -lassimp \
    $(pkg-config --libs SDL2_image sdl2 freetype2 harfbuzz fribidi libpng libjpeg libwebp zlib liblzma openssl) \
    -lbz2 $("$out/python/bin/python3.12-config" --embed --ldflags) "${gl_libs[@]}" -pthread -lm \
    >>"$log/core.log" 2>&1 || fail core
```

Change the final line to `echo "==> [renpy] $check/renpy-headless"` (the test prints it).

- [ ] **Step 6: Build and run one game by hand**

Run:
```bash
bash tools/build-renpy.sh host
rm -rf build/renpy-try
python3 -c "
import sys; sys.path.insert(0, 'tests'); from pathlib import Path; import test_renpy_core as t
t.make_game(Path('build/renpy-try/Test Game'))"
RENPY_HEADLESS_VERBOSE=1 build/renpy-host/core-check/renpy-headless build/renpy-host/assets play "build/renpy-try/Test Game" build/renpy-try/data
```
Expected: Ren'Py's log lines, the five `DULCAMARA-TEST` steps, `ended by the game`, `stopped`, `PASS`. When it fails, the verbose log says where; most likely first problems and their place:
- `ImportError: No module named ...` for a C module: add it to `tools/renpy/Setup.local`; for a Python package: to `wheels.txt`.
- `no OpenGL` or Ren'Py falling back to its software renderer: `dr_GL_CreateContext` and `gl.c`'s loader.
- the picture never changes: `dr_GL_SwapWindow` / `gl_flush_pending` (a frame read but never published).
- Cross does nothing: Ren'Py did not open the controller (`input_attach`, `dr_IsGameController`); `RENPY_HEADLESS_VERBOSE` shows Ren'Py's controller lines.
- a hang at exit: `python_stop`'s async exception, or a hook that still blocks while stopping.
Debug with superpowers:systematic-debugging; fix in the file the cause is in; rerun this step until it passes. Then `rm -rf build/renpy-try`.

- [ ] **Step 7: Run the whole check**

Run: `python3 -m unittest tests.test_renpy_core -v`
Expected: 7 tests, OK. Paste the output into the milestone report.

- [ ] **Step 8: Run the existing checks, to see nothing else changed**

Run: `python3 -m unittest tests.test_library tests.test_core -v`
Expected: OK, as before this branch.

- [ ] **Step 9: Commit**

```bash
git add src/core/renpy/python.h src/core/renpy/python.c src/core/renpy/core.c tests/core/renpy_headless.c tools/build-renpy.sh
git -c gpg.format=ssh -c user.signingkey=~/.ssh/id_ed25519.pub commit -S -m "Ren'Py core: Ren'Py's thread and Python, the libretro API, and a headless frontend that plays the test game"
```

### Task 10: The check in CI, and milestone 1's report

**Files:**
- Modify: `.github/workflows/build.yml` (a `renpy` job)

- [ ] **Step 1: Add the job**

Add under `jobs:`, after `ps5:` and before `release:`:

```yaml
  # Ren'Py 8's core built for the build machine and run headless on tests/renpy-game (tests/test_renpy_core.py):
  # the same Ren'Py, CPython, FFmpeg and Assimp as the PS5's, drawing through Mesa's EGL without a screen.
  renpy:
    name: Ren'Py core (host check)
    runs-on: ubuntu-24.04
    timeout-minutes: 60
    steps:
      - name: Check out source
        uses: actions/checkout@v4

      - name: Fetch Ren'Py and libretro.h
        run: git submodule update --init --depth=1 third_party/renpy third_party/easyrpg-player

      - name: Install toolchain
        run: |
          sudo apt-get update
          sudo apt-get install --yes clang-18 cmake ninja-build pkg-config python3 \
            libsdl2-dev libsdl2-image-dev libfreetype-dev libharfbuzz-dev libfribidi-dev libpng-dev \
            libjpeg-dev libwebp-dev zlib1g-dev libbz2-dev liblzma-dev libssl-dev \
            libegl-dev libegl-mesa0 libgl1-mesa-dri

      - name: Note Ren'Py's revision
        run: git -C third_party/renpy rev-parse HEAD > renpy-revision.txt

      - name: Restore the downloads and the built stages
        uses: actions/cache@v4
        with:
          path: |
            .deps/renpy
            build/renpy-host
          key: renpy-host-${{ hashFiles('renpy-revision.txt', 'tools/build-renpy.sh', 'tools/renpy/*', 'tools/patches/ffmpeg-*', 'src/core/renpy/sdl_renames.h', 'src/core/renpy/boot.py') }}
          restore-keys: renpy-host-

      - name: Ren'Py core check
        env:
          CC: clang-18
          CXX: clang++-18
          LIBGL_ALWAYS_SOFTWARE: "1"
        run: python3 -m unittest tests.test_renpy_core -v
```

- [ ] **Step 2: Commit and push**

```bash
git add .github/workflows/build.yml
git -c gpg.format=ssh -c user.signingkey=~/.ssh/id_ed25519.pub commit -S -m "CI: Ren'Py's core checked on the build machine"
git push nightshade feature/renpy-core
```

- [ ] **Step 3: Watch the run and fix what Linux shows**

Run: `gh run list --repo Oliv3rmoon/Dulcamara --branch feature/renpy-core --limit 3`, then `gh run watch <id> --repo Oliv3rmoon/Dulcamara` in the background.
Expected: the `renpy` job passes (the `ps5` job is unchanged). A Linux-only failure (EGL, a library name for pkg-config) is fixed in `tools/build-renpy.sh` or `gl_egl.c`, committed and pushed; the run's link goes in the report.

- [ ] **Step 4: Report milestone 1 to Andrew**

With: the `python3 -m unittest tests.test_renpy_core -v` output from this Mac, the CI run's link, the commits, and what milestone 2 will start with. Wait for his go-ahead before milestone 2.

---

## Milestones 2-4 (each gets its own plan before it starts)

### Milestone 2: PS5 cross-build

Stop rule (from the spec): if CPython does not build or link for the PS5 SDK, stop and report exactly what failed.

- **M2.1 CPython for the PS5.** Configure with `--host=x86_64-unknown-freebsd --build=$(config.guess)`, `--with-build-python=build/renpy-host/python/bin/python3.12`, the PS5 compiler of `tooling/prospero-clang18`, PacBrew's OpenSSL, zlib, bzip2 and xz, a `config.site` of cross answers (starting from renpy-build's: `ac_cv_file__dev_ptmx=no`, `ac_cv_file__dev_ptc=no`; then whatever configure and the link ask), and patches for the modules that need them (`posixmodule`, `signal`, `socket`, `pwd` are the candidates the spec names). Done when `libpython3.12.a` builds and a small embedding program links against the PS5 SDK with `tools/find-missing-symbols.sh` reporting nothing missing from the app's libc.
- **M2.2 The other libraries and Ren'Py's modules for the PS5.** FFmpeg (`--target-os=freebsd`, x86 asm with nasm when the build machine has it), Assimp, Ren'Py's modules against PacBrew's SDL2, SDL2_image, FreeType, HarfBuzz, fribidi, libpng, libjpeg, libwebp.
- **M2.3 OpenGL and the isolated archive.** ps5-opengl SDK 1.0.2 (pinned as Nightshade's `tools/deps.json` pins it, with its SHA-256), `gl_egl.c`'s pbuffer path and a compatibility context; first a probe on the console that EGL can be opened with a pbuffer while Dulcamara's Vulkan screen runs. Then `ld -r` of everything, every symbol but the libretro API made local, the API renamed `renpy_retro_*` (as `tools/build-easyrpg.sh` does), the undefined-symbol list checked: `build/renpy-ps5/librenpy.a` and `build/renpy-ps5/assets/renpy8`.
- **M2.4 Into the app, enough to run the test game on a console.** `CORE_RENPY` in `cores.c` (built where `NP_HAVE_RENPY`), `host.c` answering `RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY` with the app's folder, the Makefile linking the archive and copying `renpy8/` into `dist/PPSA99010/`, the system modules the ps5-opengl libraries need, a first library rule (a folder with `game/*.rpy`), CI building it. The console run of `tests/renpy-game` by Andrew or a tester, with `/data/dulcamara/logs` sent back, and the frame copy's cost at 1920x1080 logged.

### Milestone 3: integration and release

Library detection in `library.c` with `test_library.py` cases (version 8, version 7, no `renpy/` folder, not a game; titles from `<name>.exe`/`.sh`; covers from `game/gui/main_menu.png`, `window_icon.png`); Ren'Py's default button map in `cores.c` (Cross `PAD_B`, Circle `PAD_A`, Square `PAD_Y`, Triangle `PAD_X`, L1 `PAD_L`, R1 `PAD_R`, Start `PAD_START`); the touchpad as the mouse; fast-forward holding Ren'Py's skip; Ren'Py's common scripts compiled into the engine at build time (the app folder is read-only); a Settings page for the core; `THIRD_PARTY_NOTICES.md` and `licenses/` (Ren'Py MIT, CPython PSF, FFmpeg LGPL-2.1, Assimp BSD-3, fribidi LGPL-2.1, SDL2_image zlib, the pure-Python packages, certifi MPL-2.0) with CI's ZIP check extended (the licences present, no `.rpa`/`.rpyc`/`.rpy` outside the engine); the release notes. Tagging and publishing need Andrew's OK.

### Milestone 4: Ren'Py 7

A second core, `renpy7_retro_*`: Ren'Py 7.8.7 (`7.8.7.25031702`, commit `b38cc6a5018edc7da3f201c33f46de666a64f3cd`) with its pygame_sdl2, and CPython 2.7.18 (`Python-2.7.18.tar.xz`, SHA-256 `b62c0e7937551d0cc02b8fd5cb0f544f9405bafc9a54d3808ed4594812edef43`), built the same way, `python.c`'s start written for Python 2's embedding API. Ren'Py 7 games then start from the library.
