#!/usr/bin/env python3
# Dulcamara - adding a game from a link, built from the production sources and run on the host.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds tests/game_links_check.c with src/links.c, src/link_fetch.c, src/link_unpack.c, src/maintenance.c, the
library's scan and miniz (as src/vendor.c compiles it) under the address and undefined-behaviour sanitizers, warnings
as errors, with the host's libcurl and a stall limit of 3 seconds, and runs it against a server on 127.0.0.1 (nothing
here reaches the internet):

* a tiny RPG Maker game's ZIP (one top folder) and a tiny Ren'Py game's ZIP (loose entries, its name from
  Content-Disposition) download, unpack, and are listed by the library's real scan; a small ZIP64 archive too;
* a transfer that drops halfway goes on with a Range request; a server without ranges, or whose Content-Range does
  not go on from the bytes kept (another offset, an unknown or another total), is started over; a file that changed
  on the server between the tries is a passing failure, tried again from the start; a redirect chain is followed, a
  redirect loop is given up after 10, and a redirect to file:// or ftp:// is refused;
* a Content-Disposition or URL name ending in "%" or "%X" is kept as it is, with nothing read past it; a link's
  user:password@ never reaches the log, a message or links-done.txt;
* a web page (by its Content-Type and by its bytes, even named .zip), a RAR, a 7-Zip, a gzip and a Windows .exe stop
  the download at once with the reason, and nothing is kept; 404 and 403 say so; a refused connection and a TLS
  handshake with a plain HTTP server are told apart; a stalled connection is given up after the stall limit;
* each unsafe archive is refused with nothing written outside the staging folder and nothing left in it: traversal
  (../), an absolute path, backslashes, a symlink entry, a bomb by ratio, a bomb by count (65 537 entries), a name
  over the path limit, two different entries with one name, an entry whose data is larger than it declares,
  entries sharing their data, a case-only collision, a file that is also a folder, a colon, a control byte or a NUL
  in a name (a refused name's control bytes never reach the log as they are); a local header whose name and sizes
  disagree with the central directory changes nothing that is written;
* a ZIP64 archive over 4 GiB (made sparse: a 4 GiB + 1 MiB member of zeros, and entries after the 4 GiB mark) unpacks
  and is listed, sparse on disk;
* a game whose folder name exists: the existing game is untouched byte for byte, the new one is "Name (2)";
* a full disk (a 64 MiB disk image, macOS) while downloading (with and without the size known first) and while
  unpacking: "not enough space", nothing left behind;
* a kill (SIGSTOP at a progress call, then SIGKILL, as the save tests do) at every step of the download, the check,
  the unpacking and the rename, then the start's cleanup: no partial game, no staging left, and a game renamed before
  the kill is complete and listed;
* cancellation at each stage: nothing installed, staging removed;
* refusal while a game holds the runtime: nothing created;
* links.txt: comments, blank and invalid lines, the done file (never followed through a link, never emptied by a
  read error), and the background queue with its results.
"""

import hashlib
import http.server
import io
import os
import platform
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import urllib.parse
import time
import unittest
import warnings
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", "-D_DEFAULT_SOURCE"]
LIBRETRO = "third_party/easyrpg-player/builds/libretro/libretro-common/include"
SOURCES = ["tests/game_links_check.c", "src/links.c", "src/link_fetch.c", "src/link_unpack.c", "src/net.c", "src/maintenance.c",
           "src/library.c", "src/rpgmaker.c", "src/util.c", "src/storage.c",
           "examples/self-update/self_update_sha256.c"]
STALL_SECONDS = 3

# LinkError (src/links.h)
OK, CANCELLED, BUSY, BAD_URL, CONNECT, TLS, HTTP, WEB_PAGE, STALLED, NETWORK, NO_SPACE, FORMAT, UNSAFE, DAMAGED, IO = \
    range(15)
# LinkStage
DOWNLOAD, CHECK, UNPACK, INSTALL = 1, 2, 3, 4


def zip_bytes(entries, compression=zipfile.ZIP_DEFLATED, force_zip64=False):
    buffer = io.BytesIO()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")  # the duplicate-name archive is deliberate
        with zipfile.ZipFile(buffer, "w", compression) as z:
            for entry in entries:
                name, data = entry
                if isinstance(name, zipfile.ZipInfo):
                    info = name
                else:
                    info = zipfile.ZipInfo(name, (2026, 11, 10, 12, 0, 0))
                    info.compress_type = compression
                    info.external_attr = (0o40755 << 16) | 0x10 if name.endswith("/") else 0o100644 << 16
                if force_zip64:
                    with z.open(info, "w", force_zip64=True) as f:
                        f.write(data)
                else:
                    z.writestr(info, data)
    return buffer.getvalue()


def rpg_maker(folder, title):
    return [(f"{folder}/", b""), (f"{folder}/RPG_RT.ldb", b"LcfDataBase" + bytes(200)),
            (f"{folder}/RPG_RT.lmt", b"LcfMapTree" + bytes(64)),
            (f"{folder}/RPG_RT.ini", f"[RPG_RT]\r\nGameTitle={title}\r\n".encode()),
            (f"{folder}/Title/title.png", bytes(range(256)) * 8),
            (f"{folder}/Music/theme.txt", b"music " * 2000)]


POCKET = zip_bytes(rpg_maker("Pocket Quest", "Pocket Quest"))
REDIRECTED = zip_bytes(rpg_maker("Redirect Quest", "Redirect Quest"))
FLAKY = zip_bytes(rpg_maker("Flaky Quest", "Flaky Quest") +
                  [("Flaky Quest/Movie/noise.bin", bytes((i * 7919) % 251 for i in range(3 * 1024 * 1024)))],
                  compression=zipfile.ZIP_STORED)
NORANGE = zip_bytes(rpg_maker("Plain Quest", "Plain Quest") +
                    [("Plain Quest/Movie/noise.bin", bytes((i * 31) % 253 for i in range(2 * 1024 * 1024)))],
                    compression=zipfile.ZIP_STORED)
SLOW = zip_bytes(rpg_maker("Slow Quest", "Slow Quest") +
                 [("Slow Quest/Movie/noise.bin", bytes((i * 131) % 241 for i in range(1024 * 1024)))],
                 compression=zipfile.ZIP_STORED)
RENPY = zip_bytes([("game/script.rpy", b'label start:\n    "Hello."\n    return\n'),
                   ("game/options.rpy", b'define config.name = "Tiny Story"\n'),
                   ("Tiny Story.py", b"# launcher\n"), ("Tiny Story.sh", b"#!/bin/sh\n"),
                   ("__MACOSX/._game", b"\0\5\26\7")])
ZIP64_SMALL = zip_bytes(rpg_maker("Wide Quest", "Wide Quest"), force_zip64=True)
SPACE_STORED = zip_bytes(rpg_maker("Space Quest", "Space Quest") +
                         [("Space Quest/Movie/noise.bin", os.urandom(8 * 1024 * 1024))],
                         compression=zipfile.ZIP_STORED)
CHANGED_BEFORE = zip_bytes(rpg_maker("Changed Quest", "Changed Quest") +
                          [("Changed Quest/Movie/noise.bin", bytes((i * 37) % 239 for i in range(2 * 1024 * 1024)))],
                          compression=zipfile.ZIP_STORED)
CHANGED_AFTER = zip_bytes(rpg_maker("Changed Quest", "Changed Quest") +
                         [("Changed Quest/Movie/noise.bin", bytes((i * 41) % 233 for i in range(2 * 1024 * 1024)))],
                         compression=zipfile.ZIP_STORED)
assert len(CHANGED_BEFORE) == len(CHANGED_AFTER) and CHANGED_BEFORE != CHANGED_AFTER
PERCENT_NAMES = {"trailing": "x%", "one-digit": "x%a", "alone": "%", "not-hex": "%zz"}
HTML = b"<!DOCTYPE html>\n<html><head><title>Download</title></head><body><a href='x.zip'>Download</a></body></html>"


def raw_zip(entries):
    """A Stored ZIP written by hand, for what zipfile won't write: each entry is (name, data) or a dict with name and
    data (bytes) and optionally local_name and local_sizes (what its local header says instead) or shares (the index
    of an earlier entry whose local header and data it points at)."""
    out, central = bytearray(), []
    for entry in entries:
        e = entry if isinstance(entry, dict) else {"name": entry[0].encode(), "data": entry[1]}
        name, data = e["name"], e["data"]
        directory = name.endswith(b"/")
        attr = (0o40755 << 16) | 0x10 if directory else 0o100644 << 16
        if "shares" in e:
            central.append((name, data, attr, central[e["shares"]][3]))
            continue
        local_name = e.get("local_name", name)
        packed, size = e.get("local_sizes", (len(data), len(data)))
        offset = len(out)
        out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 0, 0, 0x5B4A, zlib.crc32(data), packed, size,
                           len(local_name), 0) + local_name + data
        central.append((name, data, attr, offset))
    start = len(out)
    for name, data, attr, offset in central:
        out += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, (3 << 8) | 20, 20, 0, 0, 0, 0x5B4A, zlib.crc32(data),
                           len(data), len(data), len(name), 0, 0, 0, 0, attr, offset) + name
    out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(central), len(central), len(out) - start, start, 0)
    return bytes(out)


def unsafe_archives():
    def raw(name, data=b"evil", attr=0o100644, system=3):
        info = zipfile.ZipInfo(name, (2026, 11, 10, 12, 0, 0))
        info.create_system = system
        info.external_attr = attr << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        return info, data

    base = rpg_maker("Evil Quest", "Evil Quest")
    archives = {
        "traversal": base + [raw("Evil Quest/../../evil.txt")],
        "absolute": base + [raw("/tmp/dulcamara-links-evil.txt")],
        "backslash": base + [raw("Evil Quest\\..\\..\\evil.txt")],
        "symlink": base + [raw("Evil Quest/escape", b"../../..", attr=0o120777)],
        "ratio": base + [("Evil Quest/zeros.bin", bytes(300 * 1024 * 1024))],
        "long": base + [("Evil Quest/" + "/".join(["d" * 120] * 4) + "/x.txt", b"deep")],
        "duplicate": base + [("Evil Quest/same.txt", b"one"), ("Evil Quest/same.txt", b"two")],
        "fifo": base + [raw("Evil Quest/pipe", b"", attr=0o010644)],
        "forged": base + [raw("Evil Quest/a\nforged line\x1b[2J\x7f.txt")],
    }
    archives.update({
        "case": base + [("Evil Quest/Data.txt", b"one"), ("Evil Quest/data.txt", b"two")],
        "file-and-folder": base + [("Evil Quest/thing", b"a file"), ("Evil Quest/thing/inner.txt", b"inside")],
        "colon": base + [raw("Evil Quest/c:evil.txt")],
        "control": base + [raw("Evil Quest/bell\x07.txt")],
    })
    out = {name: zip_bytes(entries) for name, entries in archives.items()}
    # two entries pointing at one local header and its data
    out["overlap"] = raw_zip(base + [("Evil Quest/a.bin", b"shared bytes" * 100),
                                     {"name": b"Evil Quest/b.bin", "data": b"shared bytes" * 100, "shares": len(base)}])
    # a NUL inside a name (zipfile cuts names at a NUL, so written by hand)
    out["nul"] = raw_zip(base + [{"name": b"Evil Quest/nul\x00/../../../evil.txt", "data": b"evil"}])
    # a bomb by count: one entry over the limit of 65 536, each empty
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_STORED) as z:
        for i in range(65537):
            z.writestr(f"Evil Quest/f{i}", b"")
    out["count"] = buffer.getvalue()
    # an entry that declares 100 bytes and inflates to 1 MiB: its declared sizes (local and central) patched down
    data = bytearray(zip_bytes(rpg_maker("Evil Quest", "Evil Quest") + [("Evil Quest/liar.bin", b"a" * 1048576)]))
    name = b"Evil Quest/liar.bin"
    for match in re.finditer(re.escape(b"PK\x03\x04"), bytes(data)):
        at = match.start()
        if data[at + 30:at + 30 + len(name)] == name:
            struct.pack_into("<I", data, at + 22, 100)
    for match in re.finditer(re.escape(b"PK\x01\x02"), bytes(data)):
        at = match.start()
        if data[at + 46:at + 46 + len(name)] == name:
            struct.pack_into("<I", data, at + 24, 100)
    out["liar"] = bytes(data)
    return out


UNSAFE_ARCHIVES = unsafe_archives()
UNSAFE_REASONS = {"case": "differ only in case", "file-and-folder": "both a file and a folder", "colon": "colon",
                  "control": "control character", "nul": "control character", "overlap": "share their data"}
# a local header whose name and sizes disagree with the central directory's: the central directory decides
LOCAL_MISMATCH = raw_zip([(n, d) for n, d in rpg_maker("Local Quest", "Local Quest")] +
                         [{"name": b"Local Quest/real.txt", "data": b"the real data",
                           "local_name": b"Local Quest/../../../local-name.txt", "local_sizes": (3, 999999)}])


def write_huge_zip64(path, member_size):
    """A ZIP64 archive over 4 GiB written sparse: a small entry, a stored member of member_size zeros (a hole in the
    file), and the game's other entries after the 4 GiB mark, so their offsets need ZIP64 too."""
    crc_zero = 0
    block = bytes(64 * 1024 * 1024)
    left = member_size
    while left:
        n = min(left, len(block))
        crc_zero = zlib.crc32(block[:n] if n < len(block) else block, crc_zero)
        left -= n
    entries = [("Huge Quest/RPG_RT.ldb", b"LcfDataBase" + bytes(100), None),
               ("Huge Quest/Movie/zeros.bin", None, member_size),
               ("Huge Quest/RPG_RT.lmt", b"LcfMapTree" + bytes(40), None),
               ("Huge Quest/RPG_RT.ini", b"[RPG_RT]\r\nGameTitle=Huge Quest\r\n", None)]
    central = []
    with open(path, "wb") as f:
        for name, data, size in entries:
            offset = f.tell()
            raw_name = name.encode()
            size = len(data) if data is not None else size
            crc = zlib.crc32(data) if data is not None else crc_zero
            extra = struct.pack("<HHQQ", 1, 16, size, size)
            f.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 45, 0, 0, 0, 0x5B4A, crc, 0xFFFFFFFF, 0xFFFFFFFF,
                                len(raw_name), len(extra)) + raw_name + extra)
            if data is not None:
                f.write(data)
            else:
                f.seek(size, os.SEEK_CUR)  # the hole: zeros nobody writes
            central.append((raw_name, crc, size, offset))
        cd_start = f.tell()
        for raw_name, crc, size, offset in central:
            extra = struct.pack("<HHQQQ", 1, 24, size, size, offset)
            f.write(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, (3 << 8) | 45, 45, 0, 0, 0, 0x5B4A, crc,
                                0xFFFFFFFF, 0xFFFFFFFF, len(raw_name), len(extra), 0, 0, 0, 0o100644 << 16,
                                0xFFFFFFFF) + raw_name + extra)
        cd_end = f.tell()
        f.write(struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, (3 << 8) | 45, 45, 0, 0, len(central), len(central),
                            cd_end - cd_start, cd_start))
        f.write(struct.pack("<IIQI", 0x07064B50, 0, cd_end, 1))
        f.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    requests = {}

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError):
            pass  # the client hangs up on purpose (cancel, a page refused, a range ignored)

    def log_message(self, *arguments):
        pass

    def send(self, code, body, content_type="application/zip", extra=(), length=True):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        if length:
            self.send_header("Content-Length", str(len(body)))
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (ConnectionResetError, BrokenPipeError):
            pass

    def redirect(self, code, where):
        self.send_response(code)
        self.send_header("Location", where)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def range_start(self):
        match = re.fullmatch(r"bytes=(\d+)-", self.headers.get("Range", ""))
        return int(match.group(1)) if match else None

    def drop_halfway(self, body):
        self.send_response(200)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body[:len(body) // 2])
        self.wfile.flush()
        self.close_connection = True
        self.connection.shutdown(2)

    def do_GET(self):
        path = self.path
        Handler.requests.setdefault(path.split("?")[0], []).append(self.headers.get("Range", ""))
        if path == "/rpgmaker/pocket-quest.zip":
            self.send(200, POCKET)
        elif path.startswith("/renpy/download"):
            self.send(200, RENPY, "application/octet-stream",
                      [("Content-Disposition", 'attachment; filename="Tiny-Story-1.0.zip"')])
        elif path == "/named":
            name = urllib.parse.quote("../secret:" + "\u00e9" * 60 + ".zip")
            self.send(200, RENPY, "application/zip", [("Content-Disposition", f"attachment; filename*=UTF-8''{name}")])
        elif path == "/zip64/wide.zip":
            self.send(200, ZIP64_SMALL)
        elif path == "/r1":
            self.redirect(302, "/r2")
        elif path == "/r2":
            self.redirect(301, f"http://127.0.0.1:{self.server.server_address[1]}/r3")
        elif path == "/r3":
            self.redirect(307, "/files/redirect-quest.zip")
        elif path == "/files/redirect-quest.zip":
            self.send(200, REDIRECTED)
        elif path == "/loop":
            self.redirect(302, "/loop")
        elif path == "/flaky/game.zip":
            start = self.range_start()
            if start is None:
                self.send_response(200)
                self.send_header("Content-Type", "application/zip")
                self.send_header("Content-Length", str(len(FLAKY)))
                self.end_headers()
                self.wfile.write(FLAKY[:len(FLAKY) // 2])
                self.wfile.flush()
                self.close_connection = True
                self.connection.shutdown(2)
            else:
                self.send(206, FLAKY[start:], extra=[("Content-Range", f"bytes {start}-{len(FLAKY) - 1}/{len(FLAKY)}")])
        elif path == "/changed/game.zip":
            # the file changes on the server (same length) between the dropped try and the resumed one
            if len(Handler.requests[path]) == 1:
                self.drop_halfway(CHANGED_BEFORE)
            elif self.range_start() is not None:
                start = self.range_start()
                self.send(206, CHANGED_AFTER[start:], extra=[
                    ("Content-Range", f"bytes {start}-{len(CHANGED_AFTER) - 1}/{len(CHANGED_AFTER)}")])
            else:
                self.send(200, CHANGED_AFTER)
        elif path.startswith("/range/"):
            # the resumed try answers 206 with a Content-Range that does not go on from the requested offset
            kind = path.split("/")[2]
            start, size = self.range_start(), len(FLAKY)
            if len(Handler.requests[path]) == 1:
                self.drop_halfway(FLAKY)
            elif start is None:
                self.send(200, FLAKY)
            else:
                content_range = {"offset": f"bytes {start + 1}-{size - 1}/{size}", "unknown": f"bytes */{size}",
                                 "no-total": f"bytes {start}-{size - 1}/*",
                                 "total": f"bytes {start}-{size - 1}/{size + 1}"}[kind]
                self.send(206, FLAKY[start:], extra=[("Content-Range", content_range)])
        elif path == "/norange/game.zip":
            # never a range: the first try drops halfway, then whatever the Range, the whole file
            if len(Handler.requests[path]) == 1:
                self.send_response(200)
                self.send_header("Content-Type", "application/zip")
                self.send_header("Content-Length", str(len(NORANGE)))
                self.end_headers()
                self.wfile.write(NORANGE[:len(NORANGE) // 2])
                self.wfile.flush()
                self.close_connection = True
                self.connection.shutdown(2)
            else:
                self.send(200, NORANGE)
        elif path == "/slow/game.zip":
            start = self.range_start() or 0
            body = SLOW[start:]
            self.send_response(206 if start else 200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Length", str(len(body)))
            if start:
                self.send_header("Content-Range", f"bytes {start}-{len(SLOW) - 1}/{len(SLOW)}")
            self.end_headers()
            try:
                for at in range(0, len(body), 32 * 1024):
                    self.wfile.write(body[at:at + 32 * 1024])
                    self.wfile.flush()
                    time.sleep(0.05)
            except (ConnectionResetError, BrokenPipeError):
                pass
        elif path == "/space/known.zip":
            self.send(200, b"PK\x03\x04" + bytes(80 * 1024 * 1024))
        elif path == "/space/chunked.zip":
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            try:
                chunk = b"PK\x03\x04" + os.urandom(1024 * 1024 - 4)
                for _ in range(100):
                    self.wfile.write(b"%x\r\n" % len(chunk) + chunk + b"\r\n")
                    chunk = os.urandom(1024 * 1024)
                self.wfile.write(b"0\r\n\r\n")
            except (ConnectionResetError, BrokenPipeError):
                pass
        elif path == "/space/stored.zip":
            self.send(200, SPACE_STORED)
        elif path == "/page":
            self.send(200, HTML, "text/html; charset=utf-8")
        elif path == "/page-named.zip":
            self.send(200, HTML, "application/zip")
        elif path == "/archive.rar":
            self.send(200, b"Rar!\x1a\x07\x01\x00" + os.urandom(4096), "application/octet-stream")
        elif path == "/archive.7z":
            self.send(200, b"7z\xbc\xaf\x27\x1c\x00\x04" + os.urandom(4096), "application/octet-stream")
        elif path == "/archive.tar.gz":
            self.send(200, b"\x1f\x8b\x08\x00" + os.urandom(4096), "application/gzip")
        elif path == "/setup.exe":
            self.send(200, b"MZ\x90\x00" + os.urandom(4096), "application/octet-stream")
        elif path == "/stall/game.zip":
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Length", "1000000")
            self.end_headers()
            self.wfile.flush()
            time.sleep(STALL_SECONDS * 4)
        elif path == "/forbidden.zip":
            self.send(403, b"Forbidden", "text/plain")
        elif path.startswith("/unsafe/"):
            name = path[len("/unsafe/"):-len(".zip")]
            self.send(200, UNSAFE_ARCHIVES[name])
        elif path == "/local-mismatch.zip":
            self.send(200, LOCAL_MISMATCH)
        elif path == "/to-file":
            self.redirect(302, "file:///etc/passwd")
        elif path == "/to-ftp":
            self.redirect(302, f"ftp://127.0.0.1:{self.server.server_address[1]}/game.zip")
        elif path.startswith("/percent-named/"):
            # a filename*= that ends in "%", "%a"...: the header padded so the name's last byte is the 1023rd of
            # the line, where a decoder that steps past the terminator reads beyond its 1024-byte copy
            tail = "; filename*=UTF-8''" + PERCENT_NAMES[path[len("/percent-named/"):]]
            head = "attachment; pad="
            value = head + "p" * (1023 - len("Content-Disposition: ") - len(head) - len(tail)) + tail
            self.send(200, RENPY, "application/zip", [("Content-Disposition", value)])
        elif path.startswith("/percent-path/"):
            self.send(200, RENPY, "application/zip")
        else:
            self.send(404, b"Not Found", "text/plain")


def tree(path):
    """Every file and folder below path, with each file's SHA-256."""
    out = {}
    for base, folders, files in os.walk(path):
        for name in folders:
            out[os.path.relpath(os.path.join(base, name), path) + "/"] = None
        for name in files:
            full = os.path.join(base, name)
            if os.path.islink(full):
                out[os.path.relpath(full, path)] = "link"
            else:
                out[os.path.relpath(full, path)] = hashlib.sha256(Path(full).read_bytes()).hexdigest()
    return out


class GameLinksTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = os.environ.get("CC", "clang")
        if not shutil.which(cc):
            raise unittest.SkipTest(f"needs {cc}")
        cls.directory = tempfile.TemporaryDirectory()
        work = Path(cls.directory.name)
        cls.executable = work / "game-links-check"
        miniz = work / "miniz.o"
        subprocess.run([cc, "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-w", "-c",
                        str(ROOT / "src/vendor.c"), "-o", str(miniz)], check=True)
        libs = ["-liconv"] if platform.system() == "Darwin" else []
        build = subprocess.run([cc, *FLAGS, "-DNP_HAVE_CURL", f"-DLINKS_STALL_SECONDS={STALL_SECONDS}",
                                "-I" + str(ROOT / "src"), "-I" + str(ROOT / LIBRETRO),
                                *[str(ROOT / s) for s in SOURCES], str(miniz), "-lcurl", "-lpthread", *libs,
                                "-o", str(cls.executable)], capture_output=True, text=True, check=False)
        if build.returncode != 0 and "curl/curl.h" in build.stderr:
            cls.directory.cleanup()
            raise unittest.SkipTest("needs libcurl's headers (libcurl4-openssl-dev)")
        if build.returncode != 0:
            cls.directory.cleanup()
            raise AssertionError(build.stderr)
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server.daemon_threads = True
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.port = cls.server.server_address[1]
        cls.base = f"http://127.0.0.1:{cls.port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.directory.cleanup()

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(dir=self.directory.name))
        self.games = Path(os.path.realpath(self.work)) / "games"
        self.games.mkdir()
        Handler.requests.clear()

    def tearDown(self):
        shutil.rmtree(self.work, ignore_errors=True)

    # ---------------------------------------------------------------- helpers

    def run_check(self, *arguments, timeout=180):
        result = subprocess.run([str(self.executable), *map(str, arguments)], capture_output=True, text=True,
                                errors="replace", timeout=timeout, check=False)
        return result

    def parse(self, result):
        line = [l for l in result.stdout.splitlines() if l.startswith("result ")]
        self.assertTrue(line, result.stdout + result.stderr[-4000:])
        m = re.match(r"result (\d+) folder (.*?) progress (\d+)/(\d+) calls (\d+) message (.*)$", line[-1])
        self.assertTrue(m, line[-1])
        self.assertNotIn("runtime error", result.stderr)
        self.assertNotIn("AddressSanitizer", result.stderr)
        return {"error": int(m.group(1)), "folder": m.group(2), "done": int(m.group(3)), "total": int(m.group(4)),
                "calls": int(m.group(5)), "message": m.group(6), "out": result.stdout + result.stderr[-4000:]}

    def add(self, url, *options, games=None, timeout=180):
        return self.parse(self.run_check("add", games or self.games, url, *options, timeout=timeout))

    def scan(self, folder=None, games=None):
        result = self.run_check("scan", games or self.games, *([folder] if folder else []))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        games_found = {}
        for line in result.stdout.splitlines():
            if line.startswith("game "):
                location, title, _ = line[5:].split("\t")
                games_found[location] = title
        return games_found, result.stdout

    def assert_no_staging(self, games=None):
        staging = (games or self.games) / ".dulcamara-links"
        self.assertFalse(staging.exists(), sorted(tree(staging)) if staging.exists() else "")

    def assert_nothing_installed(self, before=None, games=None):
        games = games or self.games
        self.assertEqual({k: v for k, v in tree(games).items() if not k.startswith(".dulcamara-links")},
                         before or {})

    # ---------------------------------------------------------------- downloads that work

    def test_bundled_shelf(self):
        asset = ROOT / "assets/itch-shelf.zip"
        expected = self.work / "rebuilt.zip"
        subprocess.run([sys.executable, str(ROOT / "addons/itch-shelf/Dulcamara itch.io Shelf/package.py"),
                        str(expected)], check=True)
        self.assertEqual(asset.read_bytes(), expected.read_bytes())
        legacy = self.games / "Dulcamara itch.io Shelf"
        legacy.mkdir()
        sentinel = legacy / "keep.txt"
        sentinel.write_text("user copy")
        folder = self.games / "Dulcamara itch.io Shelf 1.1"
        damaged = self.work / "damaged.zip"
        damaged.write_bytes(b"interrupted")
        for source, flags in [(damaged, []), (asset, ["busy"])]:
            result = self.run_check("shelf", self.games, source, *flags)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertFalse(folder.exists())
        result = self.run_check("shelf", self.games, asset)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(str(folder), self.scan()[0])
        (folder / "marker").write_text("keep")
        result = self.run_check("shelf", self.games, asset)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((folder / "marker").read_text(), "keep")
        self.assertEqual(set(self.games.iterdir()), {folder, legacy})
        self.assertEqual(sentinel.read_text(), "user copy")

    def test_rpg_maker_game(self):
        r = self.add(self.base + "/rpgmaker/pocket-quest.zip")
        self.assertEqual(r["error"], OK, r["out"])
        self.assertEqual(r["folder"], str(self.games / "Pocket Quest"))
        self.assertEqual(r["done"], len(POCKET), r["out"])
        self.assertEqual((self.games / "Pocket Quest/Music/theme.txt").read_bytes(), b"music " * 2000)
        self.assert_no_staging()
        found, out = self.scan(r["folder"])
        self.assertEqual(found, {str(self.games / "Pocket Quest"): "Pocket Quest"}, out)
        self.assertIn("found Pocket Quest", out)

    def test_renpy_game_named_by_the_server(self):
        r = self.add(self.base + "/renpy/download?id=7&token=secret")
        self.assertEqual(r["error"], OK, r["out"])
        folder = self.games / "Tiny-Story-1.0"
        self.assertEqual(r["folder"], str(folder))
        self.assertTrue((folder / "game/script.rpy").is_file())
        self.assertFalse((folder / "__MACOSX").exists())
        self.assertNotIn("secret", r["out"])  # the link's query is not logged
        found, out = self.scan(r["folder"])
        self.assertEqual(found, {str(folder): "Tiny Story"}, out)

    def test_folder_name_from_a_hostile_file_name(self):
        r = self.add(self.base + "/named")
        self.assertEqual(r["error"], OK, r["out"])
        folder = Path(r["folder"])
        self.assertEqual(folder.parent, self.games)
        name = folder.name.encode()
        self.assertLessEqual(len(name), 96)
        name.decode("utf-8")  # cut on a whole character
        self.assertTrue(folder.name.startswith("_secret_\u00e9"), folder.name)
        self.assertTrue((folder / "game/script.rpy").is_file())

    def test_credentials_in_a_link_are_never_shown(self):
        def checked(*arguments):
            result = self.run_check(*arguments)
            self.assertNotIn("secret", result.stdout + result.stderr)  # neither the log nor any message
            return result

        works = f"http://user:secret@127.0.0.1:{self.port}/rpgmaker/pocket-quest.zip"
        r = self.parse(checked("add", self.games, works))
        self.assertEqual(r["error"], OK, r["out"])
        self.assertEqual(r["folder"], str(self.games / "Pocket Quest"))
        self.assertIn("127.0.0.1", r["out"])  # the link is still logged, without its user and password
        probe = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        closed = probe.server_address[1]
        probe.server_close()
        r = self.parse(checked("add", self.games, f"http://user:secret@127.0.0.1:{closed}/game.zip"))
        self.assertEqual(r["error"], CONNECT, r["out"])
        self.assertIn(f"127.0.0.1:{closed}", r["message"])
        r = self.parse(checked("add", self.games, f"https://user:secret@127.0.0.1:{self.port}/x.zip"))
        self.assertEqual(r["error"], TLS, r["out"])
        self.assertIn(f"127.0.0.1:{self.port}", r["message"])
        # the queue's results and its done file; a recorded line is still found again
        shutil.rmtree(self.games / "Pocket Quest")
        done = self.work / "links-done.txt"
        missing = f"http://user:secret@127.0.0.1:{self.port}/missing.zip"
        result = checked("queue", self.games, done, works, missing)
        self.assertIn("added 1 failed 1", result.stdout)
        self.assertNotIn("secret", done.read_text())
        links = self.work / "links.txt"
        links.write_text(f"{works}\n{missing}\n")
        result = checked("pending", links, done)
        self.assertIn("pending 0 ", result.stdout)
        self.assert_no_staging()

    def test_percent_decoding_stays_inside_the_name(self):
        # an unfinished %XY is copied as it is, and nothing past the name's end is read (ASan would report it)
        for key, name in PERCENT_NAMES.items():
            with self.subTest(source="Content-Disposition", name=name):
                r = self.add(f"{self.base}/percent-named/{key}")
                self.assertEqual(r["error"], OK, r["out"])
                self.assertEqual(r["folder"], str(self.games / name), r["out"])
                shutil.rmtree(r["folder"])
            with self.subTest(source="URL", name=name):
                r = self.add(f"{self.base}/percent-path/{name}")
                self.assertEqual(r["error"], OK, r["out"])
                self.assertEqual(r["folder"], str(self.games / name), r["out"])
                shutil.rmtree(r["folder"])
        self.assert_no_staging()

    def test_small_zip64(self):
        r = self.add(self.base + "/zip64/wide.zip")
        self.assertEqual(r["error"], OK, r["out"])
        found, out = self.scan()
        self.assertEqual(found, {str(self.games / "Wide Quest"): "Wide Quest"}, out)

    def test_redirects(self):
        r = self.add(self.base + "/r1")
        self.assertEqual(r["error"], OK, r["out"])
        self.assertEqual(r["folder"], str(self.games / "Redirect Quest"))
        r = self.add(self.base + "/loop")
        self.assertEqual(r["error"], HTTP, r["out"])
        self.assertIn("redirect", r["message"].lower())
        self.assertEqual(len(Handler.requests["/loop"]), 11)
        self.assert_no_staging()

    def test_dropped_transfer_resumes(self):
        r = self.add(self.base + "/flaky/game.zip")
        self.assertEqual(r["error"], OK, r["out"])
        self.assertEqual(Handler.requests["/flaky/game.zip"], ["", f"bytes={len(FLAKY) // 2}-"])
        self.assertEqual(r["done"], len(FLAKY))
        self.assertEqual(r["total"], len(FLAKY))
        self.assertEqual(len((self.games / "Flaky Quest/Movie/noise.bin").read_bytes()), 3 * 1024 * 1024)

    def test_server_without_ranges_starts_over(self):
        r = self.add(self.base + "/norange/game.zip")
        self.assertEqual(r["error"], OK, r["out"])
        requests = Handler.requests["/norange/game.zip"]
        self.assertEqual(requests[:2], ["", f"bytes={len(NORANGE) // 2}-"])
        self.assertEqual(requests[-1], "")  # started over
        self.assertIn("from the start", r["out"])
        self.assertEqual((self.games / "Plain Quest/Movie/noise.bin").read_bytes(),
                         bytes((i * 31) % 253 for i in range(2 * 1024 * 1024)))

    def test_a_file_changed_while_resuming_is_tried_again(self):
        url = self.base + "/changed/game.zip"
        done = self.work / "links-done.txt"
        result = self.run_check("queue", self.games, done, url)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        self.assertEqual(Handler.requests["/changed/game.zip"], ["", f"bytes={len(CHANGED_BEFORE) // 2}-"])
        self.assertIn("added 0 failed 1", result.stdout)
        line = re.search(r"^link 0 result (\d+) folder - message (.*)$", result.stdout, re.M)
        self.assertTrue(line, result.stdout)
        self.assertEqual(int(line.group(1)), NETWORK, result.stdout)  # a passing failure, not DAMAGED
        self.assertIn("damaged", line.group(2))
        self.assertIn("tried again from the start", line.group(2))
        self.assertFalse(done.exists() and url in done.read_text())  # the line stays pending
        self.assert_no_staging()  # the partial file is gone
        self.assert_nothing_installed()
        r = self.add(url)  # the next try starts from zero, and works
        self.assertEqual(r["error"], OK, r["out"])
        self.assertEqual(Handler.requests["/changed/game.zip"][-1], "")
        self.assertEqual((self.games / "Changed Quest/Movie/noise.bin").read_bytes(),
                         bytes((i * 41) % 233 for i in range(2 * 1024 * 1024)))

    def test_a_content_range_that_does_not_go_on_starts_over(self):
        half = f"bytes={len(FLAKY) // 2}-"
        for kind in ["offset", "unknown", "no-total", "total"]:
            with self.subTest(kind=kind):
                r = self.add(f"{self.base}/range/{kind}/game.zip")
                self.assertEqual(r["error"], OK, r["out"])
                self.assertEqual(Handler.requests[f"/range/{kind}/game.zip"], ["", half, ""])  # started over
                self.assertEqual(len((self.games / "Flaky Quest/Movie/noise.bin").read_bytes()), 3 * 1024 * 1024)
                shutil.rmtree(self.games / "Flaky Quest")
                self.assert_no_staging()

    # ---------------------------------------------------------------- downloads that can't be games

    def test_not_a_zip(self):
        cases = {"/page": (WEB_PAGE, "web page"), "/page-named.zip": (WEB_PAGE, "web page"),
                 "/archive.rar": (FORMAT, "RAR"), "/archive.7z": (FORMAT, "7-Zip"),
                 "/archive.tar.gz": (FORMAT, "gzip"), "/setup.exe": (FORMAT, "Windows program")}
        for path, (error, words) in cases.items():
            with self.subTest(path=path):
                r = self.add(self.base + path)
                self.assertEqual(r["error"], error, r["out"])
                self.assertIn(words, r["message"])
                self.assertLess(r["done"], 64 * 1024, r["out"])  # stopped at once
                self.assert_no_staging()
                self.assert_nothing_installed()

    def test_server_errors(self):
        r = self.add(self.base + "/missing.zip")
        self.assertEqual(r["error"], HTTP, r["out"])
        self.assertIn("404", r["message"])
        self.assertIn("not found", r["message"].lower())
        r = self.add(self.base + "/forbidden.zip")
        self.assertEqual(r["error"], HTTP, r["out"])
        self.assertIn("403", r["message"])
        self.assert_no_staging()
        # a port nobody listens on
        probe = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        closed = probe.server_address[1]
        probe.server_close()
        r = self.add(f"http://127.0.0.1:{closed}/game.zip")
        self.assertEqual(r["error"], CONNECT, r["out"])
        # TLS spoken to a plain HTTP server
        r = self.add(f"https://127.0.0.1:{self.port}/rpgmaker/pocket-quest.zip")
        self.assertEqual(r["error"], TLS, r["out"])
        self.assertIn("TLS", r["message"])
        for bad in ["ftp://127.0.0.1/game.zip", "file:///etc/passwd", "https://", "http://exa mple.com/a.zip",
                    "not a link"]:
            r = self.add(bad)
            self.assertEqual(r["error"], BAD_URL, bad + r["out"])
        self.assert_no_staging()
        self.assert_nothing_installed()

    def test_stall(self):
        started = time.monotonic()
        r = self.add(self.base + "/stall/game.zip")
        took = time.monotonic() - started
        self.assertEqual(r["error"], STALLED, r["out"])
        self.assertLess(took, STALL_SECONDS * 3 + 10, r["out"])
        self.assertIn("stalled", r["message"])
        self.assert_nothing_installed()

    # ---------------------------------------------------------------- unsafe archives

    def test_unsafe_archives(self):
        outside = self.work / "outside"
        outside.mkdir()
        games = outside / "deep" / "games"  # so ../../ lands in outside/, which must stay empty
        games.mkdir(parents=True)
        for name in UNSAFE_ARCHIVES:
            with self.subTest(archive=name):
                r = self.add(f"{self.base}/unsafe/{name}.zip", games=games, timeout=300)
                self.assertEqual(r["error"], UNSAFE, name + r["out"])
                self.assertTrue(r["message"], r["out"])
                self.assertEqual(sorted(tree(outside)), ["deep/", "deep/games/"], name)
                self.assertFalse(Path("/tmp/dulcamara-links-evil.txt").exists())
        self.assert_no_staging(games)

    def test_each_unsafe_shape_is_refused_for_its_reason(self):
        outside = self.work / "outside"
        games = outside / "deep" / "games"
        games.mkdir(parents=True)
        for name, words in UNSAFE_REASONS.items():
            with self.subTest(archive=name):
                r = self.add(f"{self.base}/unsafe/{name}.zip", games=games)
                self.assertEqual(r["error"], UNSAFE, name + r["out"])
                self.assertIn(words, r["message"])
                self.assertEqual(sorted(tree(outside)), ["deep/", "deep/games/"], name)
        self.assert_no_staging(games)

    def test_the_central_directory_decides_what_is_written(self):
        outside = self.work / "outside"
        games = outside / "deep" / "games"
        games.mkdir(parents=True)
        r = self.add(f"{self.base}/local-mismatch.zip", games=games)
        self.assertEqual(r["error"], OK, r["out"])
        expected = {"deep/", "deep/games/", "deep/games/Local Quest/"}
        for entry, _ in rpg_maker("Local Quest", "Local Quest")[1:]:
            expected.add("deep/games/" + entry)
            parent = entry.rsplit("/", 1)[0] + "/"
            expected.add("deep/games/" + parent)
        expected.add("deep/games/Local Quest/real.txt")
        self.assertEqual(set(tree(outside)), expected)
        self.assertEqual((games / "Local Quest/real.txt").read_bytes(), b"the real data")
        self.assert_no_staging(games)

    def test_redirect_to_another_scheme_is_refused(self):
        for path in ["/to-file", "/to-ftp"]:
            with self.subTest(path=path):
                r = self.add(self.base + path)
                self.assertEqual(r["error"], BAD_URL, r["out"])
                self.assertIn("other than http", r["message"])
                self.assert_no_staging()
                self.assert_nothing_installed()

    def test_a_refused_name_cannot_forge_a_log_line(self):
        result = self.run_check("add", self.games, f"{self.base}/unsafe/forged.zip")
        r = self.parse(result)
        self.assertEqual(r["error"], UNSAFE, r["out"])
        self.assertIn("control character", r["message"])
        everything = result.stdout + result.stderr
        for byte in "\x1b\x7f":
            self.assertNotIn(byte, everything)
        # the name stays inside the one line that refuses it, in the message and in the log
        forged = [l for l in everything.splitlines() if "forged line" in l]
        self.assertEqual(len(forged), 2, everything)
        for line in forged:
            self.assertIn("Refused for safety", line)
        self.assert_no_staging()
        self.assert_nothing_installed()

    def test_huge_zip64_over_4_gib(self):
        if shutil.disk_usage(self.work).free < 6 << 30:
            self.skipTest("needs 6 GiB free for the free-space check of a 4 GiB member (it is written sparse)")
        archive = self.work / "huge.zip"
        size = (4 << 30) + (1 << 20)
        write_huge_zip64(archive, size)
        self.assertGreater(archive.stat().st_size, 4 << 30)
        r = self.parse(self.run_check("unpack", self.games, archive, "huge", timeout=600))
        self.assertEqual(r["error"], OK, r["out"])
        zeros = self.games / "Huge Quest/Movie/zeros.bin"
        self.assertEqual(zeros.stat().st_size, size)
        self.assertLess(zeros.stat().st_blocks * 512, 1 << 30, "the zeros were written, not skipped")
        self.assertEqual((self.games / "Huge Quest/RPG_RT.ini").read_bytes(), b"[RPG_RT]\r\nGameTitle=Huge Quest\r\n")
        found, out = self.scan()
        self.assertEqual(found, {str(self.games / "Huge Quest"): "Huge Quest"}, out)
        archive.unlink()

    # ---------------------------------------------------------------- an existing game

    def test_name_collision_leaves_the_existing_game(self):
        existing = self.games / "Pocket Quest"
        (existing / "Save").mkdir(parents=True)
        (existing / "RPG_RT.ldb").write_bytes(b"my own database")
        (existing / "RPG_RT.lmt").write_bytes(b"my own map tree")
        (existing / "Save01.lsd").write_bytes(b"precious save")
        (self.games / "Pocket Quest (2)").mkdir()  # even an empty folder is not merged into
        before = tree(self.games)
        r = self.add(self.base + "/rpgmaker/pocket-quest.zip")
        self.assertEqual(r["error"], OK, r["out"])
        self.assertEqual(r["folder"], str(self.games / "Pocket Quest (3)"))
        after = tree(self.games)
        for path, digest in before.items():
            self.assertEqual(after.get(path, "missing"), digest, path)
        self.assertEqual(os.listdir(self.games / "Pocket Quest (2)"), [])

    # ---------------------------------------------------------------- a full disk

    def mounted_volume(self):
        if platform.system() != "Darwin" or not shutil.which("hdiutil"):
            self.skipTest("a small volume for the full-disk checks needs hdiutil (macOS)")
        image = self.work / "small.dmg"
        mount = self.work / "small"
        mount.mkdir()
        made = subprocess.run(["hdiutil", "create", "-size", "64m", "-fs", "HFS+", "-volname", "dlinks", "-layout",
                               "NONE", str(image)], capture_output=True, text=True)
        attached = made.returncode == 0 and subprocess.run(
            ["hdiutil", "attach", "-nobrowse", "-mountpoint", str(mount), str(image)], capture_output=True).returncode == 0
        if not attached:
            self.skipTest("hdiutil could not make or attach a 64 MiB image: " + made.stderr)
        self.addCleanup(subprocess.run, ["hdiutil", "detach", "-force", str(mount)], capture_output=True)
        games = Path(os.path.realpath(mount)) / "games"
        games.mkdir()
        return games

    def test_disk_full(self):
        games = self.mounted_volume()
        # the size is known first: refused before anything is written
        r = self.add(self.base + "/space/known.zip", games=games)
        self.assertEqual(r["error"], NO_SPACE, r["out"])
        self.assertIn("space", r["message"])
        self.assert_no_staging(games)
        # the size is never said: the disk fills while downloading
        r = self.add(self.base + "/space/chunked.zip", games=games)
        self.assertEqual(r["error"], NO_SPACE, r["out"])
        self.assertGreater(r["done"], 4 << 20, r["out"])
        self.assert_no_staging(games)
        self.assert_nothing_installed(games=games)
        # the disk fills (another writer) as the unpacking starts
        filler = games.parent / "filler.bin"
        r = self.add(self.base + "/space/stored.zip", f"fill={UNPACK}:1:{filler}", games=games)
        self.assertEqual(r["error"], NO_SPACE, r["out"])
        self.assert_no_staging(games)
        self.assert_nothing_installed(games=games)
        filler.unlink()
        r = self.add(self.base + "/space/stored.zip", games=games)
        self.assertEqual(r["error"], OK, r["out"])  # and with the space back, it fits

    # ---------------------------------------------------------------- kills, cancels and a running game

    def stop_and_kill(self, url, stage, call):
        process = subprocess.Popen([str(self.executable), "add", str(self.games), url, f"stop={stage}:{call}"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        line = process.stdout.readline()
        while line and not line.startswith("stopped"):
            line = process.stdout.readline()
        if not line.startswith("stopped"):  # it ended without stopping: its stderr says why
            self.fail(f"never reached {stage}:{call}: {process.stderr.read()[-3000:]}")
        os.kill(process.pid, signal.SIGKILL)
        process.wait(30)
        process.stdout.close()
        process.stderr.close()

    def test_killed_at_every_step(self):
        url = self.base + "/slow/game.zip"
        steps = [(DOWNLOAD, 1), (DOWNLOAD, 4), (CHECK, 1), (UNPACK, 1), (UNPACK, 3), (INSTALL, 1), (INSTALL, 2)]
        for stage, call in steps:
            with self.subTest(stage=stage, call=call):
                shutil.rmtree(self.games)
                self.games.mkdir()
                self.stop_and_kill(url, stage, call)
                self.assertTrue((self.games / ".dulcamara-links").exists())  # it was killed with work in progress
                result = self.run_check("cleanup", self.games)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assert_no_staging()
                found, out = self.scan()
                if (stage, call) == (INSTALL, 2):  # killed after the rename: the whole game, listed
                    self.assertEqual(found, {str(self.games / "Slow Quest"): "Slow Quest"}, out)
                    self.assertEqual(len((self.games / "Slow Quest/Movie/noise.bin").read_bytes()), 1024 * 1024)
                else:
                    self.assertEqual(tree(self.games), {}, out)
        # the cleanup does not follow a link put where the staging folder goes
        target = self.work / "elsewhere"
        target.mkdir()
        (target / "keep.txt").write_text("keep")
        os.symlink(target, self.games / ".dulcamara-links")
        self.assertEqual(self.run_check("cleanup", self.games).returncode, 0)
        self.assertEqual((target / "keep.txt").read_text(), "keep")
        self.assert_no_staging()

    def test_cancel_at_each_stage(self):
        url = self.base + "/slow/game.zip"
        for stage, call in [(DOWNLOAD, 3), (CHECK, 1), (UNPACK, 1), (UNPACK, 3), (INSTALL, 1)]:
            with self.subTest(stage=stage, call=call):
                r = self.add(url, f"cancel={stage}:{call}")
                self.assertEqual(r["error"], CANCELLED, r["out"])
                self.assert_no_staging()
                self.assert_nothing_installed()

    def test_refused_while_a_game_runs(self):
        r = self.add(self.base + "/rpgmaker/pocket-quest.zip", "busy")
        self.assertEqual(r["error"], BUSY, r["out"])
        self.assertIn("game", r["message"])
        self.assertEqual(tree(self.games), {})
        self.assertNotIn("/rpgmaker/pocket-quest.zip", Handler.requests)  # nothing was downloaded

    # ---------------------------------------------------------------- links.txt and the queue

    def test_links_file_and_queue(self):
        links = self.work / "links.txt"
        done = self.work / "links-done.txt"
        result = self.run_check("pending", links, done)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pending -1", result.stdout)
        pocket = self.base + "/rpgmaker/pocket-quest.zip"
        missing = self.base + "/missing.zip"
        renpy = self.base + "/renpy/download?id=7"
        links.write_text(f"# my games\n\n  {pocket}  \r\nftp://old.example/x.zip\nnot a link\n{missing}\n"
                         f"{pocket}\n{renpy}\n", encoding="utf-8")
        original = links.read_bytes()
        result = self.run_check("pending", links, done)
        self.assertIn("pending 3 invalid 2", result.stdout, result.stdout)
        self.assertEqual(re.findall(r"^url (.*)$", result.stdout, re.M), [pocket, missing, renpy])

        result = self.run_check("queue", self.games, done, pocket, missing)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        self.assertIn(f"queue state 2 added 1 failed 1 last {self.games / 'Pocket Quest'}", result.stdout)
        self.assertIn(f"link 0 result {OK} folder {self.games / 'Pocket Quest'}", result.stdout)
        self.assertIn(f"link 1 result {HTTP}", result.stdout)
        self.assertIn("after 0", result.stdout)
        self.assertEqual(done.read_text().splitlines(), [f"added\t{pocket}", f"failed\t{missing}"])
        result = self.run_check("pending", links, done)
        self.assertEqual(re.findall(r"^url (.*)$", result.stdout, re.M), [renpy])
        self.assertEqual(links.read_bytes(), original)  # the user's file is never rewritten
        # a passing failure (a refused connection) is not recorded, so the line is tried again
        probe = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        closed = f"http://127.0.0.1:{probe.server_address[1]}/game.zip"
        probe.server_close()
        result = self.run_check("queue", self.games, done, closed)
        self.assertIn("added 0 failed 1", result.stdout)
        self.assertEqual(len(done.read_text().splitlines()), 2)

    def test_done_file_is_never_followed_nor_emptied(self):
        url = self.base + "/rpgmaker/pocket-quest.zip"
        done = self.work / "links-done.txt"
        # a link planted where the done file goes: refused with a reason, its target untouched
        target = self.work / "precious.txt"
        target.write_bytes(b"precious\x00bytes\n")
        os.symlink(target, done)
        result = self.run_check("mark", done, url, "added")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("not a regular file", result.stderr)
        self.assertEqual(target.read_bytes(), b"precious\x00bytes\n")
        self.assertTrue(done.is_symlink())
        self.assertEqual(os.readlink(done), str(target))
        done.unlink()
        # a folder in its place: refused, left as it is
        done.mkdir()
        result = self.run_check("mark", done, url, "added")
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(done.is_dir())
        self.assertEqual(os.listdir(done), [])
        done.rmdir()
        # a done file that can't be read keeps its history
        history = b"added\thttp://example.com/a.zip\nfailed\thttp://example.com/b.zip\n"
        done.write_bytes(history)
        done.chmod(0)
        self.addCleanup(lambda: done.exists() and done.chmod(0o644))
        result = self.run_check("mark", done, url, "added")
        self.assertNotEqual(result.returncode, 0)
        done.chmod(0o644)
        self.assertEqual(done.read_bytes(), history)
        # and a readable one is added to
        result = self.run_check("mark", done, url, "added")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(done.read_bytes(), history + f"added\t{url}\n".encode())


if __name__ == "__main__":
    unittest.main()
