# SPDX-License-Identifier: GPL-3.0-or-later
"""Read itch.io's public catalogue and the anonymous download choices on a creator's page."""
import contextlib
import fcntl
import http.cookiejar
import json
import os
import re
import shutil
import ssl
import stat
import zipfile
from html.parser import HTMLParser
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import (HTTPCookieProcessor, HTTPRedirectHandler, HTTPSHandler,
                            ProxyHandler, Request, build_opener)
from xml.etree import ElementTree

FEED = "https://itch.io/games/free/made-with-renpy/platform-linux.xml"
MIB = 1024 * 1024
MAX_DOWNLOAD = 8 * 1024 * MIB
MAX_GAME = 12 * 1024 * MIB
RESERVE = 32 * MIB
CHUNK = 256 * 1024
SLUG = r"[a-z0-9](?:[a-z0-9-]{0,78}[a-z0-9])?"


class ShelfError(Exception):
    pass


def project_parts(url):
    match = re.fullmatch(r"https://(" + SLUG + r")\.itch\.io/(" + SLUG + r")/?", url)
    return match.groups() if match else None


def cdn(url):
    addr = urlsplit(url)
    if addr.scheme != "https" or addr.username or addr.password or addr.port is not None:
        return False
    host = addr.hostname or ""
    return (host.endswith((".itch.zone", ".hwcdn.net")) or
            re.fullmatch(r"itchio-mirror\.[0-9a-f]+\.r2\.cloudflarestorage\.com", host))


class Redirects(HTTPRedirectHandler):
    def __init__(self, allowed):
        self.allowed = allowed

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not self.allowed(newurl):
            raise ShelfError("The download points outside this store.")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class Session:
    def __init__(self, allowed):
        import certifi
        self.allowed = allowed
        trust = ssl.create_default_context(cafile=certifi.where())
        self.opener = build_opener(ProxyHandler({}), HTTPSHandler(context=trust),
                                   HTTPCookieProcessor(http.cookiejar.CookieJar()), Redirects(allowed))

    def open(self, url, data=None, referer=None):
        if not self.allowed(url):
            raise ShelfError("This address is outside the store.")
        headers = {"User-Agent": "Dulcamara-Itch-Shelf/1.1"}
        if referer:
            headers.update({"Referer": referer, "X-Requested-With": "XMLHttpRequest"})
        return self.opener.open(Request(url, data=data, headers=headers), timeout=12)

    def read(self, url, data=None, referer=None):
        with self.open(url, data, referer) as response:
            body = response.read(2 * MIB + 1)
        if len(body) > 2 * MIB:
            raise ShelfError("The store page is too large.")
        return body


def catalogue():
    raw = Session(lambda url: url == FEED).read(FEED)
    items = []
    for item in ElementTree.fromstring(raw).findall("./channel/item")[:60]:
        page = item.findtext("link", "").rstrip("/")
        parts = project_parts(page)
        title = (item.findtext("plainTitle") or item.findtext("title") or "").strip()
        if parts and title and all(page != row[1] for row in items):
            items.append((title[:120], page))
    if not items:
        raise ShelfError("itch.io returned no free Ren’Py listings. Try refreshing later.")
    return items


class StorePage(HTMLParser):
    def __init__(self, body):
        super().__init__()
        self.csrf = ""
        self.free_button = False
        self.upload = None
        self.upload_name = ""
        self.upload_depth = 0
        self.files = []
        self.feed(body.decode("utf-8"))
        self.close()

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        classes = (a.get("class") or "").split()
        if tag == "meta" and a.get("name") == "csrf_token":
            self.csrf = a.get("value", "")
        if tag == "a" and "direct_download_btn" in classes:
            self.free_button = True
        if tag == "div":
            if "upload" in classes:
                self.upload_depth = 1
                self.upload = None
                self.upload_name = ""
            elif self.upload_depth:
                self.upload_depth += 1
        if not self.upload_depth:
            return
        if tag == "a" and "download_btn" in classes:
            value = a.get("data-upload_id", "")
            self.upload = value if re.fullmatch(r"[0-9]{1,16}", value) else None
        if tag == "strong" and "name" in classes:
            self.upload_name = a.get("title") or ""
        if not self.upload or not 0 < len(self.upload_name) <= 180:
            return
        desktop = tag == "span" and ("icon-tux" in classes or "icon-windows8" in classes)
        name = self.upload_name.lower()
        if name.endswith(".zip") or (desktop and not name.endswith((".exe", ".msi", ".7z", ".rar", ".apk", ".tar.gz", ".dmg"))):
            self.files.append((self.upload_name, self.upload))
            self.upload = None

    def handle_endtag(self, tag):
        if tag == "div" and self.upload_depth:
            self.upload_depth -= 1


class Project:
    def __init__(self, url):
        parts = project_parts(url)
        if not parts:
            raise ShelfError("This game has no public itch.io project page.")
        self.page = url.rstrip("/")
        self.session = Session(lambda target: target == self.page or target.startswith(self.page + "/"))
        self.files = []
        self.download_page = ""
        self.token = ""

    def _post(self, url, page, token):
        if not token:
            raise ShelfError("itch.io’s download form is unavailable.")
        data = urlencode({"csrf_token": token}).encode("ascii")
        return json.loads(self.session.read(url, data, page))

    def uploads(self):
        purchase = self.page + "/purchase"
        try:
            form = StorePage(self.session.read(purchase))
        except HTTPError as exc:
            if exc.code != 404:
                raise
            form = None
        if form and form.free_button:
            answer = self._post(self.page + "/download_url", purchase, form.csrf)
            self.download_page = answer.get("url", "")
            if not self.download_page.startswith(self.page + "/download/"):
                raise ShelfError("This game requires a visit to the creator’s page.")
            page = StorePage(self.session.read(self.download_page))
        else:
            # Some creators put public file buttons directly on the game page.
            self.download_page = self.page
            page = StorePage(self.session.read(self.page))
        self.files = page.files
        self.token = page.csrf
        if not self.files:
            raise ShelfError("No free ZIP download is offered here. Check the creator’s page.")
        return self.files

    def archive(self, upload):
        if upload not in [item[1] for item in self.files]:
            raise ShelfError("Choose a file from this project first.")
        answer = self._post(self.page + "/file/" + upload + "?source=game_download",
                            self.download_page, self.token)
        if answer.get("external") or answer.get("lightbox") or not cdn(answer.get("url", "")):
            raise ShelfError("This file needs the creator’s website or an external download.")
        return Session(cdn).open(answer["url"])


def free_space(path, required):
    if shutil.disk_usage(path).free < required + RESERVE:
        raise ShelfError("The games drive needs more free space.")


def game_files(archive):
    """Keep the single game's data and engine-version markers. Never install PC launchers."""
    entries = archive.infolist()
    roots = set()
    for info in entries:
        parts = info.filename.split("/")
        if "__MACOSX" in parts or "game" not in parts:
            continue
        n = parts.index("game")
        if n <= 2 and info.filename.lower().endswith((".rpy", ".rpyc", ".rpa")):
            roots.add(tuple(parts[:n]))
    if len(roots) != 1:
        raise ShelfError("This ZIP must contain one Ren’Py game. Choose its PC or Linux build.")
    prefix = next(iter(roots))
    files, seen, total = [], set(), 0
    for info in entries:
        parts = tuple(info.filename.split("/"))
        if parts[:len(prefix)] != prefix or info.is_dir():
            continue
        rel = parts[len(prefix):]
        if not rel or not (rel[0] == "game" or rel in (("renpy", "vc_version.py"), ("renpy", "__init__.py"))):
            continue
        mode = stat.S_IFMT(info.external_attr >> 16)
        if (any(p in ("", ".", "..") or len(p.encode("utf-8")) > 255 or
                any(ord(c) < 32 or c in "\\:" for c in p) for p in rel)
                or mode not in (0, stat.S_IFREG) or info.flag_bits & 1
                or info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)):
            raise ShelfError("The game contains an unsupported archive entry.")
        name = "/".join(rel)
        total += info.file_size
        if (name.casefold() in seen or len(name.encode("utf-8")) > 430 or total > MAX_GAME):
            raise ShelfError("The ZIP has conflicting paths or exceeds the installation limit.")
        seen.add(name.casefold())
        files.append((info, name))
    return files, total


@contextlib.contextmanager
def staging(shelf_dir):
    # Only this add-on owns this location. A process exit releases the lock so the next run can reclaim its files.
    with (shelf_dir / ".install.lock").open("a+b") as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ShelfError("Another shelf download is running.") from None
        stage = shelf_dir / ".itch-install"
        if stage.is_symlink():
            raise ShelfError("The staging folder cannot be a shortcut.")
        if stage.is_dir():
            shutil.rmtree(stage)
        stage.mkdir(mode=0o700)
        try:
            yield stage
        finally:
            shutil.rmtree(stage)


def install(project, upload, shelf_dir):
    """Cooperative transfer. Closing the iterator cancels and removes all staged data."""
    shelf_dir = Path(shelf_dir).resolve()
    author, slug = project_parts(project.page)
    if not re.fullmatch(r"[0-9]{1,16}", upload):
        raise ShelfError("Choose a file from this project first.")
    base = shelf_dir.parent / (slug + " (" + author + ")")
    alternate = base.with_name(base.name + " [" + upload + "]")
    receipt = ".itch-upload-" + upload
    with staging(shelf_dir) as stage:
        dest = base
        if os.path.lexists(alternate):
            raise ShelfError("This download already has a library folder.")
        if os.path.lexists(base):
            if base.is_symlink() or not base.is_dir():
                raise ShelfError("This game's library path is already in use.")
            if (base / receipt).exists() or len(project.files) <= 1:
                raise ShelfError("This game already has a library folder.")
            dest = alternate
        archive_path = stage / "download.zip"
        with project.archive(upload) as response, archive_path.open("xb") as sink:
            length = int(response.headers.get("Content-Length") or 0)
            if length < 0 or length > MAX_DOWNLOAD:
                raise ShelfError("Choose a ZIP of 8 GiB or less.")
            free_space(stage, length)
            signature = response.read(4)
            if signature != b"PK\x03\x04":
                raise ShelfError("This download is not a game ZIP. Choose a PC or Linux ZIP.")
            sink.write(signature)
            done = len(signature)
            while True:
                chunk = response.read(CHUNK)
                if not chunk:
                    break
                done += len(chunk)
                if done > MAX_DOWNLOAD:
                    raise ShelfError("The download exceeds 8 GiB.")
                if not length and done // (16 * MIB) != (done - len(chunk)) // (16 * MIB):
                    free_space(stage, 16 * MIB)
                sink.write(chunk)
                yield "Downloading", done, length
            if length and length != done:
                raise ShelfError("The download was interrupted. Try again.")
        with archive_path.open("rb") as raw:
            # Bound memory before ZipFile reads and builds an object for every central-directory member.
            end = zipfile._EndRecData(raw)
            if not end or end[4] > 20000 or end[5] > 4 * MIB:
                raise ShelfError("This ZIP has an invalid or oversized file list.")
            with zipfile.ZipFile(raw) as archive:
                files, size = game_files(archive)
                free_space(stage, size + len(files) * 4096)
                game = stage / "ready"
                game.mkdir()
                done = 0
                for info, name in files:
                    path = game / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    with archive.open(info) as source, path.open("xb") as sink:
                        while True:
                            chunk = source.read(CHUNK)
                            if not chunk:
                                break
                            sink.write(chunk)
                            done += len(chunk)
                            yield "Installing", done, size
        if os.path.lexists(dest):
            raise ShelfError("This game already has a library folder.")
        (game / receipt).touch(exist_ok=False)
        game.rename(dest)
    return dest.name


def error_message(exc):
    if isinstance(exc, ShelfError):
        return str(exc)
    if isinstance(exc, HTTPError):
        return "itch.io returned HTTP %d. Check the creator’s page or retry." % exc.code
    if isinstance(exc, (URLError, TimeoutError, ConnectionError)):
        return "The store connection failed. Check your connection and retry."
    if isinstance(exc, (zipfile.BadZipFile, NotImplementedError)):
        return "This ZIP is damaged or uses an unsupported format."
    if isinstance(exc, OSError):
        return "Could not read or write the download. Check storage and retry."
    return "Could not read the creator’s download information."


class Browser:
    """Transient screen state. Network sessions and partial downloads live only while the shelf is open."""
    per_page = 4

    def __init__(self, basedir):
        self.basedir = basedir
        self.rows = []
        self.files = []
        self.project = None
        self.title = ""
        self.page = 0
        self.mode = "home"
        self.status = ""
        self.job = None

    def refresh(self):
        try:
            self.rows = catalogue()
            self.page = 0
            self.mode = "games"
            self.status = ""
        except Exception as exc:
            self.status = error_message(exc)

    def select(self, title, url):
        self.title = title
        self.project = Project(url)
        self.files = []
        self.mode = "project"
        self.status = ""
        self.file_page = 0

    def list_files(self):
        try:
            self.files = self.project.uploads()
            self.status = "Choose a PC or Linux ZIP from this creator."
        except Exception as exc:
            self.status = error_message(exc)

    def choose(self, name, upload):
        self.name, self.upload = name, upload
        self.mode = "confirm"
        self.status = ""

    def begin(self):
        self.job = install(self.project, self.upload, self.basedir)
        self.status = "Contacting itch.io…"
        self.mode = "download"

    def tick(self):
        if self.job is None:
            return
        try:
            # Limit UI work per frame. Network reads have their own socket timeout.
            for _ in range(4):
                phase, done, total = next(self.job)
            self.status = "%s  %.1f MiB" % (phase, done / MIB)
            if total:
                self.status += " / %.1f MiB" % (total / MIB)
        except StopIteration as finished:
            self.job = None
            self.mode = "installed"
            self.status = finished.value
        except Exception as exc:
            self.job = None
            self.mode = "project"
            self.status = error_message(exc)

    def cancel(self):
        if self.job is not None:
            self.job.close()
            self.job = None
        self.mode = "project"
        self.status = "Download cancelled."
