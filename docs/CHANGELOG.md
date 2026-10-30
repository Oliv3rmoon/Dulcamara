# Changelog

## 0.2.0

Ren'Py visual novels in the same app, on Ren'Py itself.

- **Ren'Py 8 inside the app.** Ren'Py 8.5.3 as Dulcamara's own libretro core (`src/core/renpy`), with CPython
  3.12.8, FFmpeg 4.3.1 (LGPL) and Assimp, drawing through ps5-opengl into the Vulkan screen. Ren'Py games are found
  by their `game/` folder, with their launcher's name and `gui/main_menu.png` as the cover; Cross moves the story
  on, Circle opens the game's menu, L1 and R1 roll back and forward, R2 also skips read text, the touchpad is the
  mouse, and each core keeps its own buttons. Saves go to `/data/dulcamara/saves/renpy`. Checked on the build
  machine and in CI by `tests/test_renpy_core.py`, which plays a test game written for it.
- **Ren'Py 7 inside the app,** for Ren'Py 7 and 6 games: Ren'Py 7.8.7 on the same core, with CPython 2.7.18 and
  pygame_sdl2 (`tools/build-renpy7.sh`). The library sends each game to the Ren'Py it was made with, by its
  `renpy/` folder or, without one, its `lib/`. `tests/test_renpy7_core.py` plays the same test game on it.
- Ren'Py runs one game each time the app starts: opening another Ren'Py game starts the app again, and the game
  opens then.
- The shelf names a Ren'Py game's Ren'Py version.

## 0.1.0

The first Dulcamara, a Nightshade app: RPG Maker 2000 and 2003 games on the PS5, on EasyRPG Player.

- **EasyRPG Player inside the app.** Its libretro core (EasyRPG/Player at `096c004`, liblcf at `6854310`) is
  built for the PS5 into the app, with the text encodings of Japanese, Korean, Chinese, Cyrillic and Western
  games through iconv.
- **The library.** Games in `/data/dulcamara/games`: a folder holding `RPG_RT.ldb` and `RPG_RT.lmt`, one
  folder down, or a `.zip` / `.easyrpg`. Titles from `RPG_RT.ini`, covers from each game's title screen, sorted by
  title or the last played.
- **Playing.** Cross confirms and Circle cancels; every button can be mapped. R2 fast-forwards (x2 to x8), the
  touchpad opens the menu (resume, fast-forward, back to the title screen, settings, quit).
- **The picture.** Mandrake's shaders (CRT Royale and the rest) on EasyRPG's picture, sharp or smooth
  scaling, whole-number scaling, 4:3 or 16:9, Scale2x and xBR.
- **Nightshade's look,** its own icon and home-screen pictures, and updates from
  [Oliv3rmoon/Dulcamara](https://github.com/Oliv3rmoon/Dulcamara/releases).
- Built on the frontend of Mandrake (formerly Nightshade Station) 1.6.0, without its PS1 parts.
