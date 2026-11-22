# Dulcamara Eclipse 2.1.0 · PS5 test build 2

This PS5 prerelease includes itch.io Shelf 1.1 and both Summertime Saga startup fixes from test build 1. It uses the existing Eclipse title ID and game save locations. PS5 installation, gameplay and encrypted installed storage still need hardware verification.

## itch.io Shelf 1.1

Open **Settings → Library → Install itch.io Shelf**, then open **Dulcamara itch.io Shelf 1.1** in the library. This installs a separate folder alongside Shelf 1.0. Games you downloaded with 1.0 stay in the library.

The Shelf now shows creator-labeled Windows and Linux downloads such as **CoR 0.113 PC** and checks that the response is a ZIP before continuing. It accepts up to 8 GiB compressed and 12 GiB of extracted game data, with a free-space check for each stage. Separate uploads on the same itch.io project can coexist, including consecutive chapters. Installing again preserves existing folders and saves.

## Compatibility

- **Summertime Saga 0.20.16** can call its original save serializer. **21.0.0-wip.8194** can import its compiled Python package from the game archive. Library scanning also recognizes nested scripts and `_ren.py`, skips unfinished `.ava-part` transfers and reports rescan progress.
- **My New Family** requires you to finish 0.14 and save when the game prompts. Install the 0.14 PC and Epilogue PC downloads separately. Both use the same Ren’Py save directory. A fresh ending save made with the publisher’s 0.14 scripts resumed in Epilogue chapter 10 on the host. Keep a copy of your existing saves. [Creator’s handoff instructions](https://killahseven.itch.io/my-new-family)
- **Champion of Realms 0.113** has a full **CoR 0.113 PC** download. Install it to a fresh game folder. The clean download’s 103 indexed RPA archives and its loose game files contain no copy of the launcher script named in the earlier FreeBSD error. The game defines its own particle displayable. The creator also lists a separate two-archive bugfix download, which requires the base game. [Creator’s downloads](https://projectwill.itch.io/champion-of-realms)

## Install and test

Back up your saves, then install the `PPSA99010` folder from the app ZIP using your existing PS5 homebrew app-folder installation method. The environment that launches Dulcamara needs read and write access to `/data/dulcamara`.

Launch each Summertime Saga version from a fresh session and report the first screen reached. If an earlier game still stops, include the game build, whether it was a fresh folder or an update, and the first exception from `/data/dulcamara/logs/renpy-errors.txt`. For a save issue, include the game version and the point where the save was made.
