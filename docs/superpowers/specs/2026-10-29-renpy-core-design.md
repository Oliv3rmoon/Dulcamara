# Ren'Py core for Dulcamara: design

Date: 2026-10-29. Status: approved in conversation, waiting on review of this written spec.

## Goal

Dulcamara plays Ren'Py visual novels on the PS5 beside its RPG Maker 2000/2003 games. Most of the games
JoiPlay players bring are Ren'Py games, so this is what makes Dulcamara an answer to JoiPlay rather than an
RPG Maker player. Ren'Py runs as a second libretro core in the same app, so its games get what EasyRPG's
get: the library, the controller, fast-forward, the CRT and LCD shaders, and the updates.

Success: a Ren'Py 8 game the user copies into `/data/dulcamara/games/` appears in the library with a
cover. It starts, plays its dialogue, menus, images, music and voice, saves and loads, and runs at a steady
60 FPS on the console. All of it works from the controller.

## What is decided

- **A libretro core**, not a separate runner: Andrew chose it so the shaders and fast-forward apply.
- **Ren'Py 8 first** (Python 3). Ren'Py 7 (Python 2.7) is milestone 4, as its own core.
- **The engine ships in the app** and the game supplies its `game/` folder, as JoiPlay does. The game's own
  `renpy/` folder is read only for its version.
- **No game files are bundled or downloaded.** The checks use a small game written in this repository.

## Out of scope

- Games that ship their own compiled Python extension modules (`.so` / `.pyd` in `game/`).
- Ren'Py 6 and older.
- Save states (`retro_serialize`). Ren'Py's own saves cover this.
- Zero-copy frames into the Vulkan screen. The frame is copied back (see Drawing) until that is measured
  to matter.

## 1. The core

The core is `renpy_retro_*`, a static archive built the way `tools/build-easyrpg.sh` builds EasyRPG's.
The core and every library it uses are linked into one relocatable object, every symbol but the libretro
API is made local, and the API is renamed. Because of this, the Ren'Py 7 core can carry its own Python
2.7 beside this core's Python 3 without their symbols colliding.

`tools/build-renpy.sh [ps5|host]` builds `build/renpy-<target>/librenpy.a`. The pieces:

| Piece | Source | Notes |
|---|---|---|
| CPython 3 | pinned release tarball, SHA-256 | Static build for the PS5 SDK (clang, FreeBSD-like libc). The extension modules Ren'Py imports are built in (`Setup.local`). The standard library goes into a zip next to the engine. |
| Ren'Py engine | pinned Ren'Py 8 release tag | `renpy/` Python package plus `renpy/common`. The Cython modules are turned into C on the build host and compiled into the core, then registered with `PyImport_AppendInittab`. |
| pygame_sdl2 | pinned tag matching that Ren'Py | Cython to C on the host, compiled in the same way. |
| SDL2 | PacBrew's | With the offscreen video driver (EGL) and the core's own audio capture (see 3). |
| ffmpeg | pinned release, LGPL options only | Only the decoders Ren'Py games use: Vorbis, Opus, MP3, FLAC, WAV/PCM, VP8/VP9, Theora, AV1 (dav1d if PacBrew has it, else off), Matroska/WebM/Ogg/MP4 demuxers. No encoders, no network. |
| freetype, harfbuzz, fribidi, libpng, libjpeg-turbo, libwebp, zlib | PacBrew where it has them, else pinned tarballs | fribidi and libwebp are likely to be missing from PacBrew. |

The engine files (`renpy/`, the standard library zip and Ren'Py's `common/`) ship in the app package
as `PPSA99010/renpy8/`, beside `eboot.bin`, so an update replaces them together with the core. The system
directory the frontend gives cores is the data folder (`/data/dulcamara`), not the app folder. So the
frontend's `host_load` also tells the core the app's own folder, through
`RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY`, and the core reads the engine from `<that>/renpy8/`.

Licences, for `THIRD_PARTY_NOTICES.md`: Ren'Py is MIT, pygame_sdl2 is zlib/LGPL, CPython is PSF, ffmpeg is
LGPL-2.1 (no GPL options), fribidi is LGPL, and the rest are already listed.

## 2. Drawing

The frontend takes software frames only (`video_refresh` with XRGB8888). The core gets OpenGL from the
same Belladonna Mesa build that Nightshade's EGL display uses (`display_egl.cpp` in Nightshade). SDL2's
offscreen video driver creates the EGL pbuffer context, and Ren'Py's gl2 renderer draws into it as it
would into a window.

At `SDL_GL_SwapWindow` the core reads the frame back with `glReadPixels` into a pixel-pack buffer, mapped
one frame later so the copy does not stall the GPU. It then passes that buffer to `video_refresh`. When
Ren'Py presented nothing new since the last frame, the core passes `NULL` (libretro's dupe), so a still
screen of text costs no copy.

The picture's size is the game's `config.screen_width` × `config.screen_height` (1280×720 or 1920×1080 for
nearly all games). The core reports it with `SET_GEOMETRY` once the game sets it, and the maximum geometry
is 1920×1080.

## 3. Running inside libretro

Ren'Py owns its main loop, so it runs on a thread of its own and the core hands control back and forth.

- **Frames:** `retro_run` releases the Ren'Py thread and waits. Ren'Py runs until it presents a frame or
  waits for an event, and then the core takes control back. One `retro_run` is at most one Ren'Py frame.
- **Clock:** Ren'Py's time comes from the core, which advances it by the frontend's frame time
  (`SET_FRAME_TIME_CALLBACK`, `CoreInfo.frame_time = true`). SDL's `SDL_GetTicks` and Python's
  `time.time` and `time.sleep` as Ren'Py uses them read that clock. Fast-forward then runs transitions
  and animations faster without the game noticing.
- **Fast-forward also skips:** while the frontend fast-forwards, the core holds Ren'Py's skip (its Ctrl
  key), so dialogue that was already read flies by the way visual novel players expect.
- **Audio:** an SDL audio driver of the core's own writes what Ren'Py's mixer produces into a ring
  buffer. `retro_run` sends one frame's worth at 48 kHz stereo to `audio_sample_batch`.
- **Input:** the RetroPad becomes SDL game-controller events (`SDL_PushEvent`). Ren'Py's own controller
  bindings then apply: Cross advances, Circle opens the game menu, the shoulders roll back and forward,
  and the D-pad moves the focus. The touchpad, through the frontend's pointer, becomes the mouse.
  Dulcamara's button map in Settings works on top, as it does for EasyRPG.
- **Saves:** `config.savedir` is set to `/data/dulcamara/saves/renpy/<the game's save_directory>`. Ren'Py's
  persistent data lives there too.
- **Errors:** a Python exception Ren'Py does not handle shows Ren'Py's own traceback screen, which can be
  closed with the controller. The traceback also goes to Dulcamara's log, so a report can include it.

## 4. The library

`library.c` recognises a Ren'Py game: a folder with a `game/` folder that holds `.rpa`, `.rpyc` or `.rpy`
files. Its Ren'Py version comes from `renpy/vc_version.py` (`version = '8.…'`), or from
`renpy/__init__.py` in older releases.

- Ren'Py 8: `Game.core` is the Ren'Py core.
- Ren'Py 7: listed with "needs the Ren'Py 7 core (coming)" and not started, until milestone 4.
- No `renpy/` folder (some repacks): Ren'Py 8 is tried.

The title is the game's launcher name (`<name>.exe` or `<name>.sh` beside `game/`), else the folder name.
The cover is `game/gui/main_menu.png`, else `game/gui/window_icon.png`, else the default.

## 5. Checks

- `tests/test_renpy_core.py`, the counterpart of `tests/test_core.py`, builds the core for the host
  (`tools/build-renpy.sh host`) and runs it with the headless frontend of `tests/core` on
  `tests/renpy-game/`. That is a small game written for this repository: dialogue, a menu, an image, a
  sound and a save. Each run must load, give a picture every frame, change the picture when Cross is
  pressed, produce sound, take a menu choice, and save and load.
- `tests/test_library.py` gains the Ren'Py detection: version 8, version 7, no `renpy/` folder, and a
  folder that is not a game.
- On the console: the same test game, then a real Ren'Py 8 game the tester owns, with
  `/data/dulcamara/logs` sent back.

## 6. Milestones

1. **Host proof.** The core built for this computer runs `tests/renpy-game/` through the headless
   frontend. This proves the threading, the clock, the frame copy and the audio without a console.
2. **PS5 build.** The same core cross-built for the PS5. The test game runs on a console. The main risk
   is CPython on the PS5 SDK, and if it fails here the work stops and that is reported.
3. **Integration and release.** Library detection, covers, controls, saves, skip, a Settings page for
   the core, the notices, and a Dulcamara release with Ren'Py 8 support.
4. **Ren'Py 7.** A second core, `renpy7_retro_*`, with Python 2.7 and a pinned Ren'Py 7 release, built
   the same way. Ren'Py 7 games then start.

## Risks

- **CPython on the PS5 SDK.** Python's configure checks need cross-compile answers (a `config.site`) and
  some modules may need small patches (`posixmodule`, `signal`, `socket`). Ren'Py's own build tooling,
  renpy-build, cross-builds Python for Android, iOS and the web, and its recipes are the starting point.
- **SDL2's offscreen driver with Belladonna's EGL.** If PacBrew's SDL2 lacks the driver or EGL support,
  SDL2 is built from a pinned source with it.
- **Frame copy cost at 1920×1080.** It is about 8 MB a frame. The mapped one-frame-late copy and the dupe
  for unchanged frames should keep this small, and milestone 2 measures it.
- **Ren'Py's threads.** Ren'Py starts helper threads (image preloading, audio decoding). They run freely;
  only the main loop is handed back and forth.
