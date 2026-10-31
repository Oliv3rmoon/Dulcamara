# Changelog

## 0.2.3

A test build for Ren'Py on the console: 0.2.2 still crashed as Python started, one logged step later (its main part).

- **The crash reports were read wrongly, and 0.2.2's cause was not the cause.** The console's kernel puts a crashed
  thread's registers 64 bytes into the context a handler is given, not 16 as the SDK's header says (a PS5 port of
  Wine measured the same). Every report so far read them 48 bytes early: its "rsp" was the fault address and its
  "rip" another register, which is why the stack pointer looked like a string's address. `readlink` was still worth
  standing in for (a title cannot reach it), but it was not what crashed.
- **The cause, read again at the right place:** Python's start calls `getcwd`. Finding no `renpy` program to start
  from, Python takes its own folder as the current one, and a PS5 title's libc has no `getcwd`: the one the link
  took, `libSceLibcInternal`'s, crashed. 0.2.2's registers are `getcwd`'s buffer, its size (1024) and the buffer's
  end. Both Ren'Py cores now keep the working folder themselves: each starts in its game's folder, `chdir` changes
  the console's and notes the new one, and `getcwd` and `realpath` answer from it.
- **`time.strftime` goes through `strftime`.** Python calls `wcsftime` for it (Ren'Py's save times), which, like
  `getcwd`, only `libSceLibcInternal` has and no title is known to have run; `strftime` runs on the console (EasyRPG
  Player's core uses it).
- **Crash reports** read the registers where the console puts them (checked against the fault address, and saying
  which layout they used), name the system module holding an address that is not in the app's list, and find return
  addresses in any executable memory.

## 0.2.2

A test build for Ren'Py on the console: 0.2.1 still crashed as Python started, at the same place.

- **The cause, found from 0.2.1's log:** Python's start calls `readlink` (to find its own path), and the core's
  `readlink` came from `libkernel_sys`, whose functions a title's import does not reach: the call ran something else
  with readlink's arguments, which set the stack pointer to a string's address (the crash's address, the same in
  both builds). Both Ren'Py cores now stand in for every function they could only get from `libkernel_sys`
  (`readlink`, `link`, `symlink`, `chown`, `umask` and the rest: a title has no links, owners or sessions to
  change), and the build fails if a core needs one it does not stand in for.
- **Closing a Ren'Py game no longer hangs Ren'Py 7.** The core asked Ren'Py to quit with an exception raised in its
  thread wherever it was; inside Python 2's `threading.RLock` that left a lock taken and Ren'Py 7 waiting on it for
  good (CI's tests caught it, and the log's stack showed where). Ren'Py now quits at its next look for events, as
  its own Quit does, without asking "are you sure"; the exception is only a fallback after a second, raised through
  Python's pending calls so the app never waits on Ren'Py for it.
- **Python starts in two logged parts** (its core, then its main part), so a log narrows a stop further.
- **Crash reports** give the registers and the code at the faulting instruction where it can be read, list where
  the app and the system modules are in the log, and no longer read a stack that is not mapped.

## 0.2.1

A test build for Ren'Py on the console: 0.2.0 crashed on a PS5 as a Ren'Py 8 game started.

- **Ren'Py's cores keep an environment of their own.** On the console 0.2.0's crash came a moment after Ren'Py's
  thread started, in a system library reading a string through a bad pointer, where Python copies the console's
  environment (`environ`) into `os.environ`. The cores no longer read the console's environment or call its
  `setenv`: Python, SDL and FFmpeg inside them see the core's own few variables.
- **The log says how far a Ren'Py game got.** Each step of starting it is written to `dulcamara.log` at once
  ("renpy: step: ..."): loading, Ren'Py's thread, Python starting and started, the boot script, SDL, OpenGL and the
  first picture.
- **Crash reports name what crashed.** A report now says which module the faulting instruction, the faulting
  address and each return address are in (the title or a system module, with the offset), and whether the main
  thread or another crashed. It reads the crashing thread's stack only as far as it is mapped: 0.2.0's report
  stopped short on a thread that had just started.

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
