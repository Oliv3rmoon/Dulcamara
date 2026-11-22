# Dulcamara Eclipse 2.1.0 · PS5 test build 2

This PS5 prerelease includes itch.io Shelf 1.1 and both Summertime Saga startup fixes from test build 1. It uses the existing Eclipse title ID and game save locations. PS5 installation, gameplay and encrypted installed storage still need hardware verification.

## itch.io Shelf 1.1

Open **Settings → Library → Install itch.io Shelf**, then open **Dulcamara itch.io Shelf 1.1** in the library. This installs a separate folder alongside Shelf 1.0. Games you downloaded with 1.0 stay in the library.

The earlier PS5 runtime always failed DNS lookups from Ren’Py. Both runtimes now use Dulcamara’s normal console resolver. The Shelf also gives separate messages for DNS failures, certificate problems and timeouts. On the host, the bundled Shelf opened the live itch.io catalogue, listed the creator’s downloads for *one night, hot springs*, and read the start of the Windows/Linux ZIP over HTTPS.

The Shelf now shows creator-labeled Windows and Linux downloads such as **CoR 0.113 PC** and checks that the response is a ZIP before continuing. It accepts up to 8 GiB compressed and 12 GiB of extracted game data, with a free-space check for each stage. Separate uploads on the same itch.io project can coexist, including consecutive chapters. Installing again preserves existing folders and saves.

## Compatibility

- The publisher’s complete **Summertime Saga 0.20.16** reached its interactive splash screen on the host. Its save serializer is available again, and the runtime can render the empty transparent image the game creates during startup. The complete **21.0.0-wip.8194** preview reached its menu with the compiled `saga` package loaded from the game archive. Library scanning also recognizes nested scripts and `_ren.py`, skips unfinished `.ava-part` transfers and reports rescan progress.
- **My New Family** requires you to finish 0.14 and save when the game prompts. Install the 0.14 PC and Epilogue PC downloads separately. Both use the same Ren’Py save directory. A fresh ending save made with the publisher’s 0.14 scripts resumed in Epilogue chapter 10 on the host. Keep a copy of your existing saves. [Creator’s handoff instructions](https://killahseven.itch.io/my-new-family)
- **Champion of Realms 0.113** has a full **CoR 0.113 PC** download. Install it to a fresh game folder. The clean download’s 103 indexed RPA archives and its loose game files contain no copy of the launcher script named in the earlier FreeBSD error. The complete game reached its startup warning on the host, where its own particle displayable accepted the reported three arguments and created 200 particles. The creator also lists a separate two-archive bugfix download, which requires the base game. [Creator’s downloads](https://projectwill.itch.io/champion-of-realms)

Both engines now include compiled copies of all 33 optional Ren’Py screen layouts. This lets games open menus with read-only app assets.

## Install and test

**Updating from Eclipse 2.0.0:** Back up your saves, then open **Settings → System**, enable **Test builds**, and choose **Check for updates now**. The first 2.1 test tag was invisible to 2.0’s version parser. This release uses `v2.1.0`, which it recognizes. If you already installed test build 1, both builds identify as 2.1.0, so install build 2 manually.

**Manual installation:** Back up your saves, then install the `PPSA99010` folder from the app ZIP using your existing PS5 homebrew app-folder installation method. The environment that launches Dulcamara needs read and write access to `/data/dulcamara`.

Launch each Summertime Saga version from a fresh session and report the first screen reached. If an earlier game still stops, include the game build, whether it was a fresh folder or an update, and the first exception from `/data/dulcamara/logs/renpy-errors.txt`. For a save issue, include the game version and the point where the save was made.
