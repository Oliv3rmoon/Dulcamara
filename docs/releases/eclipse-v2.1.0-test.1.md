# Dulcamara Eclipse 2.1.0 · PS5 test build 1

This build targets two reported Summertime Saga startup errors and includes the itch.io Shelf. It keeps the existing Eclipse title ID, game library and save locations. PS5 installation, gameplay and encrypted installed storage still need hardware verification.

- **Summertime Saga 0.20.16** can use the Ren’Py 7 save serializer at `renpy.loadsave.dumps` again. The host check reproduces the failing call on 2.0 and verifies a save-data round trip through both serializer settings on 2.1.
- **Summertime Saga 21.0.0-wip.8194** uses a compiled Python package inside `game/lib.rpa`. Ren’Py 8 now recognizes archived packages and submodules compiled for its Python version, prefers source when both forms ship, and reports incompatible bytecode. The host check reproduces the original missing-module error and passes on 2.1.
- **itch.io Shelf** installs from **Settings → Library → Install itch.io Shelf**. Return to the library and open the selected shelf to browse free Ren’Py releases. The creator must offer a compatible download.
- **Library scans** find scripts nested under `game/`, recognize `_ren.py`, and explain unfinished `.ava-part` transfers. Finish the transfer and rescan. The rescan command now reports its progress without showing the previous game count.

The app needs read and write access to `/data/dulcamara` from the environment that launches it. Back up your existing saves before testing. Use the PS5 homebrew app-folder installation method you already use for Dulcamara, with the `PPSA99010` folder from the ZIP. Encrypted installed storage has not been verified.

For a console test, launch each Summertime Saga version from a fresh session and report the first screen reached. For the shelf, install *one night, hot springs*, return to the library, rescan and open it. If any step fails, the game version, step and first exception from `/data/dulcamara/logs/renpy-errors.txt` will help diagnose it.

Other compatibility reports remain open. The complete *Champion of Realms* 0.113 game rejects FreeBSD in its own launcher and reported a separate particle-effect error. *MyNewFamily* reported that Ren’Py could not restore an older save’s rollback position. Neither game’s reported failure has been verified fixed.
