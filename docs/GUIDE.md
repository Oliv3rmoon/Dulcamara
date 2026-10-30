# Dulcamara guide

Where games go, the controls, the settings, how Dulcamara updates, and how to build it from source.

Back to the [README](../README.md).

## Where games go

```text
/data/homebrew/PPSA99010/            the app
/data/dulcamara/
├── games/
│   ├── <Game>/                      one folder per game: RPG_RT.ldb, RPG_RT.lmt, RPG_RT.ini, Title/, ...
│   ├── <Series>/<Game>/             or one folder down
│   ├── <Game>.zip                   or a .zip of the game (the game at its root or in a folder inside it)
│   ├── <Game>.easyrpg               or an .easyrpg (a .zip by another name)
│   └── <Ren'Py game>/               a Ren'Py game's folder, with game/ (its renpy/ or lib/ tells which Ren'Py)
├── rtp/2000/ and rtp/2003/          the RPG Maker run-time packages (RTP), for games that need them
├── saves/renpy/                    Ren'Py games' saves, one folder each (their config.save_directory)
├── settings.ini, played.txt
└── logs/dulcamara.log       send this with bug reports
```

- **A game is a folder that holds `RPG_RT.ldb` and `RPG_RT.lmt`**, in any case. Copy the whole folder,
  with its `Title`, `CharSet`, `Music` and other folders.
- **Saves stay in the game's own folder** (`Save01.lsd`...), as on a PC, so they move with the game. For
  a `.zip` or `.easyrpg`, EasyRPG keeps them in a `.save` folder beside the archive.
- **The RTP.** Many games use pictures and sounds from the RPG Maker run-time package instead of
  carrying them. If a game shows missing graphics or sounds, put the RTP's folders (`Backdrop`,
  `Battle`, `CharSet`...) in `rtp/2000` (RPG Maker 2000 games) or `rtp/2003` (2003 games). The free
  [EasyRPG RTP replacement](https://easyrpg.org/rtp-replacement/) works there too.
- Copy games over FTP: the app reads the folder each time it opens, and Square on the library looks again.

## Controls

| Button | In a game (EasyRPG Player's buttons, the defaults) |
| --- | --- |
| Cross | Decision (A) |
| Circle | Cancel (B) |
| Triangle | Cancel and the game's menu (X) |
| Square | Shift (Y) |
| D-pad, left stick | Move |
| L3, R3 | The 0 and 5 keys |
| OPTIONS | EasyRPG Player's own settings |
| R2 | Fast-forward (while held, or on and off) |
| Touchpad | Dulcamara's menu: resume, fast-forward, back to the title screen, settings, quit |

Every button but R2 and the touchpad can be mapped in **Settings > Controls**. On the library: Cross
plays, Triangle sorts, Square looks for new games, OPTIONS opens Settings.

## Settings

- **Picture:** the shader, smooth or sharp scaling, whole-number scaling, the game's shape or 16:9,
  upscaling (Scale2x, xBR) when no shader is on. Opened from a game, the game shows behind it.
- **Playing:** the fast-forward speed and how R2 works, the left stick, the game's volume, the FPS.
- **Controls:** each controller button's game button.
- **Library:** the order, the interface sounds.
- **System:** the update check, and closing the app.

## Updates

When it starts (and from **Settings > System > Check for updates now**) Dulcamara asks GitHub for
the newest release of [Oliv3rmoon/Dulcamara](https://github.com/Oliv3rmoon/Dulcamara/releases)
that is not a pre-release. When its tag is a newer version and it carries `PPSA99010.zip` and
`SHA256SUMS`, both this repository's own files, the ZIP is checked against `SHA256SUMS` and installed
into the folder the app runs from; the new version runs from the next start. Your games and saves are not
touched. A release built by this repository's workflow can be checked with
`gh attestation verify PPSA99010.zip -R Oliv3rmoon/Dulcamara`.
## Building from source

The PS5 app builds on Linux or WSL (Ubuntu 24.04); GitHub Actions builds every push
([.github/workflows/build.yml](../.github/workflows/build.yml)):

```bash
sudo apt install clang-18 lld-18 llvm-18 libclang-rt-18-dev make ninja-build ccache cmake pkg-config \
  python3 python3-venv tar unzip wget zip zstd
git clone --recurse-submodules https://github.com/Oliv3rmoon/Dulcamara.git
cd Dulcamara
make                            # -> dist/PPSA99010/ and dist/PPSA99010.zip
python3 tools/make_release.py   # optional: build, and pack PPSA99010.zip as a release has it
```

`make` builds EasyRPG Player's core (`tools/build-easyrpg.sh`: EasyRPG Player and liblcf from their
submodules, pixman from its release, the rest from the PacBrew prefix, all in one archive with the API
renamed `easyrpg_retro_*`), links the RADV Vulkan driver (`tools/fetch-radv.sh`), compiles `src/` and
signs `eboot.bin`.

The host checks need no PS5 SDK (cmake, a C/C++ compiler, and fmt, libpng, FreeType, HarfBuzz,
libvorbis, mpg123 and pixman from the system):

```bash
make host-checks   # the game detection (tests/test_library.py) and EasyRPG Player run headless
                   # on EasyRPG's TestGame (tests/test_core.py), fetched for the test only
make desktop       # the app in a window on this computer (SDL2), for working on the interface
```

| Path | What |
|---|---|
| `src/main.c` | start-up, the main loop, the game screen |
| `src/core/cores.h`, `cores.c` | the cores' table: each core's libretro functions and defaults |
| `src/core/host.c` | the libretro host: environment, picture, sound, buttons |
| `src/library.c`, `src/rpgmaker.c` | finding games, their titles, covers and engine |
| `src/covers.c` | the title screens, read on a background thread |
| `src/ui/` | the library, the in-game menu, Settings, and Mandrake's drawing, text and sounds |
| `src/platform/` | Mandrake's PS5 platform layer and Vulkan screen (with the CPU-frame upload for the shaders) |
| `tools/build-easyrpg.sh` | the core, for the PS5 or (`host`) for this computer |
| `tools/patches/liblcf-iconv.patch` | liblcf's encodings through iconv, without ICU |
| `tools/render-home-art.py` | the icon and the home-screen pictures |
| `docs/boilerplate/` | the PS5 app boilerplate everything is built on |

### Adding a core

A second core (ScummVM's libretro core is next) is a static archive built like EasyRPG's, with its API
renamed `<core>_retro_*`, and one entry in `src/core/cores.c`'s table; `src/core/cores.h` lists the steps.
