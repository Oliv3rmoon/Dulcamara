# Dulcamara itch.io Shelf 1.1

A controller-friendly catalogue of free Ren’Py games for Dulcamara. It reads the current free Linux Ren’Py listings from itch.io and downloads the ZIP you select from the creator’s free download page. itch.io supplies the listings. Each game remains the work of its creator.

## Install and play

1. Unzip the release and copy the entire **Dulcamara itch.io Shelf 1.1** folder into **/data/dulcamara/games/**. Use a writable games location if you keep your library on another drive.
2. Rescan Dulcamara’s library. Select **Dulcamara itch.io Shelf 1.1** and open **Browse games**. Use the directional pad to move and Cross to select.
3. Select a title, choose **Find free downloads**, and pick the creator’s PC or Linux ZIP. Check the project address before choosing **Install game**. Ren’Py games can run Python when played.
4. After installation, choose **Return to Dulcamara** and rescan your library. Look for the game under its project slug and creator name.

On Eclipse 2.1 test build 2, **Settings → Library → Install itch.io Shelf** installs and selects version 1.1 alongside an existing 1.0 shelf. Both versions use the same games library.

The shelf requires Dulcamara’s Ren’Py 8 runtime, internet access and writable space for both the ZIP and its extracted contents. Different uploads from the same creator can be installed side by side. An additional upload uses its itch.io file ID in the folder name. Existing folders and saves are preserved. You can cancel an active transfer. A later installation reclaims an interrupted transfer’s staging folder.

## Supported downloads

The shelf uses anonymous free downloads hosted by itch.io. Creators can offer multiple files. Pick a ZIP containing a single Ren’Py game, up to 8 GiB compressed and 12 GiB of game data. Paid, account-only, browser-only and externally hosted files can require the creator’s website. You can visit the displayed project address to support the creator. A listing indicates the engine the creator chose on itch.io. Individual games may use features outside Dulcamara’s runtime.

## Source and packaging

The editable Ren’Py and Python source is included in this folder under **game/**. From an unpacked copy, run `python3 package.py OUTPUT.zip` to reproduce the release archive. A Unix host with Python 3 and zlib is sufficient. Dulcamara itch.io Shelf is licensed under GPL-3.0-or-later. See **LICENSE**. The package contains the shelf and its source. Games are downloaded separately from their creators.
