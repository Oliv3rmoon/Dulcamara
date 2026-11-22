# Adding a game from a link

Dulcamara can download a game's ZIP from a **direct link** and put the game in `games/`, instead of the game
being copied over from a PC. A direct link is one that starts the download of the `.zip` file itself when it is
opened, not a web page with a download button on it.

This note is the design: the flow, its states, where its files go, its limits and why, and what it does not do.

## Two ways in, one queue

1. **Settings > Library > Add game from link**: Dulcamara's own keyboard (the one the insider code uses; the
   console's keyboard cannot be called from the app) with a URL layout (`/ : . - _ ? = & % ~ + # @`). Done starts it.
2. **Settings > Library > Add games from links.txt**: `/data/dulcamara/links.txt`, written on a phone or PC and
   copied over FTP, one `https://` or `http://` URL a line; empty lines and lines starting with `#` are ignored. Each
   line not yet processed is queued, in file order, at most 64 a run.

The user's `links.txt` is never rewritten. A processed line is recorded in `/data/dulcamara/links-done.txt`
(`<outcome><TAB><url>` a line, written atomically, any `user:password@` left out): `added` when its game was installed, `failed` when it can never
work as it is (not found, a web page, not a ZIP, an unsafe or damaged archive). Lines that failed for a passing
reason (no connection, a stall, no space, cancelled, a game running) stay pending and are tried at the next run. To
try a recorded line again, delete its line from `links-done.txt`. A `links-done.txt` that is a link, a folder or a
file that can't be read is never followed or rewritten: nothing is recorded, and the log says why.

## Flow and states

One worker thread (`src/links.c`) runs the queue; the main thread polls it (`links_poll`) and never waits.

```
IDLE -> DOWNLOADING -> CHECKING -> UNPACKING -> INSTALLING -> (next link) -> DONE
            \______________\____________\____________\--> FAILED (per link; the queue goes on)
any state -> CANCELLED (Cross on the row; the current link is undone, the rest stay pending)
```

For each link (`link_add`, `src/link_unpack.c` and `src/link_fetch.c`):

1. **Gate.** `maintenance_acquire(MAINT_LINKS)`, the same process-wide gate backups, restores and updates use. It
   is refused while a game owns the runtime, while startup recovery is unfinished, or while another maintenance job
   runs; nothing is created then. While the gate is held, `maintenance_launch` refuses to start a game, so nothing
   is downloaded or extracted while a game runs.
2. **Download** (`link_fetch.c`, libcurl as the updater uses it, a separate function so the updater's behaviour is
   unchanged) to `<games>/.dulcamara-links/<id>.zip.part`: redirects followed, at most 10, http/https only; a stall
   is under 1 KiB/s for 60 s (no limit on the whole download); progress is bytes and the total once the server says;
   cancel is checked several times a second. The first bytes decide the format: anything but a ZIP local header
   stops the transfer at once with the reason (a web page, RAR, 7-Zip, gzip/xz/bzip2/zstd/tar, a Windows `.exe`).
   A dropped or stalled transfer that received data resumes with an HTTP Range request, up to 5 times; a server
   that ignores ranges, or answers with a `Content-Range` that does not go on from the bytes kept (another offset,
   or an unknown or different total), is restarted from zero. An archive put together from more than one transfer
   that then fails its integrity check counts as a passing failure: it is deleted, and the link is downloaded from
   the start next time (its `links.txt` line stays pending). Free space is checked when the size is known and every
   16 MiB.
3. **Check** (`link_unpack.c`): the central directory is read with miniz through `pread` (64-bit offsets, ZIP64),
   and every entry is validated before anything is written (limits below).
4. **Unpack** into `<games>/.dulcamara-links/unpack-<id>/<Folder>/`, streaming: miniz inflates each member through a
   callback in 64 KiB pieces, so neither the archive nor a member is held in memory; the callback refuses more
   bytes than the entry declares, and miniz checks each CRC-32. All-zero pieces are skipped with `lseek` (the file
   is extended with `ftruncate`), which keeps sparse data sparse. Each file is `fsync`ed, then each folder.
5. **Install**: `<Folder>` is renamed into `games/` under a name that does not exist (`Name`, `Name (2)` ...
   `Name (99)`). `rename` cannot merge into or replace a non-empty folder, so an existing game is never touched.
   The library never sees a half-extracted game: the staging folder starts with `.` and the scan skips such names.
6. **Clean**: the download and the unpack folder are deleted on success, on failure and on cancel. A download that
   failed for a passing network reason keeps its `.part` until the app closes, so trying again resumes it.
7. **Afterwards** (main thread, `src/ui/links_ui.c`): the library is rescanned; when the new (non-cached) scan is
   published, the new game is selected. If the scan does not list the folder, its "not shown" reason from the scan
   is shown (another engine, nested too deep...), or "no RPG Maker 2000/2003 or Ren'Py game in it".

The folder's name is the archive's single top folder when every entry is inside one (`Game/...`), else the file's
name (from `Content-Disposition`, else the URL's last part) without `.zip`. `__MACOSX/` entries are skipped. Names
are cleaned: no `/ \ :` or control characters, no leading `.`, at most 96 bytes.

**At startup** (`links_cleanup`, before the first scan) `<games>/.dulcamara-links` is deleted whole, without
following links: a crash or a kill at any step leaves either nothing in `games/` or a complete renamed game.

## File layout

```
/data/dulcamara/links.txt                       the user's list (read only)
/data/dulcamara/links-done.txt                  processed lines
/data/dulcamara/games/.dulcamara-links/         staging, same volume as games/ (hidden from the library)
    <id>.zip.part, <id>.zip                     the download (<id>: a hash of the URL)
    unpack-<id>/<Folder>/...                    the game being extracted
/data/dulcamara/games/<Folder>/                 the installed game
```

## Limits, and why

| Limit | Value | Why |
| --- | --- | --- |
| Redirects | 10 | download hosts redirect 2 or 3 times; a loop must end |
| Stall | under 1 KiB/s for 60 s | as the updater: slow Wi-Fi is fine, a dead connection is not |
| Resumes | 5 per link, only after progress | a flaky connection finishes; a dead server does not loop |
| URL length | 2048 bytes | longer is not a direct link |
| Links per run | 64 | one run stays a bounded job |
| `links.txt` size | 256 KiB | it is read whole |
| Entries | 65 536 | big Ren'Py games have a few thousand files; a count bomb is refused before any write |
| Total unpacked size | 64 GiB, and the free space | the largest games are about 20 GiB |
| Ratio | declared total over compressed total at most 200:1 once over 256 MiB; any Deflate entry at most 1032:1 (the format's ceiling); a Stored entry exactly 1:1 | game data compresses 1:1 to 10:1; a bomb declares far more |
| Free space | archive + unpacked + 4 KiB an entry + 16 MiB | checked before downloading (size known), every 16 MiB while downloading, before unpacking; a full disk while writing is still caught and cleaned |
| Path | the installed path under 512 bytes (NP_PATH_MAX), each name under 256 | the library and cores use 512-byte paths |
| Names | relative, no `.`/`..`/empty parts, no `\`, `:` or control characters | traversal, Windows-style paths and drive letters |
| Entry types | regular files and folders only (Unix mode checked); no encryption; Store or Deflate | links and devices could write outside the folder; others cannot be unpacked |
| Duplicates | the same name twice (any case) must be the same entry (size and CRC), a file cannot also be a folder | an archive must not say two things about one file |
| Overlap | entries' data may not overlap | overlapping entries are a known bomb shape |

## Known limits

- **A slow server holds the gate.** The gate is held for the whole download, and only a stall (under 1 KiB/s for
  60 s) ends it, with no overall limit when the size is not given. A server that keeps sending just over 1 KiB/s
  keeps game launches, backups, restores and updates "busy" until Cross cancels the row. No data is at risk.
- **Cleanup is check-then-act.** Removing the staging tree (`link_remove_tree`), opening files while unpacking, and
  `staging_dir` check a path and then use it by name. An archive or a server cannot exploit this; only something
  racing on the console itself (for example over FTP) could redirect it.
- **Gaps in a queue.** The gate is released between links. A game launched in that moment makes the remaining
  links fail with "busy" (not recorded, but counted as failed in the end message). A crash after a game is renamed
  into `games/` and before its line is recorded leaves the line pending, so the next run adds `Name (2)`.
- **Names without the UTF-8 flag** (CP437, Shift-JIS) are written as their raw bytes, and Shift-JIS names with the
  byte `\` are refused; some old Japanese RPG Maker ZIPs are refused or unpack with names the game can't find.
- **Depth.** A tree deeper than 128 folders fails with "Couldn't finish writing the game."
- **Startup cleanup** runs before the first frame: after a crash in the middle of unpacking a very large game, the
  next start deletes its files with nothing on screen yet.
- **Plain http.** A redirect may go from https to http; integrity then rests on the archive's own checks (CRC-32 for
  each entry, and every check above).

## Out of scope

This downloads a file from a direct link. It does not log in to stores or sites, does not run a game's own Windows
launcher, installer or patcher, does not solve itch.io, Patreon, Mega, Google Drive or other download pages, and does
not look for updates of a game by itself. It unpacks ZIP only: RAR, 7-Zip, tar and installers are refused with what
to do instead (extract on a PC and copy the folder, or find a `.zip` link). Dulcamara ships with no links, no catalogue
and no game names. Old Japanese ZIPs whose Shift-JIS names contain the byte `\` are refused as unsafe names.

## Tests

`tests/test_game_links.py` builds `tests/game_links_check.c` with the production sources under ASan and UBSan and runs
it against a local HTTP server on 127.0.0.1 (no internet). See its docstring for the full list.
