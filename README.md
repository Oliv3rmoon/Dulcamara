<p align="center">
  <img src="sce_sys/icon0.png" width="128" alt="Dulcamara icon">
</p>

<h1 align="center">Dulcamara</h1>

<p align="center">
  <strong>RPG Maker 2000 and 2003 games, and Ren'Py visual novels, on PS5 homebrew. Made for the controller, in Nightshade's black and red.</strong>
</p>

<p align="center">
  <a href="https://github.com/Oliv3rmoon/Dulcamara/releases/latest"><img src="https://img.shields.io/github/v/release/Oliv3rmoon/Dulcamara?style=for-the-badge&label=download&color=e5484d&labelColor=0d0d0f" alt="Download the latest release"></a>
  <a href="https://discord.gg/pSqgRU3YP9"><img src="https://img.shields.io/badge/discord-nightly-0d0d0f?style=for-the-badge&logo=discord&logoColor=e5484d" alt="Discord: nightly"></a>
  <a href="https://github.com/Oliv3rmoon/Dulcamara/stargazers"><img src="https://img.shields.io/github/stars/Oliv3rmoon/Dulcamara?style=for-the-badge&logo=github&logoColor=e5484d&color=e5484d&labelColor=0d0d0f" alt="GitHub stars"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-0d0d0f?style=for-the-badge&labelColor=0d0d0f&color=e5484d" alt="GPL-3.0-or-later"></a>
  <a href="https://buymeacoffee.com/oliv3rmoon"><img src="https://img.shields.io/badge/buy%20me%20a%20coffee-0d0d0f?style=for-the-badge&logo=buymeacoffee&logoColor=e5484d" alt="Buy me a coffee"></a>
</p>

Dulcamara, named after *Solanum dulcamara*, the bittersweet nightshade, is a PS5 homebrew app by **[Oliv3rmoon](https://github.com/Oliv3rmoon)** for story games and RPGs: a library that finds your RPG Maker 2000 and 2003 games and plays them, with the Nightshade look, CRT shaders and a Vulkan screen. The latest release is **0.1.0**; what changed is in the [changelog](docs/CHANGELOG.md).

> [!NOTE]
> **0.2.11 is a test release.** It adds Ren'Py visual novels; their picture and sound work on a console since 0.2.5, 0.2.9 fixed the crash on a game's first OGG sound, 0.2.10 started Ren'Py 7 games, and 0.2.11 is aimed at the error screen a long session could end in and finds where the black flashes are. It is checked on the build machine and in CI by tests that play a test game, and it is marked pre-release on the [Releases](https://github.com/Oliv3rmoon/Dulcamara/releases) page, so the updater skips it unless **Settings > System > Test builds** is on: [download 0.2.11](https://github.com/Oliv3rmoon/Dulcamara/releases/tag/0.2.11) to try it, and tell me what breaks.

## Why Dulcamara

- **RPG Maker 2000 and 2003 games.** The original engines' events, battles, pictures and sound (MIDI, WAV, Ogg Vorbis and MP3), the Maniac Patch and the extensions the engine supports, and Japanese, Korean, Chinese, Cyrillic and Western games with their text in their own encoding.
- **Ren'Py visual novels** *(test release 0.2.11)*. Version 8.5 is built in (with Python 3.12 and FFmpeg for music, voice and movies), and 7.8 (with Python 2.7) for games made with 7 or 6; the library picks the one a game was made with. They have their own section in the library. Cross moves the story on and picks, the D-pad moves between choices, Circle opens the game's menu, L1 and R1 roll back and forward, R2 skips text already read, L3 turns on a cursor the left stick moves, and the touchpad is the mouse; a game's own keys (its inventory's, say) can go on any button but R2, from its menu. Only one game runs each time the app starts, so opening a second one starts the app again.
- **A library that finds your games.** Copy a game's folder (or a `.zip` / `.easyrpg` of it) and it shows up with its title and its own title screen as the cover. Sort by title or by the last played; Square looks for new games without restarting.
- **Made for the controller.** Cross confirms, Circle cancels, Triangle opens the game's menu, and every button can be mapped again in Settings. The touchpad opens Dulcamara's menu at any time. Hold R2 (or press it once) to fast-forward from x2 to x8, for grinding and long cutscenes.
- **CRT and LCD shaders.** CRT Royale, CRT Lottes, CRT Pi, Newpixie, Hyllian NTSC, Mattias, NTSC signals, LCD3x, zfast LCD and Game Boy LCD, or sharp square pixels, whole-number scaling and the game's own shape.
- **Updates from its own releases.** It asks this repository for a newer release and installs it, after checking `PPSA99010.zip` against the release's `SHA256SUMS`.
- **Nothing to pay for, and no games.** No paid features, no ads, no account; free software under the GPL. It ships none and downloads none: copy your own.

## Install

1. Download `PPSA99010.zip` from the [latest release](https://github.com/Oliv3rmoon/Dulcamara/releases/latest) and unzip it. For Ren'Py visual novels, take it from the [0.2.11 test release](https://github.com/Oliv3rmoon/Dulcamara/releases/tag/0.2.11) instead.
2. Copy its `PPSA99010` folder to `/data/homebrew/` on the PS5 over FTP, as you install the other Nightshade apps, and set its permissions to `777` (in FileZilla: right-click the folder, *File permissions*, `777`, recurse into subdirectories). Without them the PS5 says *"Can't start the game or app"*. Nightshade's **Settings > Apps** can install it for you instead.
3. Restart ShadowMountPlus (or the console). **Dulcamara** appears on the home screen.
4. Start it once: it makes its folders in `/data/dulcamara/` and shows an empty library that says where games go. Copy your games to `/data/dulcamara/games/`, one folder (or `.zip`) each.

Its title ID, `PPSA99010`, is its own: it sits beside the other Nightshade apps.

Like the other Nightshade apps, it needs `/data` unlocked to look into its games folder: LegacyJB, etaHEN's *Legacy CMD server* or the Lapy helper it carries all do it (see [Unlocking /data](https://github.com/Oliv3rmoon/Mandrake/blob/main/docs/GUIDE.md#unlocking-data-so-swanstationps5-can-see-your-games)).

## Docs

- **[Guide](docs/GUIDE.md):** [where games go](docs/GUIDE.md#where-games-go), [controls](docs/GUIDE.md#controls), [settings](docs/GUIDE.md#settings), [updates](docs/GUIDE.md#updates) and [building from source](docs/GUIDE.md#building-from-source).
- **[Changelog](docs/CHANGELOG.md)**, [Privacy policy](PRIVACY.md), [Third-party notices](THIRD_PARTY_NOTICES.md) and [Contributing](CONTRIBUTING.md).

## Requests, bugs and support

- **Ask for a fix or a feature:** [open an issue](https://github.com/Oliv3rmoon/Dulcamara/issues/new/choose), or ask in **[nightly on Discord](https://discord.gg/pSqgRU3YP9)**.
- **Bugs:** open an issue with `/data/dulcamara/logs/dulcamara.log`, copied right after the problem, and which game it was. Never attach games or their files.
- **A game that doesn't run right:** check it on a PC with the engine's own player first (see [Thanks](#thanks)); if it fails there too, that project's issues are the place.
- **If my work helps you,** you can [buy me a coffee](https://buymeacoffee.com/oliv3rmoon).

## The Nightshade family

Dulcamara is one of seven repositories in the Nightshade family, all by [Oliv3rmoon](https://github.com/Oliv3rmoon): the same black-and-red look, one Discord.

| App | What it is |
| --- | --- |
| [**Nightshade**](https://github.com/Oliv3rmoon/Nightshade) | The Switch emulator for PS5 homebrew |
| [**Physalis**](https://github.com/Oliv3rmoon/Physalis) | Live TV, movies and Plex on the PS5 |
| [**Datura**](https://github.com/Oliv3rmoon/Datura) | Game streaming from your PC to the PS5 |
| [**Mandrake**](https://github.com/Oliv3rmoon/Mandrake) | PS1 emulation, with two emulators |
| **Dulcamara** (this repo) | RPG Maker 2000 and 2003 games, and Ren'Py visual novels |
| [**Solanum**](https://github.com/Oliv3rmoon/Solanum) | A store for open-source PS5 homebrew, with its catalog [on the web](https://oliv3rmoon.github.io/Solanum-Catalog/) |
| [**Belladonna**](https://github.com/Oliv3rmoon/Belladonna) | Nightshade's PS5 Vulkan driver |

## Built on

[EasyRPG Player](https://easyrpg.org) for RPG Maker 2000 and 2003 games and [Ren'Py](https://www.renpy.org) for visual novels, on the frontend of [Mandrake](https://github.com/Oliv3rmoon/Mandrake).

## Thanks

- **[EasyRPG](https://easyrpg.org)**, the team behind EasyRPG Player and liblcf, who made RPG Maker 2000
  and 2003 games playable everywhere as free software. Dulcamara only puts their work on the PS5;
  how well a game runs is theirs to credit. Check a game on EasyRPG Player on a PC first; if it fails
  there too, the [EasyRPG Player issues](https://github.com/EasyRPG/Player/issues) are the place.
- **[Ren'Py](https://www.renpy.org)**, by Tom Rothamel and the Ren'Py contributors, the visual novel
  engine built into the app as two cores (Ren'Py 8, and Ren'Py 7 for older games).
- **[Mandrake (formerly Nightshade Station)](https://github.com/Oliv3rmoon/Mandrake)** and the work it is made of:
  **[SwanStationPS5](https://github.com/darkxex/SwanStationPS5)** by **darkxex**, whose frontend this is,
  **[PSXS5](https://github.com/SynoPiia/PSXS5)** by **SynoPiia**, and **BlackBearReloaded**'s
  ps5-native-app-boilerplate.
- **[ps5-payload-dev](https://github.com/ps5-payload-dev)** for the SDK and PacBrew, **Mihawk-99** for the
  RADV port, and everyone in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

Dulcamara is free software under the GNU General Public License, version 3 or later
([LICENSE](LICENSE)). EasyRPG Player is GPL-3.0-or-later and liblcf MIT; Ren'Py is MIT, with
LGPL-derived parts; every component keeps its own licence, listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the app folder carries their texts in `licenses/`.

## Disclaimer

Dulcamara is an unofficial project: neither the EasyRPG team, the Ren'Py project, Enterbrain / Kadokawa nor
Sony endorse it. RPG Maker is a trademark of its owners. No game, RTP, key or system file is included or
downloaded: use games you have the right to play.

## Help it grow

Like Dulcamara? **[Star it](https://github.com/Oliv3rmoon/Dulcamara)** and **[join the Discord](https://discord.gg/pSqgRU3YP9)**.
