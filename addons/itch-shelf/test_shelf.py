# SPDX-License-Identifier: GPL-3.0-or-later
import io
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent / "Dulcamara itch.io Shelf/game/python-packages"))
import dulcamara_shelf as shelf


class File(io.BytesIO):
    @property
    def headers(self):
        return {"Content-Length": str(len(self.getbuffer()))}


class TestProject:
    page = "https://creator.itch.io/a-game"
    files = [("PC.zip", "42")]

    def __init__(self, entries):
        with io.BytesIO() as stream:
            with zipfile.ZipFile(stream, "w") as archive:
                for name, data in entries:
                    archive.writestr(name, data)
            self.data = stream.getvalue()

    def archive(self, upload):
        return File(self.data)


class InstallTests(unittest.TestCase):
    def setUp(self):
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        self.shelf = Path(root.name) / "Shelf"
        self.shelf.mkdir()
        self.dest = self.shelf.parent / "a-game (creator)"
        self.entries = [("Novel/game/script.rpy", b"label start:\n    return"),
                        ("Novel/renpy/vc_version.py", b"version='7.8.7'"), ("Novel/novel.exe", b"PC")]
        self.project = TestProject(self.entries)

    def install(self, project=None):
        return shelf.install(project or self.project, "42", self.shelf)

    def test_store_reports_which_connection_step_failed(self):
        for cause, detail in ((shelf.socket.gaierror(-2, "lookup"), "DNS"),
                              (shelf.ssl.SSLCertVerificationError(1, "x509"), "certificate"),
                              (TimeoutError(), "too long")):
            with self.subTest(cause=cause):
                self.assertIn(detail, shelf.error_message(shelf.URLError(cause)))

    def test_install_and_duplicate_preserves_existing_data(self):
        list(self.install())
        self.assertTrue((self.dest / "game/script.rpy").is_file())
        self.assertTrue((self.dest / "renpy/vc_version.py").is_file())
        self.assertFalse((self.dest / "novel.exe").exists())
        (self.dest / "save").write_text("keep")
        with self.assertRaises(shelf.ShelfError):
            list(self.install())
        self.assertEqual((self.dest / "save").read_text(), "keep")

    def test_cancel_unlocks_and_removes_partial_files(self):
        job = self.install()
        self.assertEqual(next(job)[0], "Downloading")
        with self.assertRaises(shelf.ShelfError):
            list(self.install())
        job.close()
        self.assertFalse(self.dest.exists())
        self.assertFalse((self.shelf / ".itch-install").exists())
        list(self.install())

    def test_invalid_game_and_path_are_never_published(self):
        for extra in ([("Novel/game/../outside.rpy", b"bad")], [("Other/game/script.rpy", b"two")],
                      [("Novel/game/SCRIPT.rpy", b"collision")], [("Novel/game/C:x", b"bad")]):
            with self.subTest(extra=extra), self.assertRaises(shelf.ShelfError):
                list(self.install(TestProject(self.entries + extra)))
            self.assertFalse(self.dest.exists())
        with self.assertRaises(shelf.ShelfError):
            list(self.install(TestProject([("a.exe", b"no game")])))
        self.assertFalse((self.shelf / ".itch-install").exists())

    def test_limits_and_interrupted_download(self):
        with patch.object(shelf, "MAX_GAME", 4), self.assertRaises(shelf.ShelfError):
            list(self.install())
        self.project.data = self.project.data[:35]
        with self.assertRaises(shelf.ShelfError):
            list(self.install())
        self.assertFalse(self.dest.exists())

    def test_download_buttons_belong_to_their_own_upload(self):
        html = b'''<meta name="csrf_token" value="test"><div class="upload">
            <a class="button download_btn" data-upload_id="7"></a><strong class="name" title="pc.zip"></strong></div>
            <div class="upload"><strong class="name" title="bonus.zip"></strong></div>
            <div class="upload"><a class="download_btn" data-upload_id="8"></a>
            <strong class="name" title="build.apk"></strong></div>'''
        page = shelf.StorePage(html)
        self.assertEqual(page.files, [("pc.zip", "7")])
        self.assertFalse(page.free_button)

    def test_platform_label_with_no_zip_extension_stays_in_its_own_row(self):
        html = b'''<div class="upload"><a class="download_btn" data-upload_id="7"></a>
            <div><strong class="name" title="CoR 0.113 PC"></strong><span class="icon icon-tux"></span>
            <span class="icon icon-windows8"></span></div></div>
            <div class="upload"><a class="download_btn" data-upload_id="8"></a>
            <strong class="name" title="Android downloader"></strong></div><span class="icon icon-tux"></span>'''
        self.assertEqual(shelf.StorePage(html).files, [("CoR 0.113 PC", "7")])

    def test_chapters_install_side_by_side_and_retries_preserve_both(self):
        self.project.files = [("0.14", "42"), ("Epilogue", "43")]
        list(self.install())
        first = self.dest / "game/script.rpy"
        first.write_text("saved original")
        list(shelf.install(self.project, "43", self.shelf))
        second = self.dest.with_name(self.dest.name + " [43]")
        self.assertTrue((second / "game/script.rpy").is_file())
        for upload in ("42", "43"):
            with self.assertRaises(shelf.ShelfError):
                list(shelf.install(self.project, upload, self.shelf))
        self.assertEqual(first.read_text(), "saved original")

    def test_rejects_non_zip_payload_behind_desktop_label(self):
        self.project.data = b"MZ" + b"0" * 100
        with self.assertRaisesRegex(shelf.ShelfError, "not a game ZIP"):
            list(self.install())
        self.assertFalse(self.dest.exists())

    def test_only_creator_pages_and_store_download_hosts(self):
        self.assertEqual(shelf.project_parts(self.project.page), ("creator", "a-game"))
        for url in ("https://creator.itch.io.example/game", "https://user@creator.itch.io/game",
                    "http://creator.itch.io/game", "https://creator.itch.io/game?key=1"):
            self.assertIsNone(shelf.project_parts(url))
        self.assertFalse(shelf.cdn("https://evil.example/file"))
        self.assertFalse(shelf.cdn("http://cdn.itch.zone/file"))
        self.assertTrue(shelf.cdn("https://cdn.itch.zone/file"))


if __name__ == "__main__":
    unittest.main()
