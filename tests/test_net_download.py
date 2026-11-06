#!/usr/bin/env python3
# Dulcamara - the update's download (src/net.c's net_download), built and run on the host against a local server.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds tests/net_download_check.c with src/net.c and the host's libcurl (CC names the compiler, default clang),
with a stall limit of 3 seconds in place of NET_STALL_SECONDS, and runs it against a server on 127.0.0.1:

* a download slower than 25 seconds all told completes, as it must: until 0.2.13 net_download gave up on any download
  after 25 seconds, and every update slower than 3 MB/s (its PPSA99010.zip is over 70 MB) ended in "The download
  failed";
* GitHub's redirect from the release's address to the file is followed;
* a download that stops arriving is given up after the stall limit, with the reason, and leaves no file behind;
* a 404 is NET_NOT_FOUND, with the reason.
"""

import http.server
import os
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", "-D_POSIX_C_SOURCE=200809L"]
STALL_SECONDS = 3
SLOW_SIZE = 3_670_016           # 3.5 MiB ...
SLOW_RATE = 131_072             # ... at 128 KiB a second: 28 seconds, longer than the old 25-second limit
SMALL = bytes(range(256)) * 4096  # 1 MiB


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *arguments):
        pass

    def send_body(self, size, rate=None, stall_after=None):
        self.send_response(200)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        sent, chunk = 0, (rate // 10) if rate else 65536
        while sent < size:
            if stall_after is not None and sent >= stall_after:
                time.sleep(60)  # nothing more arrives; the client gives up first
                return
            n = min(chunk, size - sent)
            self.wfile.write(b"z" * n)
            self.wfile.flush()
            sent += n
            if rate:
                time.sleep(0.1)

    def do_GET(self):
        if self.path == "/slow/PPSA99010.zip":
            self.send_body(SLOW_SIZE, rate=SLOW_RATE)
        elif self.path == "/releases/download/0.2.13/PPSA99010.zip":
            self.send_response(302)
            self.send_header("Location", "/assets/PPSA99010.zip")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/assets/PPSA99010.zip":
            self.send_response(200)
            self.send_header("Content-Length", str(len(SMALL)))
            self.end_headers()
            self.wfile.write(SMALL)
        elif self.path == "/stall/PPSA99010.zip":
            self.send_body(4 * 1024 * 1024, stall_after=262_144)
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()


class NetDownloadTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = os.environ.get("CC", "clang")
        if not shutil.which(cc):
            raise unittest.SkipTest(f"needs {cc}")
        cls.directory = tempfile.TemporaryDirectory()
        cls.executable = Path(cls.directory.name) / "net-download-check"
        build = subprocess.run([cc, *FLAGS, "-DNP_HAVE_CURL", f"-DNET_STALL_SECONDS={STALL_SECONDS}",
                                "-I" + str(ROOT / "src"), str(ROOT / "tests/net_download_check.c"),
                                str(ROOT / "src/net.c"), "-lcurl", "-o", str(cls.executable)],
                               capture_output=True, text=True, check=False)
        if build.returncode != 0 and "curl/curl.h" in build.stderr:
            cls.directory.cleanup()
            raise unittest.SkipTest("needs libcurl's headers (libcurl4-openssl-dev)")
        if build.returncode != 0:
            cls.directory.cleanup()
            raise AssertionError(build.stderr)
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server.daemon_threads = True
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.directory.cleanup()

    def start(self, path, name):
        dest = Path(self.directory.name) / name
        process = subprocess.Popen([str(self.executable), self.base + path, str(dest)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        return process, dest

    @staticmethod
    def finish(process, timeout):
        out, err = process.communicate(timeout=timeout)
        line = [l for l in out.splitlines() if l.startswith("result ")]
        assert line, out + err
        words = line[-1].split(" ", 9)
        fields = {"result": int(words[1]), "seconds": float(words[3]), "progress": words[5],
                  "calls": int(words[7]), "why": words[9] if len(words) > 9 else "", "out": out + err[-3000:]}
        return process.returncode, fields

    def test_downloads(self):
        # The slow one runs while the others are checked: together they take its 28 seconds.
        slow, slow_dest = self.start("/slow/PPSA99010.zip", "slow.zip")

        process, dest = self.start("/releases/download/0.2.13/PPSA99010.zip", "redirected.zip")
        code, fields = self.finish(process, 30)
        self.assertEqual(code, 0, fields["out"])
        self.assertEqual(dest.read_bytes(), SMALL)
        self.assertEqual(fields["progress"], f"{len(SMALL)}/{len(SMALL)}", fields["out"])

        process, dest = self.start("/stall/PPSA99010.zip", "stalled.zip")
        code, fields = self.finish(process, 60)
        self.assertEqual(fields["result"], 2, fields["out"])  # NET_UNAVAILABLE
        self.assertLess(fields["seconds"], STALL_SECONDS + 10, fields["out"])
        self.assertEqual(fields["why"], f"nothing arrived for {STALL_SECONDS} seconds", fields["out"])
        self.assertIn("curl 28", fields["out"])
        self.assertFalse(dest.exists())
        self.assertFalse(Path(str(dest) + ".part").exists())

        process, dest = self.start("/releases/download/0.2.13/missing.zip", "missing.zip")
        code, fields = self.finish(process, 30)
        self.assertEqual(fields["result"], 1, fields["out"])  # NET_NOT_FOUND
        self.assertEqual(fields["why"], "the server answered 404", fields["out"])
        self.assertFalse(dest.exists())

        code, fields = self.finish(slow, 120)
        print(f"slow download: {fields['seconds']:.1f} s, {fields['calls']} progress calls")
        self.assertEqual(code, 0, fields["out"])
        self.assertGreater(fields["seconds"], 25.0, fields["out"])
        self.assertEqual(slow_dest.stat().st_size, SLOW_SIZE)
        self.assertEqual(fields["progress"], f"{SLOW_SIZE}/{SLOW_SIZE}", fields["out"])
        self.assertGreater(fields["calls"], 10, fields["out"])

    def test_no_limit_on_the_whole_download(self):
        source = (ROOT / "src/net.c").read_text(encoding="utf-8")
        download = source[source.index("NetResult net_download("):source.index("int net_request(")]
        self.assertNotIn("CURLOPT_TIMEOUT", download)
        self.assertIn("CURLOPT_LOW_SPEED_TIME, (long)NET_STALL_SECONDS", download)


if __name__ == "__main__":
    unittest.main()
